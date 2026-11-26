
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_check.h"
#include "bootloader_random.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#define ID_SIZE 97
#define NONCE_SIZE 33
#define HASH_SIZE 65
#define SECRET_BYTES 32
#define NETWORK_READY BIT0
#define HTTP_PORT 52410
#define DISCOVERY_PORT 52404
#define URGENT_BOOT_MS 45000

typedef struct {
    char magic[8];
    char project[ID_SIZE];
    char build[ID_SIZE];
} app_identity_t;

const __attribute__((section(".rodata_custom_desc"))) app_identity_t sdp_app_desc = {
    "SDPOTA1", SDP_PROJECT_ID, SDP_BUILD_ID
};

// Wi-Fi settings only. The pairing secret lives in the identity namespace and never changes.
typedef struct {
    char mode[4];
    char ssid[33];
    char password[65];
} provision_t;

typedef struct {
    char nonce[NONCE_SIZE];
    char from[ID_SIZE];
    char to[ID_SIZE];
} attempt_t;

static provision_t config;
static attempt_t attempt;
static nvs_handle_t identity_store, ota_store;
static uint8_t secret[SECRET_BYTES];
static bool paired;
static char device_id[17], boot_id[NONCE_SIZE], challenge[NONCE_SIZE];
static _Atomic(const char *) boot_state = "confirmed";
static EventGroupHandle_t network_events;
static _Atomic(esp_netif_t *) wifi_netif;

static void to_hex(const uint8_t *bytes, size_t size, char *text)
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        text[2 * i] = alphabet[bytes[i] >> 4];
        text[2 * i + 1] = alphabet[bytes[i] & 15];
    }
    text[2 * size] = '\0';
}

static bool is_hex(const char *text, size_t size)
{
    return strlen(text) == size && strspn(text, "0123456789abcdef") == size;
}

static uint8_t hex_digit(char c)
{
    return c <= '9' ? c - '0' : c - 'a' + 10;
}

static void random_nonce(char text[NONCE_SIZE])
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    to_hex(raw, sizeof(raw), text);
}

static bool authenticate(const char *message, const char *provided)
{
    uint8_t raw[32];
    char expected[HASH_SIZE];
    if (!is_hex(provided, HASH_SIZE - 1) ||
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret, sizeof(secret),
                        (const uint8_t *)message, strlen(message), raw)) {
        return false;
    }
    to_hex(raw, sizeof(raw), expected);
    return mbedtls_ct_memcmp(expected, provided, HASH_SIZE - 1) == 0;
}

static esp_err_t signed_response(httpd_req_t *req, const char *nonce, int code, const char *body)
{
    char message[768], auth[HASH_SIZE];
    uint8_t raw[32];
    int count = snprintf(message, sizeof(message), "response\n%s\n%d\n%s", nonce, code, body);
    if (count < 0 || count >= sizeof(message) ||
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret, sizeof(secret),
                        (const uint8_t *)message, count, raw)) {
        return ESP_FAIL;
    }
    to_hex(raw, sizeof(raw), auth);
    httpd_resp_set_status(req, code == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-SDP-Auth", auth);
    return httpd_resp_sendstr(req, body);
}

static bool valid_id(const char *id)
{
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = strlen(id);
    return len && len < ID_SIZE && strchr(alnum, id[0]) &&
        strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_-") == len;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char nonce[NONCE_SIZE], auth[HASH_SIZE], message[64], body[512];
    if (httpd_req_get_hdr_value_str(req, "X-SDP-Nonce", nonce, sizeof(nonce)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-Auth", auth, sizeof(auth)) != ESP_OK ||
        !is_hex(nonce, NONCE_SIZE - 1)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Authentication required");
    }
    snprintf(message, sizeof(message), "status\n%s", nonce);
    if (!authenticate(message, auth)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Authentication required");
    }
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"projectId\":\"%s\",\"runningBuildId\":\"%s\","
             "\"bootId\":\"%s\",\"bootState\":\"%s\",\"challenge\":\"%s\",\"lastAttempt\":\"%s\"}",
             device_id, SDP_PROJECT_ID, SDP_BUILD_ID, boot_id, boot_state, challenge, attempt.nonce);
    return signed_response(req, nonce, 200, body);
}

static bool read_image(httpd_req_t *req, uint8_t *buffer, size_t size)
{
    while (size) {
        int got = httpd_req_recv(req, (char *)buffer, size);
        if (got <= 0) {
            return false;
        }
        buffer += got;
        size -= got;
    }
    return true;
}

enum { DESCRIPTION_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                            sizeof(esp_app_desc_t) };

static bool matches_identity(const app_identity_t *identity, const char *build)
{
    return memcmp(identity->magic, "SDPOTA1", sizeof(identity->magic)) == 0 &&
        identity->project[ID_SIZE - 1] == '\0' && identity->build[ID_SIZE - 1] == '\0' &&
        strcmp(identity->project, SDP_PROJECT_ID) == 0 && strcmp(identity->build, build) == 0;
}

static bool stage_image(httpd_req_t *req, const esp_partition_t *partition, const char *build,
                        const char *expected_hash)
{
    uint8_t buffer[2048], digest[32];
    char actual_hash[HASH_SIZE];
    const size_t prefix_size = DESCRIPTION_OFFSET + sizeof(app_identity_t);
    if (!read_image(req, buffer, prefix_size)) {
        return false;
    }
    app_identity_t identity;
    memcpy(&identity, buffer + DESCRIPTION_OFFSET, sizeof(identity));
    if (!matches_identity(&identity, build)) {
        return false;
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool okay = mbedtls_sha256_starts(&sha, 0) == 0;
    size_t remaining = req->content_len, chunk = prefix_size;
    while (okay) {
        okay = mbedtls_sha256_update(&sha, buffer, chunk) == 0 &&
           esp_ota_write(handle, buffer, chunk) == ESP_OK;
        remaining -= chunk;
        if (!okay || !remaining) {
            break;
        }
        chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        okay = read_image(req, buffer, chunk);
    }
    okay = okay && mbedtls_sha256_finish(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    if (okay) {
        to_hex(digest, sizeof(digest), actual_hash);
        okay = mbedtls_ct_memcmp(actual_hash, expected_hash, HASH_SIZE - 1) == 0;
    }
    if (!okay) {
        esp_ota_abort(handle);
        return false;
    }
    // IDF verifies the ESP image and its signature using the current app's trust key.
    return esp_ota_end(handle) == ESP_OK;
}

static esp_err_t save_attempt(const attempt_t *record)
{
    esp_err_t result = nvs_set_blob(ota_store, "attempt", record, sizeof(*record));
    return result == ESP_OK ? nvs_commit(ota_store) : result;
}

static esp_err_t update_handler(httpd_req_t *req)
{
    char nonce[NONCE_SIZE], auth[HASH_SIZE], target[17], project[ID_SIZE], build[ID_SIZE];
    char sha[HASH_SIZE], message[384];
    if (httpd_req_get_hdr_value_str(req, "X-SDP-Nonce", nonce, sizeof(nonce)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-Auth", auth, sizeof(auth)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-Device", target, sizeof(target)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-Project", project, sizeof(project)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-Build", build, sizeof(build)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-SDP-SHA256", sha, sizeof(sha)) != ESP_OK ||
        strcmp(nonce, challenge) || !is_hex(target, 16) || !valid_id(project) || !valid_id(build) ||
        !is_hex(sha, HASH_SIZE - 1)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid update request");
    }
    int length = snprintf(message, sizeof(message), "ota\n%s\n%s\n%s\n%s\n%s\n%zu",
                          nonce, target, project, build, sha, req->content_len);
    if (length < 0 || length >= sizeof(message) || !authenticate(message, auth)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid update request");
    }
    random_nonce(challenge);
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (strcmp(target, device_id) || strcmp(project, SDP_PROJECT_ID) ||
        !strcmp(build, SDP_BUILD_ID) || !strcmp(boot_state, "pending") || !partition ||
        req->content_len < DESCRIPTION_OFFSET + sizeof(app_identity_t) || req->content_len > partition->size) {
        return signed_response(req, nonce, 400, "{\"error\":\"target_or_image_size\"}");
    }
    if (!stage_image(req, partition, build, sha)) {
        return signed_response(req, nonce, 400, "{\"error\":\"image_rejected\"}");
    }
    attempt_t pending = {0};
    strcpy(pending.nonce, nonce);
    strcpy(pending.from, SDP_BUILD_ID);
    strcpy(pending.to, build);
    if (save_attempt(&pending) != ESP_OK) {
        return signed_response(req, nonce, 400, "{\"error\":\"attempt_storage\"}");
    }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        save_attempt(&attempt);
        return signed_response(req, nonce, 400, "{\"error\":\"boot_partition\"}");
    }
    signed_response(req, nonce, 200, "{\"status\":\"staged\"}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_ota_http(void)
{
    httpd_config_t options = HTTPD_DEFAULT_CONFIG();
    options.server_port = HTTP_PORT;
    options.stack_size = 8192;
    options.recv_wait_timeout = 5;
    options.lru_purge_enable = true;
    httpd_handle_t server;
    esp_err_t result = httpd_start(&server, &options);
    if (result != ESP_OK) {
        return result;
    }
    const httpd_uri_t status = {.uri = "/v1/status", .method = HTTP_GET, .handler = status_handler};
    const httpd_uri_t update = {.uri = "/v1/ota", .method = HTTP_POST, .handler = update_handler};
    result = httpd_register_uri_handler(server, &status);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &update);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
    }
    return result;
}

static void on_network(void *arg, esp_event_base_t event, int32_t id, void *data)
{
    if ((event == IP_EVENT && id == IP_EVENT_STA_GOT_IP) ||
        (event == WIFI_EVENT && id == WIFI_EVENT_AP_START)) {
        xEventGroupSetBits(network_events, NETWORK_READY);
    } else if (event == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(network_events, NETWORK_READY);
        esp_wifi_connect();
    }
}

static esp_err_t start_wifi(void)
{
    const bool ap = !strcmp(config.mode, "ap");
    network_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(network_events, ESP_ERR_NO_MEM, "sdp-ota", "Network event group");
    ESP_RETURN_ON_ERROR(esp_netif_init(), "sdp-ota", "Network init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "sdp-ota", "Event loop");
    esp_netif_t *netif = ap ? esp_netif_create_default_wifi_ap() : esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, "sdp-ota", "Network interface");
    wifi_netif = netif;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), "sdp-ota", "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "sdp-ota", "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_network, NULL),
                        "sdp-ota", "Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_network, NULL),
                        "sdp-ota", "IP event handler");
    wifi_config_t settings = {0};
    if (ap) {
        memcpy(settings.ap.ssid, config.ssid, strlen(config.ssid));
        settings.ap.ssid_len = strlen(config.ssid);
        memcpy(settings.ap.password, config.password, strlen(config.password));
        settings.ap.authmode = WIFI_AUTH_WPA2_PSK;
        settings.ap.max_connection = 2;
        settings.ap.channel = 1;
    } else {
        memcpy(settings.sta.ssid, config.ssid, strlen(config.ssid));
        memcpy(settings.sta.password, config.password, strlen(config.password));
        settings.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        settings.sta.pmf_cfg.capable = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(ap ? WIFI_MODE_AP : WIFI_MODE_STA), "sdp-ota", "Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(ap ? WIFI_IF_AP : WIFI_IF_STA, &settings), "sdp-ota", "Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "sdp-ota", "Wi-Fi start");
    return ESP_OK;
}

// Answers {"probe":"synapta-sdp"} on UDP 52404 so hosts can list boards without typing an address.
// The reply holds only identity and pairing state; status and builds stay behind the HMAC endpoint.
static void discovery_task(void *unused)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        vTaskDelete(NULL);
    }
    char probe[64], beacon[256];
    int length = snprintf(beacon, sizeof(beacon), "{\"protocol\":\"sdp/v1\",\"deviceId\":\"%s\","
                          "\"projectId\":\"%s\",\"paired\":%s,\"port\":%d}",
                          device_id, SDP_PROJECT_ID, paired ? "true" : "false", HTTP_PORT);
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_size = sizeof(from);
        int got = recvfrom(sock, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&from, &from_size);
        if (got <= 0) {
            continue;
        }
        probe[got] = '\0';
        if (!strcmp(probe, "{\"probe\":\"synapta-sdp\"}")) {
            sendto(sock, beacon, length, 0, (struct sockaddr *)&from, from_size);
        }
    }
}

static void usb_reply(const char *text)
{
    uart_write_bytes(UART_NUM_0, text, strlen(text));
}

// Stores Wi-Fi settings from JSON. Fields are optional for SDP-PROVISION (pair only, keep the network).
static bool save_wifi(const char *json, bool required)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "mode"));
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ssid"));
    const char *pwd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "password"));
    bool okay = root != NULL;
    if (okay && (required || ssid)) {
        provision_t next = {0};
        okay = mode && (!strcmp(mode, "ap") || !strcmp(mode, "sta")) &&
            ssid && strlen(ssid) && strlen(ssid) <= 32 &&
            pwd && strlen(pwd) >= 8 && strlen(pwd) <= 63;
        if (okay) {
            strcpy(next.mode, mode);
            strcpy(next.ssid, ssid);
            strcpy(next.password, pwd);
            okay = nvs_set_blob(ota_store, "wifi", &next, sizeof(next)) == ESP_OK && nvs_commit(ota_store) == ESP_OK;
        }
    }
    cJSON_Delete(root);
    return okay;
}

// Physical USB access is the trust boundary: pairing hands the board's own secret to the host.
static bool pair_over_usb(void)
{
    paired = true;
    return nvs_set_u8(identity_store, "paired", 1) == ESP_OK && nvs_commit(identity_store) == ESP_OK;
}

static void usb_task(void *unused)
{
    char line[768];
    size_t size = 0;
    bool overflow = false;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(1000)) != 1 || byte == '\r') {
            continue;
        }
        if (byte != '\n') {
            if (size < sizeof(line) - 1) {
                line[size++] = byte;
            } else {
                overflow = true;
            }
            continue;
        }
        line[size] = '\0';
        if (!overflow && !strcmp(line, "SDP-INFO")) {
            esp_netif_ip_info_t ip = {0};
            esp_netif_t *netif = wifi_netif;
            if (netif) {
                esp_netif_get_ip_info(netif, &ip);
            }
            char out[352];
            snprintf(out, sizeof(out), "SDP-USB {\"deviceId\":\"%s\",\"projectId\":\"%s\","
                     "\"runningBuildId\":\"%s\",\"bootState\":\"%s\",\"ipv4\":\"" IPSTR "\",\"paired\":%s}\n",
                     device_id, SDP_PROJECT_ID, SDP_BUILD_ID, boot_state, IP2STR(&ip.ip),
                     paired ? "true" : "false");
            usb_reply(out);
        } else if (!overflow && (!strncmp(line, "SDP-PROVISION ", 14) || !strncmp(line, "SDP-WIFI ", 9))) {
            // SDP-WIFI changes the network only; SDP-PROVISION also pairs and returns the secret.
            const bool wifi_only = !strncmp(line, "SDP-WIFI ", 9);
            if (save_wifi(line + (wifi_only ? 9 : 14), wifi_only) && (wifi_only || pair_over_usb())) {
                char out[192], hex[2 * SECRET_BYTES + 1];
                to_hex(secret, sizeof(secret), hex);
                if (wifi_only) {
                    snprintf(out, sizeof(out), "SDP-USB {\"provisioned\":true,\"deviceId\":\"%s\"}\n", device_id);
                } else {
                    snprintf(out, sizeof(out), "SDP-USB {\"provisioned\":true,\"deviceId\":\"%s\",\"secret\":\"%s\"}\n",
                             device_id, hex);
                }
                usb_reply(out);
                memset(out, 0, sizeof(out));
                memset(hex, 0, sizeof(hex));
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(300));
                esp_restart();
            }
            usb_reply("SDP-USB {\"provisioned\":false}\n");
        }
        memset(line, 0, size);
        size = 0;
        overflow = false;
    }
}

// Hardware fingerprint: SHA-256 of the factory MAC and the SPI flash unique ID, whichever can be read.
// Cloned or damaged boards may have a blank, duplicated or unreadable MAC, so neither source is required.
// Returns false when neither can be read.
static bool hardware_fingerprint(uint8_t digest[32])
{
    uint8_t mac[6];
    uint64_t flash_id = 0;
    const bool has_mac = esp_efuse_mac_get_default(mac) == ESP_OK;
    const bool has_flash = esp_flash_read_unique_chip_id(esp_flash_default_chip, &flash_id) == ESP_OK && flash_id != 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool okay = (has_mac || has_flash) && mbedtls_sha256_starts(&sha, 0) == 0 &&
        mbedtls_sha256_update(&sha, (const uint8_t *)"SDP-HW-v1", 9) == 0 &&
        (!has_mac || mbedtls_sha256_update(&sha, mac, sizeof(mac)) == 0) &&
        (!has_flash || mbedtls_sha256_update(&sha, (const uint8_t *)&flash_id, sizeof(flash_id)) == 0) &&
        mbedtls_sha256_finish(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    return okay;
}

// A fresh secret: SHA-256 over 64 bytes of the hardware RNG (with its entropy source switched on, since
// Wi-Fi is not running yet), the hardware fingerprint and boot timing. Any one good source is enough.
static void new_secret(const uint8_t fingerprint[32])
{
    uint8_t noise[64];
    bootloader_random_enable();
    esp_fill_random(noise, sizeof(noise));
    bootloader_random_disable();
    const int64_t now = esp_timer_get_time();
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool okay = mbedtls_sha256_starts(&sha, 0) == 0 && mbedtls_sha256_update(&sha, noise, sizeof(noise)) == 0 &&
        mbedtls_sha256_update(&sha, fingerprint, 32) == 0 &&
        mbedtls_sha256_update(&sha, (const uint8_t *)&now, sizeof(now)) == 0 &&
        mbedtls_sha256_finish(&sha, secret) == 0;
    mbedtls_sha256_free(&sha);
    memset(noise, 0, sizeof(noise));
    ESP_ERROR_CHECK(okay ? ESP_OK : ESP_FAIL);
}

// The secret lives in NVS until the flash is erased; deviceId = first 8 bytes of SHA-256("SDP-ID-v2" || secret).
// The ID needs no hardware value, so a broken MAC cannot stop the board from booting or make IDs collide.
// The stored hardware fingerprint only detects a flash image copied onto another chip: then the copy
// draws its own secret (new ID, unpaired) instead of impersonating the original board.
static void load_identity(void)
{
    uint8_t fingerprint[32] = {0}, stored[32];
    const bool has_fingerprint = hardware_fingerprint(fingerprint);
    size_t size = sizeof(secret);
    esp_err_t found = nvs_get_blob(identity_store, "secret", secret, &size);
    size_t stored_size = sizeof(stored);
    const bool cloned = found == ESP_OK && has_fingerprint &&
        nvs_get_blob(identity_store, "hardware", stored, &stored_size) == ESP_OK && stored_size == sizeof(stored) &&
        memcmp(stored, fingerprint, sizeof(stored)) != 0;
    if (found == ESP_ERR_NVS_NOT_FOUND || found == ESP_ERR_NVS_INVALID_LENGTH || cloned ||
        (found == ESP_OK && size != sizeof(secret))) {
        new_secret(fingerprint);
        ESP_ERROR_CHECK(nvs_set_blob(identity_store, "secret", secret, sizeof(secret)));
        esp_err_t erased = nvs_erase_key(identity_store, "paired");
        ESP_ERROR_CHECK(erased == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : erased);
    } else {
        ESP_ERROR_CHECK(found);
    }
    if (has_fingerprint) {
        ESP_ERROR_CHECK(nvs_set_blob(identity_store, "hardware", fingerprint, sizeof(fingerprint)));
    }
    ESP_ERROR_CHECK(nvs_commit(identity_store));

    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool hashed = mbedtls_sha256_starts(&sha, 0) == 0 &&
        mbedtls_sha256_update(&sha, (const uint8_t *)"SDP-ID-v2", 9) == 0 &&
        mbedtls_sha256_update(&sha, secret, sizeof(secret)) == 0 &&
        mbedtls_sha256_finish(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    ESP_ERROR_CHECK(hashed ? ESP_OK : ESP_FAIL);
    to_hex(digest, 8, device_id);
    uint8_t flag = 0;
    paired = nvs_get_u8(identity_store, "paired", &flag) == ESP_OK && flag == 1;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("sdp", NVS_READWRITE, &identity_store));
    ESP_ERROR_CHECK(nvs_open("sdp_ota", NVS_READWRITE, &ota_store));
    load_identity();
    random_nonce(boot_id);
    random_nonce(challenge);
    size_t size = sizeof(attempt);
    if (nvs_get_blob(ota_store, "attempt", &attempt, &size) != ESP_OK || size != sizeof(attempt)) {
        memset(&attempt, 0, sizeof(attempt));
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    bool pending = esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
                   ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending) {
        boot_state = "pending";
    } else if (attempt.nonce[0] && !strcmp(attempt.from, SDP_BUILD_ID)) {
        const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
        app_identity_t identity;
        if (invalid && esp_partition_read(invalid, DESCRIPTION_OFFSET, &identity, sizeof(identity)) == ESP_OK &&
            matches_identity(&identity, attempt.to)) {
            boot_state = "rolled_back";
        }
    }
    size = sizeof(config);
    bool configured = nvs_get_blob(ota_store, "wifi", &config, &size) == ESP_OK && size == sizeof(config);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));
    configASSERT(xTaskCreate(usb_task, "sdp_usb", 4096, NULL, 5, NULL) == pdPASS);
    bool healthy = configured && start_wifi() == ESP_OK;
    if (healthy) {
        configASSERT(xTaskCreate(discovery_task, "sdp_discovery", 3072, NULL, 4, NULL) == pdPASS);
    }
    if (healthy && pending) {
        healthy = (xEventGroupWaitBits(network_events, NETWORK_READY, pdFALSE, pdFALSE,
                                     pdMS_TO_TICKS(URGENT_BOOT_MS)) & NETWORK_READY) != 0;
    }
#ifdef SDP_OTA_TEST_REJECT
    if (pending) {
        healthy = false;
    }
#endif
    healthy = healthy && paired;
    if (healthy) {
        // Register the service before confirming so a candidate can fail back if OTA cannot start.
        healthy = start_ota_http() == ESP_OK;
    }
    if (pending && healthy && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        boot_state = "confirmed";
    } else if (pending) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
}


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
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#define ID_SIZE 97
#define NONCE_SIZE 33
#define HASH_SIZE 65
#define KEY_BYTES 16
#define NETWORK_READY BIT0
#define HTTP_PORT 52410
#define URGENT_BOOT_MS 45000

typedef struct {
    char magic[8];
    char project[ID_SIZE];
    char build[ID_SIZE];
} app_identity_t;

const __attribute__((section(".rodata_custom_desc"))) app_identity_t sdp_app_desc = {
    "SDPOTA1", SDP_PROJECT_ID, SDP_BUILD_ID
};

typedef struct {
    uint8_t key[KEY_BYTES];
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
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), config.key, sizeof(config.key),
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
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), config.key, sizeof(config.key),
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

static void usb_reply(const char *text)
{
    uart_write_bytes(UART_NUM_0, text, strlen(text));
}

static bool has_pairing_key(const provision_t *settings)
{
    const uint8_t empty[KEY_BYTES] = {0};
    return memcmp(settings->key, empty, sizeof(empty)) != 0;
}

static bool provision(const char *json, bool wifi_only)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "mode"));
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ssid"));
    const char *pwd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "password"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "key"));
    provision_t next = {0};
    bool okay = mode && (!strcmp(mode, "ap") || !strcmp(mode, "sta")) &&
        ssid && strlen(ssid) && strlen(ssid) <= 32 &&
        pwd && strlen(pwd) >= 8 && strlen(pwd) <= 63 &&
        (wifi_only ? !cJSON_HasObjectItem(root, "key") : key && is_hex(key, KEY_BYTES * 2));
    if (okay) {
        strcpy(next.mode, mode);
        strcpy(next.ssid, ssid);
        strcpy(next.password, pwd);
        if (wifi_only) {
            provision_t previous;
            size_t size = sizeof(previous);
            esp_err_t loaded = nvs_get_blob(ota_store, "network", &previous, &size);
            if (loaded == ESP_OK && size == sizeof(previous)) {
                memcpy(next.key, previous.key, sizeof(next.key));
            } else if (loaded != ESP_ERR_NVS_NOT_FOUND) {
                okay = false;
            }
        } else {
            for (size_t i = 0; i < sizeof(next.key); ++i) {
                next.key[i] = (hex_digit(key[2 * i]) << 4) | hex_digit(key[2 * i + 1]);
            }
            okay = has_pairing_key(&next);
        }
        okay = okay && nvs_set_blob(ota_store, "network", &next, sizeof(next)) == ESP_OK &&
               nvs_commit(ota_store) == ESP_OK;
    }
    cJSON_Delete(root);
    return okay;
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
                     has_pairing_key(&config) ? "true" : "false");
            usb_reply(out);
        } else if (!overflow && (!strncmp(line, "SDP-PROVISION ", 14) || !strncmp(line, "SDP-WIFI ", 9))) {
            const bool wifi_only = !strncmp(line, "SDP-WIFI ", 9);
            if (provision(line + (wifi_only ? 9 : 14), wifi_only)) {
                usb_reply("SDP-USB {\"provisioned\":true}\n");
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

static void load_identity(void)
{
    uint8_t id[8];
    size_t size = sizeof(id);
    esp_err_t found = nvs_get_blob(identity_store, "device_id", id, &size);
    if (found == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(id, sizeof(id));
        ESP_ERROR_CHECK(nvs_set_blob(identity_store, "device_id", id, sizeof(id)));
        ESP_ERROR_CHECK(nvs_commit(identity_store));
    } else {
        ESP_ERROR_CHECK(found);
        ESP_ERROR_CHECK(size == sizeof(id) ? ESP_OK : ESP_ERR_INVALID_SIZE);
    }
    to_hex(id, sizeof(id), device_id);
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
    bool configured = nvs_get_blob(ota_store, "network", &config, &size) == ESP_OK && size == sizeof(config);
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));
    configASSERT(xTaskCreate(usb_task, "sdp_usb", 4096, NULL, 5, NULL) == pdPASS);
    bool healthy = configured && start_wifi() == ESP_OK;
    if (healthy && pending) {
        healthy = (xEventGroupWaitBits(network_events, NETWORK_READY, pdFALSE, pdFALSE,
                                     pdMS_TO_TICKS(URGENT_BOOT_MS)) & NETWORK_READY) != 0;
    }
#ifdef SDP_OTA_TEST_REJECT
    if (pending) {
        healthy = false;
    }
#endif
    healthy = healthy && has_pairing_key(&config);
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

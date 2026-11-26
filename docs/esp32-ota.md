# ESP32 OTA

The native ESP-IDF project is in `firmware/esp-idf/sdp-ota`. The uploader is in `tools/sdp-ota/sdp_ota.py`. Build on `app01` and upload from MSI or another host on the board's network. This project uses a 4 MB flash layout with two OTA slots. It leaves the Arduino SDP library and serial wire format available for existing devices. The native project's USB setup and OTA status have their own interface. Its partition table preserves the 20 KiB Arduino NVS region used for the device identifier.

## System module

The module manifest is [tools/sdp-ota/module.json](../tools/sdp-ota/module.json). Schema version 1 exposes the module ID, semantic version, and a fixed Python entrypoint adjacent to the manifest. Configure the owner-trusted local SDP checkout in the host application. The host reads the manifest and starts the entrypoint again for each tool call. Updating this checkout changes the uploader available to the next call. Increment the manifest version when changing the uploader. Keep the CLI arguments and NDJSON schema compatible within schema version 1.

The host stores device addresses and pairing credentials on the upload machine. Agents work with device and build IDs. The host resolves authorized input through its device registry and preserves the resulting verified, failed, rolled back, or unknown outcome in upload history.

## Build on app01

Compile the exact source archive chosen in SynaptaCode. Use a new build folder for each build. The signing key stays under server access control outside Git and the source archive. Use the same trusted signer for USB bootstrap and later builds. For a fresh test signer, generate an ECDSA key with the ESP-IDF Python environment using `python -m espsecure generate_signing_key --version 1 PATH`. Set the key's absolute path in a server-local SDK configuration defaults file using `CONFIG_SECURE_BOOT_SIGNING_KEY="C:/path/ota-signing-key.pem"`. Pass both defaults files to ESP-IDF with `-D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;C:/path/signing.defaults"`.

```powershell
& 'C:\Nin\esp-idf\idf.cmd' -D IDF_TARGET=esp32 -D SDP_PROJECT_ID=my-project -D SDP_BUILD_ID=build-a -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;C:/path/signing.defaults" build
```

Later builds of the same project use a new `SDP_BUILD_ID`. OTA takes only the signed `build/sdp_ota.bin` application file. Install changes to the bootloader or partition table through USB. Application-specific startup can be added in `app_main` before the pending build is marked valid. Never enable the optional `SDP_OTA_TEST_REJECT` build flag for normal firmware.

## The `sdpo` tool

`tools/sdp-ota/sdpo.cmd` (Windows) and `tools/sdp-ota/sdpo` (Linux, macOS) run the uploader. Every command prints NDJSON on stdout, so people and agents read the same records, and `sdpo --help` lists the common tasks. It needs Python 3.11 or newer with pyserial; `install` and `setup` also need esptool (`python -m pip install esptool`, or set `SDP_ESPTOOL` to a folder that holds it).

```powershell
sdpo ports                                                    # USB serial ports
sdpo setup --port COM3 --firmware fw.zip --key-dir KEYS --ssid "My Wi-Fi" --no-backup
sdpo discover                                                 # boards on the network
sdpo status --device-id ID --key-dir KEYS
sdpo upload --device-id ID --key-dir KEYS --image sdp_ota.bin
sdpo install --port COM3 --firmware fw.zip --backup old-flash.bin
```

- `setup` is the one step for a new board: install over USB, pair, join Wi-Fi and wait until the board answers on the network. `--firmware` takes the ESP-IDF build folder or a ZIP holding `flasher_args.json` and the files it lists (a flat ZIP is fine). The tool refuses an image without the SDP descriptor, since the board would lose wireless updates.
- `install` writes the bootloader, partition table, OTA data and app over USB, for recovery or a changed partition table. Choose `--backup FILE` (the whole flash, read in 256 KiB pieces because one long read hung on a real board) or `--no-backup`.
- `status` and `upload` find the board's address from its ID when `--host` is not given, take the key from `--key-dir`, and `upload` reads the project and build IDs from the image. SynaptaCode and Connect still pass `--host`, `--key-file` and every ID explicitly.

## Identity

On first boot the board draws a 256-bit secret and keeps it in NVS until the flash is erased. The secret is SHA-256 over 64 bytes of the hardware RNG (read with its entropy source on, before Wi-Fi starts), the hardware fingerprint and boot timing. `deviceId` is the first 8 bytes (16 hex characters) of SHA-256(`"SDP-ID-v2"` ‖ secret). The ID needs no hardware value: a blank, duplicated or unreadable MAC on a clone or a damaged chip neither stops the board from booting nor makes two IDs collide. The secret is also the per-board HMAC-SHA256 pairing key.

The hardware fingerprint is SHA-256 of the factory MAC and the SPI flash unique ID, whichever can be read, stored next to the secret. It only detects a flash image copied onto another chip: the copy then draws its own secret, so it gets a new, unpaired ID instead of impersonating the original. As a second guard, pairing refuses to overwrite a key file that holds a different secret for the same ID.

## Pair over USB

`setup` pairs as part of installing. To pair or change Wi-Fi on a board that already runs SDP firmware:

```powershell
sdpo provision --port COM3 --key-dir KEYS --ssid "My Wi-Fi"
sdpo wifi --port COM3 --ssid "My Wi-Fi"
sdpo info --port COM3
```

`provision` pairs the board: the board returns its secret over USB, and the uploader writes it to `KEYS/<deviceId>.txt` (or `--key-file PATH`) as 64 lowercase hex characters and never prints it. Physical USB access is the trust boundary. Wi-Fi is optional for `provision`; without it the board keeps its current network. `--ssid` prompts for the WPA2 password without echo, and `--wifi-config PATH` takes a private UTF-8 JSON file containing `ssid` and `password`. `--mode ap` makes the board a WPA2 access point at `192.168.4.1` for direct bench use. The default `sta` joins an existing 2.4 GHz network. Keep key files in a host directory restricted to the updater account, outside Git.

`wifi` sets the network without pairing. `info` shows the current `ipv4` address and whether the board is `paired`. The board opens its OTA endpoint only after pairing. Key files from older firmware (32 hex characters, host-generated) are still accepted by `status` and `upload`.

## Discovery

A board on the network answers the UDP datagram `{"probe":"synapta-sdp"}` on port `52404` with `{"protocol":"sdp/v1","deviceId","projectId","paired","port"}`. Status and builds stay behind the authenticated endpoint. `sdpo.cmd discover` lists boards as NDJSON `device` records with their current address. It sends the probe from every local IPv4 address, since Windows sends a plain broadcast through one adapter only.

## SynaptaCode and agent calls

Host applications call the explicit form:

```powershell
python tools/sdp-ota/sdp_ota.py status --host HOST --key-file PATH
python tools/sdp-ota/sdp_ota.py upload --host HOST --device-id ID --project-id ID --build-id ID --image FILE --key-file PATH
```

The connection is local HTTP to TCP port `52410`, with a per-board HMAC-SHA256 pairing credential. The uploader authenticates board responses against its request nonce. The board accepts an upload once per fresh challenge and authenticates the target IDs, length and SHA-256. It compares project and build IDs embedded in the image and uses ESP-IDF to verify the image signature before enabling the inactive slot. The image itself travels in plaintext. Use the board's current address from `discover`. The USB info command also reports its IPv4 address after it joins the network.

Stdout is NDJSON. `type: "progress"` events have a `stage` of `checking_device`, `uploading` or `verifying`. Upload progress includes `bytesSent` and `bytesTotal`. The final `type: "result"` record includes `outcome`, `deviceId`, `projectId`, `buildId`, `runningBuildId` (nullable) and a possible `error`. Exit codes are 0 for `verified`, 1 for `failed`, 2 for `rolled_back` and 3 for `unknown`. Use the result record to write SynaptaCode's upload history. An invalid invocation may exit with argparse code 2 and no result record.

An acknowledgement only means the board staged the binary. The host records `verified` after a new authenticated boot of the intended build, and `rolled_back` after a new boot of the previous build with evidence the candidate was rejected. A missing or ambiguous post-reboot response is `unknown`. If the candidate cannot bring up its network and OTA service, the bootloader restores the previous firmware.

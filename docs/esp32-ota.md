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

For initial installation, follow the generated `flasher_args.json` and write the bootloader, partition table, OTA data and application files over USB. Keep a verified backup for recovery. Later builds of the same project use a new `SDP_BUILD_ID`. OTA takes only the signed `build/sdp_ota.bin` application file. Install changes to the bootloader or partition table through USB. Application-specific startup can be added in `app_main` before the pending build is marked valid. Never enable the optional `SDP_OTA_TEST_REJECT` build flag for normal firmware.

## Provision once over USB on the upload host

```powershell
.\tools\sdp-ota\sdpo.cmd info --port COM3
.\tools\sdp-ota\sdpo.cmd wifi --port COM3 --ssid "My Wi-Fi"
.\tools\sdp-ota\sdpo.cmd provision --port COM3 --ssid "My Wi-Fi" --key-file PATH
```

The uploader runs on Python 3.11 or newer. USB commands use pyserial. Provisioning requests the WPA2 Wi-Fi password without echo. For automation, `--wifi-config PATH` takes a private UTF-8 JSON file containing `ssid` and `password` in place of `--ssid`. `--mode ap` makes the board a WPA2 access point at `192.168.4.1` for direct bench use. The default `sta` joins an existing 2.4 GHz network. The pairing credential is a file containing 32 lowercase hex characters, generated on provisioning if absent. Keep it in a host directory restricted to the updater account, outside Git. The board stores the pairing credential and Wi-Fi configuration in NVS. Use authorized physical USB access to provision or recover the board.

Use `wifi` on firmware that supports `SDP-WIFI` to set the network without setting a pairing credential. It preserves an existing credential. `info` shows the current `ipv4` address and whether the board is `paired`. The board opens its OTA endpoint only after pairing.

## SynaptaCode and agent calls

```powershell
python tools/sdp-ota/sdp_ota.py status --host HOST --key-file PATH
python tools/sdp-ota/sdp_ota.py upload --host HOST --device-id ID --project-id ID --build-id ID --image FILE --key-file PATH
```

The connection is local HTTP to TCP port `52410`, with a per-board HMAC-SHA256 pairing credential. The uploader authenticates board responses against its request nonce. The board accepts an upload once per fresh challenge and authenticates the target IDs, length and SHA-256. It compares project and build IDs embedded in the image and uses ESP-IDF to verify the image signature before enabling the inactive slot. The image itself travels in plaintext. Use the board's current address on the local network. The USB info command reports its IPv4 address after it joins the network.

Stdout is NDJSON. `type: "progress"` events have a `stage` of `checking_device`, `uploading` or `verifying`. Upload progress includes `bytesSent` and `bytesTotal`. The final `type: "result"` record includes `outcome`, `deviceId`, `projectId`, `buildId`, `runningBuildId` (nullable) and a possible `error`. Exit codes are 0 for `verified`, 1 for `failed`, 2 for `rolled_back` and 3 for `unknown`. Use the result record to write SynaptaCode's upload history. An invalid invocation may exit with argparse code 2 and no result record.

An acknowledgement only means the board staged the binary. The host records `verified` after a new authenticated boot of the intended build, and `rolled_back` after a new boot of the previous build with evidence the candidate was rejected. A missing or ambiguous post-reboot response is `unknown`. If the candidate cannot bring up its network and OTA service, the bootloader restores the previous firmware.

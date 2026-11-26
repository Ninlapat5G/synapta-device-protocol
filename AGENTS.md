# Synapta Device Protocol

- For ESP32 firmware updates, use the SDP module under [tools/sdp-ota](tools/sdp-ota/module.json). Read [the OTA guide](docs/esp32-ota.md) for the CLI and the JSON results.
- Compile ESP32 firmware on app01 with ESP-IDF. Use MSI for USB provisioning and Wi-Fi upload. Keep signing keys on the build host and pairing credentials on the upload host.
- SynaptaCode and Connect use the owner's trusted SDP checkout. When changing the uploader, keep schema version 1 compatible and increment the module version.

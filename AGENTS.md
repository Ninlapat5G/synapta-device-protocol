# Synapta Device Protocol

- For ESP32 firmware, use `tools/sdp-ota/sdpo.cmd` (`./sdpo` elsewhere): `setup` installs, pairs and joins Wi-Fi over USB; `upload` updates over Wi-Fi; `sdpo --help` lists the tasks. Read [the OTA guide](docs/esp32-ota.md) for the JSON results.
- Compile ESP32 firmware on app01 with ESP-IDF. Use MSI for USB provisioning and Wi-Fi upload. Keep signing keys on the build host and pairing credentials on the upload host.
- SynaptaCode and Connect use the owner's trusted SDP checkout. When changing the uploader, keep schema version 1 compatible and increment the module version.

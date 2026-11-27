import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import sdp_ota as ota


class TestUpload(unittest.TestCase):
    def test_wifi_only_does_not_request_or_store_the_board_secret(self):
        with tempfile.TemporaryDirectory() as temp:
            wifi = Path(temp) / "wifi.json"
            wifi.write_text(json.dumps({"ssid": "test-network", "password": "test-password"}), encoding="utf-8")
            connection = io.BytesIO()
            args = argparse.Namespace(wifi_config=wifi, ssid=None, port="COM3", device_id=None, mode="sta")
            with (
                patch.object(ota, "serial_port", return_value=contextlib.nullcontext(connection)),
                patch.object(ota, "usb_info", return_value={"deviceId": "a" * 16}),
                patch.object(ota, "usb_read", return_value={"provisioned": True, "deviceId": "a" * 16}),
                patch.object(ota, "write_key") as write_key,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(ota.provision(args, wifi_only=True), 0)
            write_key.assert_not_called()
            prefix, payload = connection.getvalue().split(b" ", 1)
            self.assertEqual(prefix, b"SDP-WIFI")
            self.assertEqual(json.loads(payload), {"mode": "sta", "ssid": "test-network", "password": "test-password"})

    def test_pairing_stores_the_board_secret_by_device_id_and_never_prints_it(self):
        secret = "ab" * 32
        with tempfile.TemporaryDirectory() as temp:
            connection = io.BytesIO()
            args = argparse.Namespace(wifi_config=None, ssid=None, port="COM3", device_id=None, mode="sta",
                                      key_file=None, key_dir=Path(temp) / "keys")
            output = io.StringIO()
            with (
                patch.object(ota, "serial_port", return_value=contextlib.nullcontext(connection)),
                patch.object(ota, "usb_info", return_value={"deviceId": "a" * 16}),
                patch.object(ota, "usb_read", return_value={"provisioned": True, "deviceId": "a" * 16, "secret": secret}),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(ota.provision(args), 0)
            # Pair-only: no Wi-Fi fields are sent, so the board keeps its network.
            self.assertEqual(connection.getvalue(), b"SDP-PROVISION {}\n")
            key = Path(temp) / "keys" / ("a" * 16 + ".txt")
            self.assertEqual(ota.read_key(key), bytes.fromhex(secret))
            self.assertNotIn(secret, output.getvalue())

    def test_pairing_rejects_firmware_without_a_board_secret(self):
        with tempfile.TemporaryDirectory() as temp:
            args = argparse.Namespace(wifi_config=None, ssid=None, port="COM3", device_id=None, mode="sta",
                                      key_file=Path(temp) / "key.txt", key_dir=None)
            with (
                patch.object(ota, "serial_port", return_value=contextlib.nullcontext(io.BytesIO())),
                patch.object(ota, "usb_info", return_value={"deviceId": "a" * 16}),
                patch.object(ota, "usb_read", return_value={"provisioned": True, "deviceId": "a" * 16}),
                self.assertRaises(ota.OtaError),
            ):
                ota.provision(args)
            self.assertFalse((Path(temp) / "key.txt").exists())

    def test_signed_reply_cannot_be_reused_for_another_request(self):
        key = bytes(range(16))
        body = b'{"status":"staged"}'
        nonce = "a" * 32

        class Reply:
            status = 200

            def read(self, size):
                return body

            def getheader(self, name, default):
                return ota.mac(key, "response", nonce, 200, body)

        class Connection:
            def getresponse(self):
                return Reply()

        self.assertEqual(ota.checked_response(Connection(), key, nonce), (200, {"status": "staged"}))
        with self.assertRaises(ota.OtaError):
            ota.checked_response(Connection(), key, "b" * 32)

    def test_followup_must_match_attempt_and_boot(self):
        original = {
            "deviceId": "a" * 16, "projectId": "example", "runningBuildId": "original",
            "bootId": "a" * 32, "bootState": "confirmed", "challenge": "c" * 32,
        }
        for outcome, desired_status in [
            ("verified", {"bootState": "confirmed", "runningBuildId": "target"}),
            ("rolled_back", {"bootState": "rolled_back", "runningBuildId": "original"}),
        ]:
            current = {**original, **desired_status, "bootId": "b" * 32, "lastAttempt": original["challenge"]}
            self.assertEqual(ota.reboot_result(original, current, "target", original["challenge"]), outcome)
            for wrong in [{"deviceId": "d" * 16}, {"bootId": original["bootId"]}, {"lastAttempt": ""}]:
                self.assertIsNone(ota.reboot_result(original, {**current, **wrong}, "target", original["challenge"]))

    def test_upload_returns_verified_after_losing_staging_response(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            key, image = path / "key", path / "image.bin"
            key.write_text("1" * 32, encoding="ascii")
            descriptor = b"SDPOTA1\0" + b"project".ljust(ota.ID_LENGTH, b"\0") + b"test-b".ljust(ota.ID_LENGTH, b"\0")
            image.write_bytes(bytes(ota.APP_DESC_OFFSET) + descriptor + b"fake image")
            original = {
                "deviceId": "a" * 16, "projectId": "project", "runningBuildId": "test-a",
                "bootId": "a" * 32, "bootState": "confirmed", "challenge": "c" * 32,
            }
            current = {
                **original, "bootId": "b" * 32, "lastAttempt": original["challenge"], "runningBuildId": "test-b",
            }
            args = argparse.Namespace(
                device_id=original["deviceId"], project_id="project", build_id="test-b",
                host="test", port=52410, key_file=key, image=image, timeout=2,
            )
            output = io.StringIO()
            with (
                patch.object(ota, "get_status", side_effect=[original, current]),
                patch.object(ota, "post_image", side_effect=TimeoutError),
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(ota.upload(args), 0)
            final = json.loads(output.getvalue().splitlines()[-1])
            self.assertEqual(final["outcome"], "verified")
            self.assertEqual(final["runningBuildId"], "test-b")

            with image.open("rb") as stream, self.assertRaises(ota.OtaError):
                ota.validate_image(stream, "project", "other")


class TestShell(unittest.TestCase):
    def test_image_identity_reads_ids_and_rejects_foreign_images(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "app.bin"
            descriptor = b"SDPOTA1\0" + b"project".ljust(ota.ID_LENGTH, b"\0") + b"build-7".ljust(ota.ID_LENGTH, b"\0")
            image.write_bytes(bytes(ota.APP_DESC_OFFSET) + descriptor)
            self.assertEqual(ota.image_identity(image), ("project", "build-7"))
            image.write_bytes(bytes(ota.APP_DESC_OFFSET + 300))
            with self.assertRaises(ota.OtaError):
                ota.image_identity(image)

    def test_a_second_board_cannot_take_over_an_existing_pairing(self):
        with tempfile.TemporaryDirectory() as temp:
            key = Path(temp) / ("a" * 16 + ".txt")
            ota.write_key(key, "ab" * 32)
            ota.write_key(key, "ab" * 32)  # re-pairing the same board is fine
            with self.assertRaises(ota.OtaError):
                ota.write_key(key, "cd" * 32)
            self.assertEqual(ota.read_key(key), bytes.fromhex("ab" * 32))

    def test_firmware_files_resolve_in_a_flat_zip_by_name(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            flasher = {"flash_files": {"0x10000": "sdp_ota.bin", "0x1000": "bootloader/bootloader.bin"},
                       "app": {"file": "sdp_ota.bin"}, "extra_esptool_args": {"chip": "esp32"}}
            (root / "flasher_args.json").write_text(json.dumps(flasher), encoding="utf-8")
            (root / "sdp_ota.bin").write_bytes(b"app")
            (root / "bootloader.bin").write_bytes(b"boot")
            _, files, app = ota.firmware_files(root)
            self.assertEqual([(offset, path.name) for offset, path in files], [("0x1000", "bootloader.bin"), ("0x10000", "sdp_ota.bin")])
            self.assertEqual(app.name, "sdp_ota.bin")

    def test_short_status_finds_the_address_and_key_by_device_id(self):
        args = argparse.Namespace(host=None, port=52410, device_id="a" * 16, key_file=None, key_dir=Path("keys"))
        board = {"deviceId": "a" * 16, "projectId": "p", "paired": True, "address": "192.0.2.9", "port": 52410}
        with patch.object(ota, "find_boards", return_value={"a" * 16: board}):
            ota.resolve_board(args)
        self.assertEqual((args.host, args.key_file), ("192.0.2.9", Path("keys") / ("a" * 16 + ".txt")))

    def test_probes_match_the_firmware_strings_byte_for_byte(self):
        # The firmware compares whole datagrams with strcmp, so spacing or key order would hide a paired board.
        self.assertEqual(ota.probes(["a" * 16]),
                         [b'{"probe":"synapta-sdp"}', b'{"probe":"synapta-sdp","deviceId":"' + b"a" * 16 + b'"}'])

    def test_unpair_signs_the_current_challenge_and_device(self):
        key = bytes(range(32))
        status = {"deviceId": "a" * 16, "projectId": "p", "challenge": "c" * 32}
        sent = {}

        class Connection:
            def __init__(self, *args, **kwargs):
                pass

            def request(self, method, path, headers):
                sent.update(method=method, path=path, headers=headers)

            def close(self):
                pass

        args = argparse.Namespace(host="192.0.2.9", port=52410, device_id="a" * 16, key_file=Path("k"), key_dir=None)
        with (
            patch.object(ota, "read_key", return_value=key),
            patch.object(ota, "get_status", return_value=status),
            patch.object(ota, "HTTPConnection", Connection),
            patch.object(ota, "checked_response", return_value=(200, {"status": "unpaired"})),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(ota.unpair(args), 0)
        self.assertEqual((sent["method"], sent["path"]), ("POST", "/v1/unpair"))
        self.assertEqual(sent["headers"]["X-SDP-Nonce"], "c" * 32)
        self.assertEqual(sent["headers"]["X-SDP-Auth"], ota.mac(key, "unpair", "c" * 32, "a" * 16))
        self.assertIn('"type":"unpaired"', output.getvalue())


if __name__ == "__main__":
    unittest.main()

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
    def test_wifi_only_does_not_generate_or_send_a_pairing_key(self):
        with tempfile.TemporaryDirectory() as temp:
            wifi = Path(temp) / "wifi.json"
            wifi.write_text(json.dumps({"ssid": "test-network", "password": "test-password"}), encoding="utf-8")
            connection = io.BytesIO()
            args = argparse.Namespace(wifi_config=wifi, port="COM3", device_id=None, mode="sta")
            with (
                patch.object(ota, "serial_port", return_value=contextlib.nullcontext(connection)),
                patch.object(ota, "usb_info", return_value={"deviceId": "a" * 16}),
                patch.object(ota, "usb_read", return_value={"provisioned": True}),
                patch.object(ota, "read_key") as read_key,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(ota.provision(args, wifi_only=True), 0)
            read_key.assert_not_called()
            prefix, payload = connection.getvalue().split(b" ", 1)
            self.assertEqual(prefix, b"SDP-WIFI")
            self.assertEqual(json.loads(payload), {"mode": "sta", "ssid": "test-network", "password": "test-password"})

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


if __name__ == "__main__":
    unittest.main()

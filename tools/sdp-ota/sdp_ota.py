
"""USB provisioning and authenticated LAN OTA for Synapta's ESP32 firmware."""

import argparse
import getpass
import hashlib
import hmac
from http.client import HTTPConnection, HTTPException
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
import warnings

PORT = 52410
APP_DESC_OFFSET = 288
ID_LENGTH = 97
CHUNK_SIZE = 16384
RESULT_CODES = {"verified": 0, "failed": 1, "rolled_back": 2, "unknown": 3}
IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9_-]{0,95}\Z")
RESERVED = re.compile(r"(?:con|prn|aux|nul|com[0-9]|lpt[0-9])\Z")
NONCE = re.compile(r"[0-9a-f]{32}\Z")
DEVICE = re.compile(r"[0-9a-f]{16}\Z")


class OtaError(Exception):
    pass


def emit(event_type, **data):
    print(json.dumps({"type": event_type, **data}, separators=(",", ":")), flush=True)


def mac(key, *fields):
    data = b"\n".join(str(field).encode("ascii") if not isinstance(field, bytes) else field for field in fields)
    return hmac.digest(key, data, "sha256").hex()


def read_key(file, create=False):
    if create and not file.exists():
        fd = os.open(file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(secrets.token_hex(16) + "\n")
    text = file.read_text(encoding="ascii").strip()
    if not NONCE.fullmatch(text):
        raise OtaError("key file must contain 32 lowercase hex characters")
    return bytes.fromhex(text)


def checked_response(connection, key, nonce):
    response = connection.getresponse()
    body = response.read(1025)
    if len(body) > 1024 or not hmac.compare_digest(
        response.getheader("X-SDP-Auth", ""), mac(key, "response", nonce, response.status, body)
    ):
        raise OtaError("board response failed authentication")
    return response.status, json.loads(body)


def get_status(host, port, key):
    nonce = secrets.token_hex(16)
    connection = HTTPConnection(host, port, timeout=5)
    try:
        connection.request("GET", "/v1/status", headers={
            "X-SDP-Nonce": nonce, "X-SDP-Auth": mac(key, "status", nonce),
            "Connection": "close",
        })
        code, status = checked_response(connection, key, nonce)
    finally:
        connection.close()
    if (code != 200 or not isinstance(status, dict)
        or not DEVICE.fullmatch(str(status.get("deviceId", "")))
        or not all(IDENTIFIER.fullmatch(str(status.get(field, ""))) for field in ("projectId", "runningBuildId"))
        or not all(NONCE.fullmatch(str(status.get(field, ""))) for field in ("bootId", "challenge"))
        or (status.get("lastAttempt") != "" and not NONCE.fullmatch(str(status.get("lastAttempt", ""))))
        or status.get("bootState") not in {"confirmed", "pending", "rolled_back"}):
        raise OtaError("board status is invalid")
    return status


def validate_image(image, project, build):
    image.seek(APP_DESC_OFFSET)
    identity = b"SDPOTA1\0" + project.encode("ascii").ljust(ID_LENGTH, b"\0") + build.encode("ascii").ljust(ID_LENGTH, b"\0")
    if image.read(len(identity)) != identity:
        raise OtaError("image project/build metadata does not match the requested update")
    size = image.seek(0, os.SEEK_END)
    image.seek(0)
    digest = hashlib.file_digest(image, "sha256").hexdigest()
    image.seek(0)
    return size, digest


def post_image(args, key, nonce, image, size, digest):
    connection = HTTPConnection(args.host, args.port, timeout=15)
    try:
        connection.putrequest("POST", "/v1/ota")
        for name, value in {
            "Connection": "close", "Content-Type": "application/octet-stream", "Content-Length": str(size),
            "X-SDP-Nonce": nonce, "X-SDP-Device": args.device_id, "X-SDP-Project": args.project_id,
            "X-SDP-Build": args.build_id, "X-SDP-SHA256": digest,
            "X-SDP-Auth": mac(key, "ota", nonce, args.device_id, args.project_id, args.build_id, digest, size),
        }.items():
            connection.putheader(name, value)
        connection.endheaders()
        sent = 0
        last_percent = -10
        for chunk in iter(lambda: image.read(CHUNK_SIZE), b""):
            sent += len(chunk)
            if sent > size:
                raise OtaError("image changed during upload")
            connection.send(chunk)
            percent = sent * 100 // size
            if percent >= last_percent + 10 or sent == size:
                emit("progress", stage="uploading", bytesSent=sent, bytesTotal=size)
                last_percent = percent
        return checked_response(connection, key, nonce)
    finally:
        connection.close()


def reboot_result(previous, current, desired, attempt):
    if (current["deviceId"] != previous["deviceId"] or current["projectId"] != previous["projectId"]
        or current["bootId"] == previous["bootId"] or current["lastAttempt"] != attempt):
        return None
    if current["runningBuildId"] == desired and current["bootState"] == "confirmed":
        return "verified"
    if current["runningBuildId"] == previous["runningBuildId"] and current["bootState"] == "rolled_back":
        return "rolled_back"
    return None


def result(args, outcome, running=None, error=None):
    data = {"outcome": outcome, "deviceId": args.device_id, "projectId": args.project_id,
            "buildId": args.build_id, "runningBuildId": running}
    if error:
        data["error"] = error
    emit("result", **data)
    return RESULT_CODES[outcome]


def upload(args):
    emit("progress", stage="checking_device")
    try:
        key = read_key(args.key_file)
        previous = get_status(args.host, args.port, key)
        if previous["deviceId"] != args.device_id or previous["projectId"] != args.project_id:
            raise OtaError("paired board identity or project does not match")
        if previous["runningBuildId"] == args.build_id or previous["bootState"] == "pending":
            raise OtaError("requested build is already running or an update is pending")
        image = args.image.open("rb")
    except (OSError, ValueError, HTTPException, OtaError) as exc:
        return result(args, "failed", error=str(exc))

    nonce = previous["challenge"]
    with image:
        try:
            size, digest = validate_image(image, args.project_id, args.build_id)
        except (OSError, ValueError, OtaError) as exc:
            return result(args, "failed", previous["runningBuildId"], str(exc))
        try:
            code, reply = post_image(args, key, nonce, image, size, digest)
            if code == 400:
                reason = reply.get("error") if isinstance(reply, dict) else None
                return result(args, "failed", previous["runningBuildId"], reason or "board rejected the image")
        except (OSError, HTTPException, ValueError, OtaError):
            # Loss of the upload response can happen after staging or before any bytes land.
            pass

    emit("progress", stage="verifying")
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            current = get_status(args.host, args.port, key)
            outcome = reboot_result(previous, current, args.build_id, nonce)
            if outcome:
                return result(args, outcome, current["runningBuildId"])
        except (OSError, ValueError, HTTPException, OtaError):
            pass
        time.sleep(1)
    return result(args, "unknown", error="no authenticated post-reboot result for this upload")


def serial_port(port):
    try:
        import serial
    except ImportError as exc:
        raise OtaError("USB commands require pyserial on the upload host") from exc
    connection = serial.Serial(baudrate=115200, timeout=0.5, write_timeout=4)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    return connection


def usb_read(connection, deadline):
    while time.monotonic() < deadline:
        line = connection.readline(1024)
        if line.startswith(b"SDP-USB "):
            return json.loads(line[8:])
    raise OtaError("board did not respond over USB")


def usb_info(connection):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        connection.write(b"\nSDP-INFO\n")
        try:
            info = usb_read(connection, min(deadline, time.monotonic() + 1.0))
        except OtaError:
            continue
        if DEVICE.fullmatch(str(info.get("deviceId", ""))):
            return info
    raise OtaError("OTA bootstrap firmware did not report its identity over USB")


def provision(args, wifi_only=False):
    if args.wifi_config:
        wifi = json.loads(args.wifi_config.read_text(encoding="utf-8-sig"))
        if not isinstance(wifi, dict):
            raise OtaError("invalid Wi-Fi configuration")
        ssid, password = wifi.get("ssid"), wifi.get("password")
    else:
        ssid = args.ssid
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            try:
                password = getpass.getpass("Wi-Fi password: ")
            except getpass.GetPassWarning as exc:
                raise OtaError("use --wifi-config when the terminal cannot hide input") from exc
    if (not isinstance(ssid, str) or not isinstance(password, str)
        or "\0" in ssid or "\0" in password or not 1 <= len(ssid.encode("utf-8")) <= 32
        or not 8 <= len(password.encode("utf-8")) <= 63):
        raise OtaError("invalid Wi-Fi configuration")
    with serial_port(args.port) as connection:
        identity = usb_info(connection)
        if args.device_id and args.device_id != identity["deviceId"]:
            raise OtaError("USB board identity does not match")
        settings = {"mode": args.mode, "ssid": ssid, "password": password}
        if not wifi_only:
            settings["key"] = read_key(args.key_file, create=True).hex()
        payload = json.dumps(settings, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        prefix = b"SDP-WIFI " if wifi_only else b"SDP-PROVISION "
        connection.write(prefix + payload + b"\n")
        reply = usb_read(connection, time.monotonic() + 5)
    if reply != {"provisioned": True}:
        raise OtaError("board rejected USB provisioning")
    emit("wifi_configured" if wifi_only else "provisioned", **identity)
    return 0


def identifier(text):
    if not IDENTIFIER.fullmatch(text) or RESERVED.fullmatch(text):
        raise argparse.ArgumentTypeError("expected a valid SynaptaCode identifier")
    return text


def device_id(text):
    if not DEVICE.fullmatch(text):
        raise argparse.ArgumentTypeError("expected 16 lowercase hex characters")
    return text


def network_port(text):
    value = int(text)
    if not 1 <= value <= 65535:
        raise argparse.ArgumentTypeError("expected a port from 1 to 65535")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "upload"):
        network = commands.add_parser(name)
        network.add_argument("--host", required=True)
        network.add_argument("--port", type=network_port, default=PORT)
        network.add_argument("--key-file", required=True, type=Path)
    network.add_argument("--device-id", required=True, type=device_id)
    network.add_argument("--project-id", required=True, type=identifier)
    network.add_argument("--build-id", required=True, type=identifier)
    network.add_argument("--image", required=True, type=Path)
    network.add_argument("--timeout", type=int, default=75, help="post-reboot verification timeout in seconds")
    usb = commands.add_parser("usb-info", aliases=["info"])
    usb.add_argument("--port", required=True)
    for operation in ("provision", "wifi"):
        setup = commands.add_parser(operation)
        setup.add_argument("--port", required=True)
        if operation == "provision":
            setup.add_argument("--key-file", required=True, type=Path)
        setup.add_argument("--mode", choices=("sta", "ap"), default="sta")
        setup.add_argument("--device-id", type=device_id)
        source = setup.add_mutually_exclusive_group(required=True)
        source.add_argument("--ssid")
        source.add_argument("--wifi-config", type=Path)
    args = parser.parse_args()
    if args.command == "upload":
        if args.timeout <= 0:
            parser.error("--timeout must be positive")
        return upload(args)
    try:
        if args.command in {"provision", "wifi"}:
            return provision(args, wifi_only=args.command == "wifi")
        if args.command in {"usb-info", "info"}:
            with serial_port(args.port) as connection:
                emit("usb_info", **usb_info(connection))
        else:
            emit("status", **get_status(args.host, args.port, read_key(args.key_file)))
        return 0
    except (OSError, ValueError, HTTPException, OtaError) as exc:
        emit("error", error=str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())

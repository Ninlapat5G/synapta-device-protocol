
"""Install, pair and update Synapta's ESP32 firmware over USB and Wi-Fi.

Every command prints NDJSON records on stdout, so people and agents read the same results.
"""

import argparse
import getpass
import hashlib
import hmac
import importlib.util
from http.client import HTTPConnection, HTTPException
import json
import os
from pathlib import Path
import re
import secrets
import select
import socket
import subprocess
import sys
import tempfile
import time
import warnings
import zipfile

PORT = 52410
DISCOVERY_PORT = 52404
DISCOVERY_PROBE = b'{"probe":"synapta-sdp"}'
APP_DESC_OFFSET = 288
ID_LENGTH = 97
CHUNK_SIZE = 16384
RESULT_CODES = {"verified": 0, "failed": 1, "rolled_back": 2, "unknown": 3}
IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9_-]{0,95}\Z")
RESERVED = re.compile(r"(?:con|prn|aux|nul|com[0-9]|lpt[0-9])\Z")
NONCE = re.compile(r"[0-9a-f]{32}\Z")
# 64 hex = board-generated 256-bit secret (identity v2); 32 hex = host-generated key of older firmware.
KEY = re.compile(r"(?:[0-9a-f]{32}|[0-9a-f]{64})\Z")
DEVICE = re.compile(r"[0-9a-f]{16}\Z")


class OtaError(Exception):
    pass


def emit(event_type, **data):
    print(json.dumps({"type": event_type, **data}, separators=(",", ":")), flush=True)


def mac(key, *fields):
    data = b"\n".join(str(field).encode("ascii") if not isinstance(field, bytes) else field for field in fields)
    return hmac.digest(key, data, "sha256").hex()


def read_key(file):
    text = file.read_text(encoding="ascii").strip()
    if not KEY.fullmatch(text):
        raise OtaError("key file must contain 64 (or legacy 32) lowercase hex characters")
    return bytes.fromhex(text)


def write_key(file, secret):
    """Writes the board secret readable only by this account, replacing a previous copy atomically.

    The same ID with a different secret means two boards claim one identity (a copied flash image on
    firmware without clone detection), so the existing pairing is kept and the new one refused."""
    if file.exists() and file.read_text(encoding="ascii").strip() != secret:
        raise OtaError(f"{file.name} already holds a different board's key; remove it only if that board is gone")
    file.parent.mkdir(parents=True, exist_ok=True)
    temporary = file.with_name(file.name + ".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as stream:
        stream.write(secret + "\n")
    os.replace(temporary, file)


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


def image_identity(path):
    """Project and build IDs embedded in an SDP application image (sdp_app_desc), or an OtaError."""
    with open(path, "rb") as image:
        image.seek(APP_DESC_OFFSET)
        raw = image.read(8 + 2 * ID_LENGTH)
    if len(raw) != 8 + 2 * ID_LENGTH or raw[:8] != b"SDPOTA1\0":
        raise OtaError("not an SDP OTA firmware image; the board would lose wireless updates")
    project, build = (raw[8 + i * ID_LENGTH:8 + (i + 1) * ID_LENGTH].split(b"\0", 1)[0].decode("ascii") for i in (0, 1))
    if not IDENTIFIER.fullmatch(project) or not IDENTIFIER.fullmatch(build):
        raise OtaError("image carries invalid project or build IDs")
    return project, build


def resolve_board(args):
    """Fills in what a short command leaves out: the address from discovery and the key from --key-dir."""
    if not args.host:
        if not args.device_id:
            raise OtaError("give --device-id (the address is then found on the network) or --host")
        board = find_boards(timeout=2.0, device_ids=[args.device_id]).get(args.device_id)
        if not board:
            raise OtaError("board not found on the network; check that it is powered and on Wi-Fi")
        args.host, args.port = board["address"], board["port"]
    if not args.key_file:
        if not (args.key_dir and args.device_id):
            raise OtaError("give --key-file, or --key-dir with --device-id")
        args.key_file = args.key_dir / (args.device_id + ".txt")


def upload(args):
    emit("progress", stage="checking_device")
    try:
        if not args.project_id or not args.build_id:
            args.project_id, args.build_id = image_identity(args.image)
        resolve_board(args)
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
        raise OtaError(f"USB commands need pyserial: {sys.executable} -m pip install pyserial") from exc
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


def wifi_settings(args, required):
    """Wi-Fi from --wifi-config or --ssid (password prompted); None when optional and not given."""
    if args.wifi_config:
        wifi = json.loads(args.wifi_config.read_text(encoding="utf-8-sig"))
        if not isinstance(wifi, dict):
            raise OtaError("invalid Wi-Fi configuration")
        ssid, password = wifi.get("ssid"), wifi.get("password")
    elif args.ssid:
        ssid = args.ssid
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            try:
                password = getpass.getpass("Wi-Fi password: ")
            except getpass.GetPassWarning as exc:
                raise OtaError("use --wifi-config when the terminal cannot hide input") from exc
    elif required:
        raise OtaError("set --ssid or --wifi-config")
    else:
        return None
    if (not isinstance(ssid, str) or not isinstance(password, str)
        or "\0" in ssid or "\0" in password or not 1 <= len(ssid.encode("utf-8")) <= 32
        or not 8 <= len(password.encode("utf-8")) <= 63):
        raise OtaError("invalid Wi-Fi configuration")
    return {"mode": args.mode, "ssid": ssid, "password": password}


def provision(args, wifi_only=False):
    """wifi: set the network. provision: pair (and optionally set the network); the board returns its
    own secret, which is stored as the key file and never printed."""
    settings = wifi_settings(args, required=wifi_only) or {}
    with serial_port(args.port) as connection:
        identity = usb_info(connection)
        if args.device_id and args.device_id != identity["deviceId"]:
            raise OtaError("USB board identity does not match")
        payload = json.dumps(settings, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        prefix = b"SDP-WIFI " if wifi_only else b"SDP-PROVISION "
        connection.write(prefix + payload + b"\n")
        reply = usb_read(connection, time.monotonic() + 5)
    if reply.get("provisioned") is not True or reply.get("deviceId") != identity["deviceId"]:
        raise OtaError("board rejected USB provisioning")
    if not wifi_only:
        secret = reply.get("secret")
        if not isinstance(secret, str) or not re.fullmatch(r"[0-9a-f]{64}", secret):
            raise OtaError("board firmware does not support identity v2 pairing")
        key_file = args.key_file or args.key_dir / (identity["deviceId"] + ".txt")
        write_key(key_file, secret)
    emit("wifi_configured" if wifi_only else "provisioned", **identity)
    return 0


def local_addresses():
    try:
        return sorted({info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)})
    except OSError:
        return []


def probes(device_ids):
    """The plain probe reaches unpaired boards (and v0.2.1 firmware); a paired board answers only a probe
    that names its ID, so each known board gets its own."""
    return [DISCOVERY_PROBE, *(json.dumps({"probe": "synapta-sdp", "deviceId": board}, separators=(",", ":")).encode()
                               for board in device_ids)]


def find_boards(address=None, timeout=1.5, device_ids=()):
    """Broadcasts the SDP probes and returns {deviceId: board} for each board that answers.

    Windows sends 255.255.255.255 out of one adapter only (often a VPN such as Radmin or Tailscale),
    so the probe goes out once per local IPv4 address from a socket bound to it."""
    sockets = []
    messages = probes(device_ids)
    try:
        for local in ([""] if address else ["", *local_addresses()]):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sock)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            try:
                sock.bind((local, 0))
                for message in messages:
                    sock.sendto(message, (address or "255.255.255.255", DISCOVERY_PORT))
            except OSError:
                continue
        found = {}
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            ready, _, _ = select.select(sockets, [], [], remaining)
            if not ready:
                break
            for sock in ready:
                try:
                    data, (address, port) = sock.recvfrom(512)
                    beacon = json.loads(data)
                except (OSError, ValueError):
                    continue
                if (port != DISCOVERY_PORT or not isinstance(beacon, dict) or beacon.get("protocol") != "sdp/v1"
                        or not DEVICE.fullmatch(str(beacon.get("deviceId", "")))
                        or not IDENTIFIER.fullmatch(str(beacon.get("projectId", "")))
                        or not isinstance(beacon.get("paired"), bool)):
                    continue
                found[beacon["deviceId"]] = {"deviceId": beacon["deviceId"], "projectId": beacon["projectId"],
                                             "paired": beacon["paired"], "address": address,
                                             "port": beacon.get("port", PORT)}
    finally:
        for sock in sockets:
            sock.close()
    return found


def discover(args):
    for board in find_boards(args.address, args.timeout, args.device_id).values():
        emit("device", **board)
    return 0


def unpair(args):
    """Ends the pairing on the board itself, so it answers scans for new boards again. The key file is
    left in place: the board keeps its secret, and pairing again over USB writes the same key."""
    resolve_board(args)
    key = read_key(args.key_file)
    status = get_status(args.host, args.port, key)
    if status["deviceId"] != args.device_id:
        raise OtaError("the board at this address has a different ID")
    nonce = status["challenge"]
    connection = HTTPConnection(args.host, args.port, timeout=5)
    try:
        connection.request("POST", "/v1/unpair", headers={
            "X-SDP-Nonce": nonce, "X-SDP-Auth": mac(key, "unpair", nonce, args.device_id),
            "Content-Length": "0", "Connection": "close",
        })
        code, reply = checked_response(connection, key, nonce)
    finally:
        connection.close()
    if code != 200 or not isinstance(reply, dict) or reply.get("status") != "unpaired":
        raise OtaError(f"board refused to unpair: {reply.get('error') if isinstance(reply, dict) else reply}")
    emit("unpaired", deviceId=args.device_id, projectId=status["projectId"])
    return 0


def list_ports():
    """Serial ports for USB pairing, so a host UI can offer a choice instead of asking for COM names."""
    try:
        from serial.tools import list_ports as serial_ports
    except ImportError as exc:
        raise OtaError(f"USB commands need pyserial: {sys.executable} -m pip install pyserial") from exc
    for item in serial_ports.comports():
        emit("port", port=item.device, description=item.description or "", vid=item.vid, pid=item.pid)
    return 0


def esptool(args):
    """`python -m esptool` from this interpreter. SDP_ESPTOOL may name a folder that holds esptool."""
    env = dict(os.environ)
    extra = os.environ.get("SDP_ESPTOOL")
    if extra:
        env["PYTHONPATH"] = extra + os.pathsep + env.get("PYTHONPATH", "")
    elif importlib.util.find_spec("esptool") is None:
        raise OtaError("install esptool (python -m pip install esptool) or set SDP_ESPTOOL to its folder")
    return [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", args.port], env


def run_esptool(command, env, what, timeout):
    try:
        done = subprocess.run(command, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise OtaError(f"{what} timed out") from exc
    if done.returncode:
        tail = (done.stdout + done.stderr).strip().splitlines()[-1:] or ["no output"]
        raise OtaError(f"{what} failed: {tail[0][:200]}")


def firmware_files(root):
    """Offsets and files from ESP-IDF's flasher_args.json in a build folder or a (possibly flat) ZIP."""
    try:
        flasher = json.loads(next(root.rglob("flasher_args.json")).read_text(encoding="utf-8"))
    except StopIteration as exc:
        raise OtaError("flasher_args.json not found in the firmware") from exc
    if flasher.get("extra_esptool_args", {}).get("chip") != "esp32":
        raise OtaError("this firmware is not built for the ESP32")
    files = []
    for offset, relative in sorted(flasher["flash_files"].items(), key=lambda item: int(item[0], 16)):
        path = root / relative
        if not path.is_file():
            path = next(root.rglob(Path(relative).name), None)
        if not path:
            raise OtaError(f"firmware is missing {relative}")
        files.append((offset, path))
    app = root / flasher["app"]["file"]
    if not app.is_file():
        app = next(root.rglob(Path(flasher["app"]["file"]).name))
    return flasher, files, app


def backup_flash(base, env, flash_size, target, baud):
    """Reads the whole flash into `target` in 256 KiB pieces. On a real CP210x board one long read hung,
    and reads at 230400-460800 baud failed at some offsets while 115200 never did, so each piece is
    tried at the fast rate first and again at 115200 before giving up."""
    total = int(flash_size.rstrip("MB")) * 1024 * 1024
    chunk = 256 * 1024
    partial = target.with_name(target.name + ".part")
    with tempfile.TemporaryDirectory() as temp, open(partial, "wb") as output:
        piece = Path(temp) / "piece.bin"
        for offset in range(0, total, chunk):
            rates = [baud, 115200] if baud != 115200 else [115200, 115200]
            for attempt, rate in enumerate(rates):
                try:
                    run_esptool(base + ["--baud", str(rate), "--after", "no_reset", "read_flash", hex(offset), hex(chunk), str(piece)],
                                env, f"reading flash at {offset:#x}", 120)
                    break
                except OtaError:
                    if attempt == len(rates) - 1:
                        raise
            output.write(piece.read_bytes())
            emit("progress", stage="backup", bytesDone=offset + chunk, bytesTotal=total)
    os.replace(partial, target)


def install(args):
    """Installs SDP firmware over USB: optional full backup, then bootloader, partitions, OTA data and app."""
    base, env = esptool(args)
    with tempfile.TemporaryDirectory() as temp:
        root = args.firmware
        if root.is_file():
            with zipfile.ZipFile(root) as archive:
                archive.extractall(temp)
            root = Path(temp)
        flasher, files, app = firmware_files(root)
        project, build = image_identity(app)
        if args.backup:
            backup_flash(base, env, flasher["flash_settings"]["flash_size"], args.backup, args.baud)
        emit("progress", stage="flashing", projectId=project, buildId=build)
        pairs = [str(item) for offset, path in files for item in (offset, path)]
        run_esptool(base + ["--baud", str(args.baud), "--before", "default_reset", "--after", "hard_reset", "write_flash",
                            *flasher["write_flash_args"], *pairs],
                    env, "writing firmware", 600)
    with serial_port(args.port) as connection:
        identity = usb_info(connection)
    if identity.get("runningBuildId") != build:
        raise OtaError("board did not start the installed build")
    emit("installed", **identity)
    return identity


def setup_board(args):
    """One step for a new board: install over USB, pair (and Wi-Fi), then wait until it answers on the network."""
    install(args)
    provision(args)
    with serial_port(args.port) as connection:
        identity = usb_info(connection)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        board = find_boards(timeout=2.0, device_ids=[identity["deviceId"]]).get(identity["deviceId"])
        if board:
            emit("ready", **board)
            return 0
    emit("ready", deviceId=identity["deviceId"], projectId=identity["projectId"], address=None,
         note="paired; not on the network yet (no Wi-Fi given or it did not connect)")
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


EXAMPLES = """common tasks (sdpo = sdpo.cmd on Windows, ./sdpo elsewhere):
  sdpo ports                                    find the board's USB port
  sdpo setup --port COM3 --firmware fw.zip --key-dir KEYS --ssid "My Wi-Fi" --no-backup
                                                new board: install, pair, join Wi-Fi
  sdpo discover                                 new (unpaired) boards on the network
  sdpo discover --device-id ID                  where a paired board is now
  sdpo status --device-id ID --key-dir KEYS     what a board runs
  sdpo upload --device-id ID --key-dir KEYS --image sdp_ota.bin
                                                update over Wi-Fi (IDs read from the image)
  sdpo install --port COM3 --firmware fw.zip --backup old.bin
                                                reinstall or recover over USB
  sdpo unpair --device-id ID --key-dir KEYS     end the pairing; the board shows in scans again
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=EXAMPLES)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "unpair", "upload"):
        network = commands.add_parser(name, help={"status": "read the running build", "unpair": "end the pairing",
                                                  "upload": "send a build over Wi-Fi"}[name])
        network.add_argument("--device-id", type=device_id, help="board ID; finds the address when --host is not given")
        network.add_argument("--host", help="board address (default: discover it)")
        network.add_argument("--port", type=network_port, default=PORT)
        key = network.add_mutually_exclusive_group(required=True)
        key.add_argument("--key-file", type=Path)
        key.add_argument("--key-dir", type=Path, help="folder holding <deviceId>.txt from pairing")
    network.add_argument("--project-id", type=identifier, help="default: read from the image")
    network.add_argument("--build-id", type=identifier, help="default: read from the image")
    network.add_argument("--image", required=True, type=Path, help="signed sdp_ota.bin")
    network.add_argument("--timeout", type=int, default=75, help="post-reboot verification timeout in seconds")
    usb = commands.add_parser("usb-info", aliases=["info"])
    usb.add_argument("--port", required=True)
    for operation in ("provision", "wifi"):
        setup = commands.add_parser(operation)
        setup.add_argument("--port", required=True)
        if operation == "provision":
            target = setup.add_mutually_exclusive_group(required=True)
            target.add_argument("--key-file", type=Path, help="file that receives the board secret")
            target.add_argument("--key-dir", type=Path, help="folder that receives <deviceId>.txt")
        setup.add_argument("--mode", choices=("sta", "ap"), default="sta")
        setup.add_argument("--device-id", type=device_id)
        source = setup.add_mutually_exclusive_group(required=operation == "wifi")
        source.add_argument("--ssid")
        source.add_argument("--wifi-config", type=Path)
    for name in ("install", "setup"):
        flash = commands.add_parser(name, help="install firmware over USB" if name == "install"
                                    else "install, pair and join Wi-Fi in one step")
        flash.add_argument("--port", required=True, help="USB serial port, see `ports`")
        flash.add_argument("--firmware", required=True, type=Path, help="ESP-IDF build folder or ZIP with flasher_args.json")
        flash.add_argument("--baud", type=int, default=460800)
        keep = flash.add_mutually_exclusive_group(required=True)
        keep.add_argument("--backup", type=Path, help="save the whole current flash here first")
        keep.add_argument("--no-backup", action="store_true", help="overwrite without saving the current flash")
        if name == "setup":
            target = flash.add_mutually_exclusive_group(required=True)
            target.add_argument("--key-file", type=Path)
            target.add_argument("--key-dir", type=Path, help="folder that receives <deviceId>.txt")
            flash.add_argument("--mode", choices=("sta", "ap"), default="sta")
            flash.add_argument("--device-id", type=device_id)
            source = flash.add_mutually_exclusive_group()
            source.add_argument("--ssid")
            source.add_argument("--wifi-config", type=Path)
    finder = commands.add_parser("discover")
    finder.add_argument("--address", help="ask one address instead of broadcasting")
    finder.add_argument("--timeout", type=float, default=1.5)
    finder.add_argument("--device-id", type=device_id, action="append", default=[],
                        help="also find this paired board (repeatable); paired boards skip the plain scan")
    commands.add_parser("ports")
    args = parser.parse_args()
    if args.command == "upload":
        if args.timeout <= 0:
            parser.error("--timeout must be positive")
        return upload(args)
    try:
        if args.command in {"provision", "wifi"}:
            return provision(args, wifi_only=args.command == "wifi")
        if args.command == "discover":
            return discover(args)
        if args.command == "ports":
            return list_ports()
        if args.command == "install":
            install(args)
            return 0
        if args.command == "setup":
            return setup_board(args)
        if args.command == "unpair":
            return unpair(args)
        if args.command in {"usb-info", "info"}:
            with serial_port(args.port) as connection:
                emit("usb_info", **usb_info(connection))
        else:
            resolve_board(args)
            emit("status", **get_status(args.host, args.port, read_key(args.key_file)))
        return 0
    except (OSError, ValueError, HTTPException, OtaError) as exc:
        emit("error", error=str(exc))
        return 1


if __name__ == "__main__":
    if sys.version_info < (3, 11):
        # Older interpreters fail later in confusing ways (hashlib.file_digest), so stop here and name the fix.
        emit("error", error=f"{sys.executable} is Python {sys.version.split()[0]}; set SDP_PYTHON to Python 3.11 or newer")
        sys.exit(1)
    sys.exit(main())

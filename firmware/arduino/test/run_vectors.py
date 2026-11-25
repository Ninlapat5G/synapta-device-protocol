"""ทดสอบไลบรารี Arduino บน PC กับ spec/vectors — คอมไพล์ host/harness.cpp ด้วย MSVC แล้วคุยผ่าน stdin/stdout

    python firmware/arduino/test/run_vectors.py

ไลบรารีและ examples/LedDemo ถูกคอมไพล์จริง (มีแค่ Arduino.h ที่จำลอง) จึงเทียบได้ว่าบอร์ดตอบ byte ตรงกับสเปก
"""

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LIB = HERE.parent / "SynaptaDevice"
VECTORS = ROOT / "spec" / "vectors"
BUILD = HERE / "build"

sys.path.insert(0, str(VECTORS))
from refimpl import cbor, cobs_encode, crc16_ccitt_false, serial_frame, serial_receive  # noqa: E402

failures = []
checked = [0]


def check(name, ok, detail=""):
    checked[0] += 1
    if not ok:
        failures.append(f"{name}: {detail}")


def load(name):
    return json.loads((VECTORS / name).read_text(encoding="utf-8"))


# ---------- build ----------

def build():
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    install = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    if not install:
        sys.exit("ไม่พบ MSVC (Visual Studio Build Tools พร้อม C++)")
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    BUILD.mkdir(exist_ok=True)
    sources = [*sorted((LIB / "src").glob("*.cpp")), *sorted((LIB / "src" / "sdp").glob("*.cpp")),
               HERE / "host" / "harness.cpp"]
    cl = (f'cl /nologo /std:c++17 /W4 /WX /EHsc /utf-8 /DSDP_HOST_TEST /D_CRT_SECURE_NO_WARNINGS /I"{HERE / "host"}" /I"{LIB / "src"}" '
          f'/Fe"{BUILD / "harness.exe"}" /Fo"{BUILD}\\\\" ' + " ".join(f'"{s}"' for s in sources))
    result = subprocess.run(f'call "{vcvars}" >nul && {cl}', shell=True, capture_output=True, text=True,
                            cwd=BUILD)
    if result.returncode != 0:
        print(result.stdout, result.stderr)
        sys.exit("คอมไพล์ harness ไม่ผ่าน")


# ---------- คุยกับ harness ----------

class Harness:
    def __init__(self):
        self.proc = subprocess.Popen([str(BUILD / "harness.exe")], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.boot_lines, self.boot_tx = self._read()

    def _read(self):
        lines, tx = [], b""
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("harness หยุดทำงาน")
            line = line.rstrip("\n")
            if line == ".":
                return lines, tx
            if line.startswith("S "):
                tx = bytes.fromhex(line[2:])
            elif line == "S":
                tx = b""
            else:
                lines.append(line)

    def send(self, *parts):
        self.proc.stdin.write(" ".join(str(p) for p in parts) + "\n")
        self.proc.stdin.flush()
        return self._read()

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def frames(tx):
    """แยก byte ขาออกเป็นเฟรม (inner) กับข้อความ log — log ท้ายสุดอาจไม่มี 0x00 ปิด"""
    inner, logs = [], b""
    for chunk in tx.split(b"\x00"):
        if not chunk:
            continue
        event = serial_receive(chunk + b"\x00", 4096)["events"][0]
        if "frame" in event:
            inner.append(bytes.fromhex(event["frame"]))
        else:
            logs += chunk
    return inner, logs.decode("utf-8", "replace")


def body_of(inner):
    return inner[1:]  # flags 0 (USB ไม่เข้ารหัส ไม่มี addr)


def request(t, mid, m):
    return serial_frame(b"\x00" + cbor([t, mid, m])).hex()


def error_body(mid, code, msg=None):
    return cbor([7, mid, {0: code, **({1: msg} if msg else {})}])


# ---------- กลุ่มทดสอบ ----------

def test_crc_and_framing(h):
    framing = load("framing.json")
    for v in framing["crc16_ccitt_false"]:
        lines, _ = h.send("K", v["data"] or "-")
        got16 = lines[0].split()[1] if lines else ""
        check(f"crc16 {v['data'][:16]}", got16 == v["crc"], got16)
    for v in framing["crc32"]:
        lines, _ = h.send("K", v["data"])
        check("crc32", lines[0].split()[2] == v["crc"], lines[0])
    for v in framing["cobs"]:
        raw = bytes.fromhex(v["raw"])
        _, tx = h.send("F", v["raw"])
        want = b"\x00" + cobs_encode(raw + crc16_ccitt_false(raw).to_bytes(2, "big")) + b"\x00"
        check(f"frame_tx len={len(raw)}", tx == want, tx.hex()[:60])
    for s in framing["serial_streams"]:
        lines, _ = h.send("X", s["max_frame"], s["stream"] or "-")
        got = [ln for ln in lines if not ln.startswith("PENDING")]
        want = [f"FRAME {e['frame']}" if "frame" in e else f"DROP {e['drop']}" for e in s["events"]]
        check(f"serial_stream {s['name']}", got == want, f"{got} != {want}")
        check(f"serial_stream {s['name']} pending", lines[-1] == f"PENDING {s['pending_bytes']}", lines[-1])


def test_decoder(h, messages):
    """ตัวอ่าน CBOR ของบอร์ด: ทุกข้อความถอดแล้วเขียนกลับต้องได้ byte เดิม, รูปที่ผู้ส่งอาจส่งมาต้องได้ค่าเดียวกัน"""
    for name, m in messages.items():
        lines, _ = h.send("V", m["body"])
        check(f"roundtrip {name}", lines == [f"CANON {m['body']}"], lines)
    for case in load("decode.json")["cases"]:
        # unknown_key_skipped: ตัวอ่านต้องข้าม key ที่ไม่รู้จัก ซึ่งการเขียนกลับทั้งก้อนวัดไม่ได้ — test_device ตรวจแทน
        if case["expect"] != "ok" or case["name"] == "unknown_key_skipped":
            continue
        lines, _ = h.send("V", case["body"])
        check(f"decode {case['name']}", lines == [f"CANON {messages[case['same_as']]['body']}"], lines)


def expect_reply(h, label, req_hex, want_body):
    _, tx = h.send("R", req_hex)
    inner, logs = frames(tx)
    check(label, len(inner) == 1 and body_of(inner[0]) == want_body,
          f"got {[i.hex() for i in inner]} logs={logs!r} want {want_body.hex()}")


def test_device(messages):
    h = Harness()
    check("boot is silent", h.boot_tx == b"", h.boot_tx)
    serial = {name: m["serial"] for name, m in messages.items()}

    h.send("T", 1000)
    for req, resp in [("hello", "hello_ack"), ("describe", "describe_r"), ("call_led_set", "result_led_set"),
                      ("call_temp_read", "result_temp_read")]:
        _, tx = h.send("R", serial[req])
        check(f"{req} -> {resp}", tx.hex() == serial[resp], tx.hex())
    check("led on", h.send("Q", 2)[0] == ["PIN 1"])

    # แบ่งหน้า: led.set ใส่หน้าแรกได้ แต่ led.set + temp.read ไม่ได้ (24 = หัวของ describe_r ที่ไลบรารีกันไว้)
    led = len(messages["describe_r_page0"]["body"]) // 2 - len(cbor([4, 2, {0: [], 2: 1}]))
    temp_and_button = len(messages["describe_r_page1"]["body"]) // 2 - len(cbor([4, 3, {0: [], 1: []}]))
    page = max(led, temp_and_button) + 24
    check("page size splits LED from temp", page - 24 < len(messages["describe_r"]["body"]) // 2 - 10, page)
    h.send("PAGE", page)
    for req, resp in [("describe", "describe_r_page0"), ("describe_page1", "describe_r_page1")]:
        _, tx = h.send("R", serial[req])
        check(f"paged {req} -> {resp}", tx.hex() == serial[resp], tx.hex())
    expect_reply(h, "describe page past end", request(3, 9, {0: 2}), error_body(9, 3))
    h.send("PAGE", 4096)

    h.send("T", 123456)
    _, tx = h.send("R", serial["ping"])
    check("ping -> pong", tx.hex() == serial["pong"], tx.hex())
    h.send("T", 0xFFFFFFFF)
    _, tx = h.send("R", request(9, 9, {}))
    check("pong at millis max", body_of(frames(tx)[0][0]).hex() == messages["pong_uptime_max"]["body"], tx.hex())
    h.send("T", 200000)

    expect_reply(h, "unknown capability", request(5, 7, {0: "led.blink"}),
                 bytes.fromhex(messages["error_unknown_capability"]["body"]))
    _, tx = h.send("R", serial_frame(b"\x00" + b"\x55" * 600).hex())
    check("frame too large", tx.hex() == serial["error_frame_too_large"], tx.hex())

    # decode.json: ข้อความที่บอร์ดต้องปฏิเสธ หรือรับเหมือนข้อความปกติ
    for case in load("decode.json")["cases"]:
        req = serial_frame(b"\x00" + bytes.fromhex(case["body"])).hex()
        if case["expect"] == "error":
            expect_reply(h, f"decode {case['name']}", req, error_body(case["reply_id"], case["error"]))
        elif case["same_as"] in ("hello", "ping"):
            resp = {"hello": "hello_ack", "ping": "pong"}[case["same_as"]]
            h.send("T", 123456 if resp == "pong" else 200000)
            _, tx = h.send("R", req)
            check(f"decode {case['name']}", tx.hex() == serial[resp], tx.hex())

    # ตรวจ input ตาม schema ก่อนถึง handler
    h.send("T", 300000)
    expect_reply(h, "wrong type", request(5, 20, {0: "led.set", 1: {"on": 1}}),
                 error_body(20, 3, "'on' must be boolean"))
    expect_reply(h, "missing required", request(5, 21, {0: "led.set", 1: {}}), error_body(21, 3, "'on' is required"))
    expect_reply(h, "input not a map", request(5, 22, {0: "led.set", 1: [1]}), error_body(22, 3, "input must be an object"))
    expect_reply(h, "no input expected", request(5, 23, {0: "temp.read", 1: {"x": 1}}),
                 error_body(23, 3, "this capability takes no input"))
    expect_reply(h, "version mismatch", request(1, 24, {0: 2, 1: 3, 2: "x", 3: b"\0" * 8}),
                 cbor([7, 24, {0: 1, 2: {0: 1, 1: 1}}]))
    expect_reply(h, "pair refused", request(11, 25, {0: b"\1" * 16}),
                 error_body(25, 5, "pairing is not supported by this firmware (no wireless link)"))
    expect_reply(h, "encrypted refused", serial_frame(b"\x01" + b"\0" * 20).hex(),
                 error_body(0, 3, "encrypted frames are not supported on this link"))
    _, tx = h.send("R", serial_frame(b"\x02\x07" + cbor([9, 26, {}])).hex())
    check("other device's addr ignored", tx == b"", tx.hex())

    # event จากปุ่ม (กันเด้ง 30 ms) — ครั้งที่ 3 ต้องตรง event_button
    t = 400000
    h.send("T", t)
    h.send("R", serial["ping"])
    for press in range(1, 4):
        for level in (0, 1):
            h.send("P", 0, level)
            h.send("T", t)
            h.send("L")
            t += 40
            h.send("T", t)
            _, tx = h.send("L")
            if level == 0:
                inner, _ = frames(tx)
                want = cbor([8, 0, {0: "button.pressed", 1: {"count": press}}])
                check(f"event press {press}", len(inner) == 1 and body_of(inner[0]) == want, tx.hex())
                if press == 3:
                    check("event_button vector", tx.hex() == serial["event_button"], tx.hex())

    # heartbeat: ไม่ได้ยิน host เกิน 3 × 1000 ms → onLinkLost ปิด LED
    h.send("R", serial["call_led_set"])
    h.send("T", t + 3000)
    _, tx = h.send("L")
    check("no link loss at exactly 3x heartbeat", tx == b"", tx)
    h.send("T", t + 3001)
    _, tx = h.send("L")
    check("link lost logged", frames(tx)[1] == "SDP: link lost - onLinkLost() called\n", tx)
    check("link lost turned LED off", h.send("Q", 2)[0] == ["PIN 0"])
    h.send("P", 0, 0)
    h.send("T", t + 4000)
    h.send("L")
    h.send("T", t + 4100)
    _, tx = h.send("L")
    check("no event while unlinked", tx == b"", tx.hex())
    h.send("P", 0, 1)

    h.send("U")
    expect_reply(h, "actuator needs onLinkLost", request(5, 30, {0: "led.set", 1: {"on": True}}),
                 error_body(30, 5, "firmware must set onLinkLost() before exposing actuator capabilities"))
    h.close()


def test_output_and_schema():
    h = Harness()
    lines, tx = h.send("TESTCAPS")
    check("test caps begin", lines == ["BEGIN 1"], lines)
    h.send("T", 1000)
    expect_reply(h, "array > 23 items + nested", request(5, 1, {0: "test.big"}),
                 cbor([6, 1, {0: {"list": list(range(30)), "nested": {"ok": True, "neg": -5, "ratio": 0.1}}}]))
    _, tx = h.send("R", request(5, 2, {0: "test.order"}))
    inner, logs = frames(tx)
    # ส่งตามลำดับที่ handler ใส่ ({"long": 1, "a": 2}) — host รับได้ทุกลำดับ แต่ผู้เขียน firmware ควรรู้
    check("misordered keys still sent", inner == [b"\x00" + bytes.fromhex("830602a100a2646c6f6e6701616102")], tx.hex())
    check("misordered keys warned", "output keys out of order" in logs, logs)
    expect_reply(h, "output overflow", request(5, 3, {0: "test.overflow"}),
                 error_body(3, 5, "output larger than outputBytes in SdpConfig"))
    expect_reply(h, "handler fail", request(5, 4, {0: "test.fail"}), error_body(4, 4, "motor still moving"))

    ok = {"duty": 255, "label": "abcd", "blob": b"\1" * 6, "list": [1, 2.5]}
    expect_reply(h, "range ok", request(5, 5, {0: "test.range", 1: ok}), cbor([6, 5, {0: {"duty": 255, "label": "abcd"}}]))
    for i, (field, value, msg) in enumerate([
        ("duty", 256, "'duty' is above maximum"),
        ("duty", -1, "'duty' is below minimum"),
        ("duty", 1.5, "'duty' must be an integer"),
        ("label", "abcde", "'label' is longer than maxLength"),
        ("label", "กขค", None),  # 3 ตัวอักษร (9 ไบต์) ไม่เกิน 4
        ("blob", b"\1" * 7, "'blob' is longer than maxLength"),
        ("blob", "text", "'blob' must be bytes"),
        ("list", [1, 2, 3], "'list' has more items than maxItems"),
        ("list", ["x"], "'list' must be a number"),
    ]):
        mid = 40 + i
        req = request(5, mid, {0: "test.range", 1: {**ok, field: value}})
        _, tx = h.send("R", req)
        inner, _ = frames(tx)
        body = body_of(inner[0]) if inner else b""
        if msg is None:
            check(f"range {field}={value!r}", body.startswith(bytes([0x83, 0x06]) + cbor(mid)), body.hex())
        else:
            check(f"range {field}={value!r}", body == error_body(mid, 3, msg), body.hex())
    h.close()

    h = Harness()
    lines, tx = h.send("BADCAPS")
    check("bad schema fails begin", lines == ["BEGIN 0"], lines)
    log = frames(tx)[1]
    for expected in ["bad.json input_schema: invalid JSON", "bad.keyword input_schema: keyword not supported",
                     "bad.depth input_schema: schema nested deeper than 3 levels",
                     "bad.top input_schema: input_schema must have \"type\":\"object\""]:
        check(f"lint '{expected}'", expected in log, log)
    expect_reply(h, "hello refused when not ready", request(1, 1, {0: 1, 1: 1}),
                 error_body(1, 5, "firmware descriptor error - see device serial log"))
    h.close()


def main():
    build()
    messages = {m["name"]: m for m in load("messages.json")["messages"]}
    h = Harness()
    test_crc_and_framing(h)
    test_decoder(h, messages)
    h.close()
    test_device(messages)
    test_output_and_schema()

    print("ข้าม: crypto.json — firmware รุ่นนี้ยังไม่มีลิงก์ไร้สาย (เฟส 2)")
    if failures:
        print(f"ไม่ผ่าน {len(failures)} ข้อ:")
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print(f"ผ่านทั้งหมด {checked[0]} ข้อ")


if __name__ == "__main__":
    main()

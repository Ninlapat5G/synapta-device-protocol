"""สร้าง test vectors ของ SDP v1 จาก docs/wire-format.md

รัน: python spec/vectors/generate.py   (ต้องมีแพ็กเกจ cryptography สำหรับ AES-CCM)
ผลลัพธ์: messages.json, framing.json, decode.json, crypto.json ในโฟลเดอร์เดียวกัน
แก้สเปกเมื่อไหร่ แก้ไฟล์นี้แล้วรันใหม่ แล้ว commit ไฟล์ json ไปพร้อมกัน
"""

import hashlib
import hmac
import json
import struct
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

from refimpl import (
    _cobs_decode_bounded, _head, cbor, cobs_decode, cobs_encode, crc16_ccitt_false, crc32, diag,
    serial_frame, serial_receive, tcp_frame,
)

OUT = Path(__file__).parent
SPEC = "SDP v1 (docs/wire-format.md)"


def sanity_checks():
    """ตรวจตัวอ้างอิงกับค่าที่รู้คำตอบจากแหล่งภายนอก ก่อนเอาไปสร้าง vector"""
    assert crc16_ccitt_false(b"123456789") == 0x29B1  # check value ของ CRC-16/CCITT-FALSE
    assert crc32(b"123456789") == 0xCBF43926
    # ตัวอย่างจาก RFC 8949 Appendix A
    assert cbor(0) == b"\x00" and cbor(23) == b"\x17" and cbor(24) == b"\x18\x18"
    assert cbor(1000000) == bytes.fromhex("1a000f4240")
    assert cbor(-1000) == bytes.fromhex("3903e7")
    assert cbor(100000.0) == bytes.fromhex("fa47c35000")
    assert cbor(1.1) == bytes.fromhex("fb3ff199999999999a")
    assert cbor({1: 2, 3: 4}) == bytes.fromhex("a201020304")
    assert cbor("IETF") == bytes.fromhex("6449455446")
    for raw, enc in COBS_KNOWN:
        assert cobs_encode(raw) == enc, raw.hex()
        assert cobs_decode(enc) == raw
        assert _cobs_decode_bounded(enc, 4096) == raw


def seq(a, b):
    return bytes(range(a, b + 1))


# ตัวอย่างจาก Wikipedia "Consistent Overhead Byte Stuffing" (ไม่รวม 0x00 ตัวปิด)
COBS_KNOWN = [
    (b"\x00", b"\x01\x01"),
    (b"\x00\x00", b"\x01\x01\x01"),
    (b"\x00\x11\x00", b"\x01\x02\x11\x01"),
    (b"\x11\x22\x00\x33", b"\x03\x11\x22\x02\x33"),
    (b"\x11\x22\x33\x44", b"\x05\x11\x22\x33\x44"),
    (b"\x11\x00\x00\x00", b"\x02\x11\x01\x01\x01"),
    (seq(1, 254), b"\xff" + seq(1, 254)),
    (b"\x00" + seq(1, 254), b"\x01\xff" + seq(1, 254)),
    (seq(1, 255), b"\xff" + seq(1, 254) + b"\x02\xff"),
    (seq(2, 255) + b"\x00", b"\xff" + seq(2, 255) + b"\x01\x01"),
    (seq(3, 255) + b"\x00\x01", b"\xfe" + seq(3, 255) + b"\x02\x01"),
]


# ---------- อุปกรณ์ตัวอย่าง: ESP32 มี LED, วัดอุณหภูมิ, ปุ่ม BOOT ----------

HOST_NONCE = seq(1, 8)
DEV_NONCE = seq(0x11, 0x18)
PSK = seq(0, 15)
DEVICE_ID = bytes.fromhex("a1b2c3d4e5f60718")
BOOT_ID = bytes.fromhex("0badf00d")

CAP_LED = {
    0: "led.set",
    1: "Turn the on-board LED on or off",
    2: {"type": "object", "properties": {"on": {"type": "boolean"}}, "required": ["on"]},
    4: 1,
}
CAP_TEMP = {
    0: "temp.read",
    1: "Read board temperature in Celsius",
    3: {"type": "object", "properties": {"celsius": {"type": "number"}}},
}
EVT_BUTTON = {
    0: "button.pressed",
    1: "BOOT button pressed",
    2: {"type": "object", "properties": {"count": {"type": "integer", "minimum": 0}}},
}
CAPS_HASH = crc32(cbor(CAP_LED) + cbor(CAP_TEMP) + cbor(EVT_BUTTON))

KEY_PROOF = hmac.new(PSK, b"proof" + HOST_NONCE + DEV_NONCE, hashlib.sha256).digest()[:8]
SESSION_KEY = hmac.new(PSK, b"key" + HOST_NONCE + DEV_NONCE, hashlib.sha256).digest()[:16]

HELLO_ACK_BASE = {
    0: 1, 1: 1, 2: 1, 3: DEVICE_ID, 4: "esp32-devkit", 5: "led-demo 1.0.0",
    6: 512, 7: CAPS_HASH, 8: 1000, 9: BOOT_ID, 10: False,
}

# (ชื่อ, t, id, m, หมายเหตุ)
MESSAGES = [
    ("hello", 1, 1, {0: 1, 1: 1, 2: "SynaptaCode", 3: HOST_NONCE}, ""),
    ("hello_ack", 2, 1, HELLO_ACK_BASE, "ยังไม่จับคู่"),
    ("hello_ack_paired", 2, 1,
     {**HELLO_ACK_BASE, 10: True, 11: DEV_NONCE, 12: KEY_PROOF}, "key_proof มาจาก crypto.json"),
    ("error_version_mismatch", 7, 1, {0: 1, 2: {0: 2, 1: 3}}, "อุปกรณ์รองรับ v2–v3 แต่ host ขอ v1"),
    ("describe", 3, 2, {0: 0}, ""),
    ("describe_r", 4, 2, {0: [CAP_LED, CAP_TEMP], 1: [EVT_BUTTON]}, "หน้าเดียวจบ"),
    ("describe_r_page0", 4, 2, {0: [CAP_LED], 2: 1}, "แบ่งเป็นสองหน้า: caps_hash ต้องเท่ากับแบบหน้าเดียว"),
    ("describe_page1", 3, 3, {0: 1}, ""),
    ("describe_r_page1", 4, 3, {0: [CAP_TEMP], 1: [EVT_BUTTON]}, ""),
    ("call_led_set", 5, 4, {0: "led.set", 1: {"on": True}}, ""),
    ("result_led_set", 6, 4, {0: {"on": True}}, ""),
    ("call_temp_read", 5, 5, {0: "temp.read"}, "ไม่มี input = ไม่ส่ง key 1"),
    ("result_temp_read", 6, 5, {0: {"celsius": 23.5}}, "23.5 แทนด้วย float 32 บิตได้ตรง จึงส่ง 32"),
    ("result_float64", 6, 12, {0: {"ratio": 0.1}}, "0.1 แทนด้วย 32 บิตไม่ตรง จึงส่ง 64"),
    ("result_empty", 6, 6, {}, "คำสั่งที่ไม่มี output"),
    ("error_unknown_capability", 7, 7, {0: 2, 1: "no capability 'led.blink'"}, ""),
    ("error_frame_too_large", 7, 0, {0: 6}, "id = 0 เพราะอ่าน id ของเฟรมที่ทิ้งไม่ได้"),
    ("event_button", 8, 0, {0: "button.pressed", 1: {"count": 3}}, ""),
    ("ping", 9, 8, {}, ""),
    ("pong", 10, 8, {0: 123456, 1: BOOT_ID}, ""),
    ("pong_uptime_max", 10, 9, {0: 0xFFFFFFFF, 1: BOOT_ID}, "ก่อน millis() วนกลับ — ใช้ boot_id ตรวจรีเซ็ต"),
    ("pair", 11, 10, {0: PSK}, "USB เท่านั้น"),
    ("pair_ack", 12, 10, {}, ""),
    ("unpair", 13, 11, {}, "USB เท่านั้น"),
    ("id_max", 9, 65535, {}, "id สูงสุด 16 บิต"),
]


def body_of(name):
    for n, t, i, m, _ in MESSAGES:
        if n == name:
            return cbor([t, i, m])
    raise KeyError(name)


def build_messages():
    out = []
    for name, t, mid, m, note in MESSAGES:
        body = cbor([t, mid, m])
        inner = b"\x00" + body
        entry = {"name": name, "diag": diag([t, mid, m]), "body": body.hex(),
                 "serial": serial_frame(inner).hex(), "tcp": tcp_frame(inner).hex()}
        if note:
            entry["note"] = note
        out.append(entry)
    return {"spec": SPEC, "caps_hash": f"{CAPS_HASH:08x}",
            "caps_hash_input": "cbor(CAP_LED) | cbor(CAP_TEMP) | cbor(EVT_BUTTON)",
            "messages": out}


# ---------- framing ----------

def frame(inner):
    return serial_frame(inner)


def raw_chunk(data_with_crc):
    """เฟรมที่ประกอบเอง (CRC ผิดได้) — ใส่ 0x00 หน้าหลังให้แล้ว"""
    return b"\x00" + cobs_encode(data_with_crc) + b"\x00"


def with_crc(inner):
    return inner + crc16_ccitt_false(inner).to_bytes(2, "big")


def build_framing():
    ping = b"\x00" + body_of("ping")
    hello = b"\x00" + body_of("hello")
    long_inner = b"\x00" + bytes((i % 255) + 1 for i in range(300)) + b"\x00\x00" + seq(1, 50)

    bad_crc = bytearray(with_crc(ping))
    bad_crc[-1] ^= 0x01
    cut = frame(hello)[: len(frame(hello)) // 2]

    streams = [
        ("single_frame", 128, frame(ping), ""),
        ("two_frames_back_to_back", 128, frame(ping) + frame(hello), "0x00 ติดกันสองตัวระหว่างเฟรม = ท่อนว่าง ข้าม"),
        ("log_then_frame", 128, b"boot ok\r\n" + frame(ping),
         "log จาก Serial.println แยกออกเพราะผู้ส่งใส่ 0x00 หน้าเฟรม"),
        ("log_glued_without_leading_zero", 128, b"boot ok\r\n" + frame(ping)[1:],
         "ผู้ส่งที่ไม่ใส่ 0x00 หน้าเฟรม — เฟรมจริงหายไปกับ log (เหตุที่สเปกบังคับ 0x00 หน้าเฟรม)"),
        ("crc_mismatch", 128, raw_chunk(bytes(bad_crc)), ""),
        ("cut_mid_then_good", 128, cut + frame(ping), "ท่อนที่ขาดถูกทิ้ง เฟรมถัดไปต้องรับได้"),
        ("incomplete_at_end", 128, frame(ping) + frame(hello)[:-1], "ยังไม่เจอ 0x00 ปิดท้าย = รอต่อ ไม่ใช่ทิ้ง"),
        ("too_short", 128, b"\x00\x03\xaa\xbb\x00", ""),
        ("max_frame_exact", 128, frame(b"\x00" + b"\x55" * 127), "ยาวพอดี max_frame รับได้"),
        ("max_frame_plus_one", 128, frame(b"\x00" + b"\x55" * 128), "เกินหนึ่งไบต์ ทิ้ง"),
        ("reserved_flag", 128, frame(b"\x04" + body_of("ping")), "bit 2 ของ flags"),
        ("only_delimiters", 128, b"\x00\x00\x00", ""),
        ("rs485_addr", 128, frame(b"\x02\x05" + body_of("ping")), "flags bit 1 + addr 5 — framing ไม่แกะ addr"),
        ("encrypted_flag_passes_framing", 128, frame(b"\x01" + seq(1, 20)),
         "framing ไม่ถอดรหัส ส่งต่อให้ชั้นถัดไป"),
        ("long_frame_cobs_blocks", 512, frame(long_inner), "ข้ามขอบบล็อก COBS 254 ไบต์"),
    ]
    stream_vectors = []
    for name, max_frame, data, note in streams:
        v = {"name": name, "max_frame": max_frame, "stream": data.hex(), **serial_receive(data, max_frame)}
        if note:
            v["note"] = note
        stream_vectors.append(v)

    return {
        "spec": SPEC,
        "crc16_ccitt_false": [{"data": b"123456789".hex(), "crc": "29b1"},
                              {"data": "", "crc": "ffff"},
                              {"data": ping.hex(), "crc": f"{crc16_ccitt_false(ping):04x}"}],
        "crc32": [{"data": b"123456789".hex(), "crc": "cbf43926"}],
        "cobs": [{"raw": r.hex(), "encoded": e.hex()} for r, e in COBS_KNOWN],
        "serial_streams": stream_vectors,
        "note": "เหตุการณ์ frame = flags | [addr] | body; drop.raw = ท่อนก่อนถอด COBS (host เก็บเป็น console log)",
    }


# ---------- ถอดข้อความ: กฎฝั่งผู้รับ ----------

def raw_map(pairs):
    """map ที่ไม่เรียง key — ใช้ทดสอบว่าผู้รับไม่พึ่งลำดับ"""
    return _head(5, len(pairs)) + b"".join(cbor(k) + cbor(v) for k, v in pairs)


def build_decode():
    hello_m = [(3, HOST_NONCE), (2, "SynaptaCode"), (1, 1), (0, 1)]
    cases = [
        ("keys_out_of_order", b"\x83\x01\x01" + raw_map(hello_m), {"same_as": "hello"}),
        ("unknown_key_skipped", b"\x83\x01\x01" + raw_map([(0, 1), (1, 1), (2, "SynaptaCode"),
                                                          (3, HOST_NONCE), (99, {"x": [1, 2]})]),
         {"same_as": "hello"}),
        ("non_shortest_int", b"\x83\x18\x09\x19\x00\x08\xa0", {"same_as": "ping"}),
        ("float16_value", b"\x83\x06\x05\xa1\x00\xa1\x67celsius\xf9" + struct.pack(">e", 23.5),
         {"same_as": "result_temp_read"}),
        ("float64_value", b"\x83\x06\x05\xa1\x00\xa1\x67celsius\xfb" + struct.pack(">d", 23.5),
         {"same_as": "result_temp_read"}),
        ("unknown_message_type", cbor([99, 7, {}]), {"error": 7, "reply_id": 7}),
        ("unknown_type_with_bad_m", cbor([99, 7, []]), {"error": 3, "reply_id": 7}),
        ("missing_required_field", cbor([5, 4, {1: {"on": True}}]), {"error": 3, "reply_id": 4}),
        ("indefinite_array", b"\x9f\x09\x08\xa0\xff", {"error": 3, "reply_id": 0}),
        ("indefinite_map", b"\x83\x09\x08\xbf\xff", {"error": 3, "reply_id": 8}),
        ("indefinite_text_in_call", b"\x83\x05\x04\xa1\x00\x7f\x63led\x64.set\xff", {"error": 3, "reply_id": 4}),
        ("array_of_two", cbor([9, 8]), {"error": 3, "reply_id": 0}),
        ("not_an_array", cbor({0: 9}), {"error": 3, "reply_id": 0}),
        ("type_is_text", cbor(["ping", 8, {}]), {"error": 3, "reply_id": 0}),
        ("id_over_16_bits", cbor([9, 65536, {}]), {"error": 3, "reply_id": 0}),
        ("m_not_a_map", cbor([9, 8, []]), {"error": 3, "reply_id": 8}),
        ("truncated", body_of("hello")[:-3], {"error": 3, "reply_id": 1}),
        ("trailing_byte", body_of("ping") + b"\x00", {"error": 3, "reply_id": 8}),
    ]
    out = []
    for name, body, expect in cases:
        out.append({"name": name, "body": body.hex(), **({"expect": "ok"} if "same_as" in expect else
                                                          {"expect": "error"}), **expect})
    return {
        "spec": SPEC,
        "note": "same_as = ถอดแล้วต้องได้ค่าเท่ากับข้อความชื่อนั้นใน messages.json; "
                "error = รหัสที่อุปกรณ์ตอบ, reply_id = id ที่ใส่ในคำตอบ (0 เมื่ออ่าน id ไม่ได้)",
        "cases": out,
    }


# ---------- การเข้ารหัส ----------

def encrypt(direction, counter, body, flags=0x01):
    nonce = bytes([direction]) + counter.to_bytes(4, "big") + bytes(8)
    sealed = AESCCM(SESSION_KEY, tag_length=8).encrypt(nonce, body, bytes([flags]))
    return bytes([flags]) + counter.to_bytes(4, "big") + sealed


def build_crypto():
    call_body = body_of("call_led_set")
    result_body = body_of("result_led_set")
    ping_body = body_of("ping")

    host_frames = [encrypt(0, n, b) for n, b in ((1, call_body), (2, ping_body), (3, ping_body))]
    tampered = bytearray(host_frames[2])
    tampered[-1] ^= 0x01
    reflected = encrypt(1, 3, ping_body)  # เฟรมทิศ dev→host ถูกส่งย้อนกลับเข้าอุปกรณ์

    sequence = [
        ("counter_1", host_frames[0], {"ok": call_body.hex()}),
        ("counter_2", host_frames[1], {"ok": ping_body.hex()}),
        ("replay_counter_2", host_frames[1], {"drop": "replay"}),
        ("replay_counter_1", host_frames[0], {"drop": "replay"}),
        ("tampered_tag_counter_3", bytes(tampered), {"drop": "auth_failed"}),
        ("reflected_direction", reflected, {"drop": "auth_failed"}),
        ("genuine_counter_3_after_failures", host_frames[2], {"ok": ping_body.hex()}),
    ]
    return {
        "spec": SPEC,
        "psk": PSK.hex(), "host_nonce": HOST_NONCE.hex(), "dev_nonce": DEV_NONCE.hex(),
        "key_proof": KEY_PROOF.hex(), "session_key": SESSION_KEY.hex(),
        "frames": [
            {"name": "host_to_dev_call", "direction": 0, "counter": 1, "plain_body": call_body.hex(),
             "inner": host_frames[0].hex(), "serial": serial_frame(host_frames[0]).hex(),
             "tcp": tcp_frame(host_frames[0]).hex()},
            {"name": "dev_to_host_result", "direction": 1, "counter": 1, "plain_body": result_body.hex(),
             "inner": encrypt(1, 1, result_body).hex()},
        ],
        "device_receive_sequence": [{"name": n, "inner": f.hex(), **e} for n, f, e in sequence],
        "note": "inner = flags | counter | ciphertext | tag; device_receive_sequence ป้อนตามลำดับเข้าอุปกรณ์ตัวเดียว "
                "— เฟรมที่ถอดไม่ผ่านต้องไม่ขยับ counter ไม่งั้นผู้โจมตีส่งเฟรมปลอม counter สูง ๆ แล้วเฟรมจริงถูกทิ้งหมด",
    }


def write(name, data):
    path = OUT / name
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path.name}")


if __name__ == "__main__":
    sanity_checks()
    write("messages.json", build_messages())
    write("framing.json", build_framing())
    write("decode.json", build_decode())
    write("crypto.json", build_crypto())

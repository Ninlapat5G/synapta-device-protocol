"""ตัวเข้ารหัส/ถอดอ้างอิงของ SDP v1 ที่ใช้สร้าง test vectors เท่านั้น

เขียนด้วย Python stdlib ล้วน แยกจากโค้ด Rust/C++ โดยตั้งใจ — ถ้าตีความสเปกไม่ตรงกัน เทสต์ของอีกฝั่งจะตก
ไม่ได้ใช้ตอนรันจริง ความเร็วไม่สำคัญ อ่านง่ายสำคัญกว่า
"""

import struct
import zlib


# ---------- CBOR แบบ deterministic (RFC 8949 §4.2.1) ----------

def _head(major: int, n: int) -> bytes:
    if n < 24:
        return bytes([major << 5 | n])
    for ai, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if n < 1 << (8 * size):
            return bytes([major << 5 | ai]) + n.to_bytes(size, "big")
    raise ValueError("เลขใหญ่เกิน 64 บิต")


def cbor(v) -> bytes:
    if isinstance(v, bool):  # ต้องเช็กก่อน int เพราะ bool เป็น int ใน Python
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return _head(0, v) if v >= 0 else _head(1, -1 - v)
    if isinstance(v, float):
        # 32 บิตเมื่อแทนค่าได้ตรง ไม่งั้น 64 (AVR มีแค่ float 32 บิต)
        as32 = struct.pack(">f", v)
        if struct.unpack(">f", as32)[0] == v:
            return b"\xfa" + as32
        return b"\xfb" + struct.pack(">d", v)
    if isinstance(v, bytes):
        return _head(2, len(v)) + v
    if isinstance(v, str):
        b = v.encode("utf-8")
        return _head(3, len(b)) + b
    if isinstance(v, list):
        return _head(4, len(v)) + b"".join(cbor(x) for x in v)
    if isinstance(v, dict):
        items = sorted((cbor(k), cbor(x)) for k, x in v.items())  # เรียงตาม byte ของ key
        return _head(5, len(items)) + b"".join(k + x for k, x in items)
    raise TypeError(f"เข้ารหัส {type(v)} ไม่ได้")


def diag(v) -> str:
    """CBOR diagnostic notation (RFC 8949 §8) ให้คนอ่าน vector ออก"""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v) + ("_2" if cbor(v)[0] == 0xFA else "_3")
    if isinstance(v, bytes):
        return f"h'{v.hex()}'"
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, list):
        return "[" + ", ".join(diag(x) for x in v) + "]"
    if isinstance(v, dict):
        items = sorted(v.items(), key=lambda kv: cbor(kv[0]))
        return "{" + ", ".join(f"{diag(k)}: {diag(x)}" for k, x in items) + "}"
    raise TypeError(type(v))


# ---------- CRC ----------

def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------- COBS ----------

def cobs_encode(data: bytes) -> bytes:
    """COBS มาตรฐาน: บล็อกเต็ม 254 ไบต์ที่จบพอดีท้ายข้อมูล ไม่เติมรหัส 0x01 ต่อท้าย"""
    out = bytearray()
    block = bytearray()
    ended_on_full_block = False
    for byte in data:
        ended_on_full_block = False
        if byte == 0:
            out += bytes([len(block) + 1]) + block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out += b"\xff" + block
                block.clear()
                ended_on_full_block = True
    if not ended_on_full_block:
        out += bytes([len(block) + 1]) + block
    return bytes(out)


def cobs_decode(chunk: bytes) -> bytes | None:
    """คืน None ถ้ารหัสชี้เลยท้ายท่อน หรือเจอ 0x00 ข้างใน"""
    out = bytearray()
    i = 0
    while i < len(chunk):
        code = chunk[i]
        if code == 0 or i + code > len(chunk):
            return None
        block = chunk[i + 1 : i + code]
        if 0 in block:
            return None
        out += block
        i += code
        if code != 0xFF and i < len(chunk):
            out.append(0)
    return bytes(out)


# ---------- เฟรม ----------

def serial_frame(inner: bytes) -> bytes:
    """inner = flags | [addr] | body → 0x00 COBS(inner | CRC) 0x00"""
    crc = crc16_ccitt_false(inner).to_bytes(2, "big")
    return b"\x00" + cobs_encode(inner + crc) + b"\x00"


def tcp_frame(inner: bytes) -> bytes:
    return len(inner).to_bytes(2, "big") + inner


def serial_receive(stream: bytes, max_frame: int) -> dict:
    """ผู้รับ serial ตามลำดับการตรวจใน wire-format.md — คืนเหตุการณ์ และจำนวน byte ที่ค้างรอ 0x00"""
    events = []
    *chunks, pending = stream.split(b"\x00")
    for chunk in chunks:
        if not chunk:
            continue
        events.append(_check_chunk(chunk, max_frame))
    return {"events": events, "pending_bytes": len(pending)}


def _cobs_decode_bounded(chunk: bytes, limit: int):
    """ถอด COBS ทีละ byte แบบผู้รับจริง: เกิน limit = "too_large" ทันที, จบไม่พอดี = None"""
    out = bytearray()
    remaining = 0
    pending_zero = False
    for byte in chunk:
        if remaining == 0:  # byte นี้คือรหัสของบล็อกใหม่
            if pending_zero:
                out.append(0)
            remaining = byte - 1
            pending_zero = byte != 0xFF
        else:
            out.append(byte)
            remaining -= 1
        if len(out) > limit:
            return "too_large"
    if remaining:
        return None
    return bytes(out)


def _check_chunk(chunk: bytes, max_frame: int) -> dict:
    raw = chunk.hex()
    data = _cobs_decode_bounded(chunk, max_frame + 2)
    if data == "too_large":
        return {"drop": "too_large"}  # ผู้รับจริงไม่มี buffer พอเก็บ raw
    if data is None:
        return {"drop": "cobs_invalid", "raw": raw}
    if len(data) < 3:
        return {"drop": "too_short", "raw": raw}
    inner, crc = data[:-2], data[-2:]
    if crc16_ccitt_false(inner).to_bytes(2, "big") != crc:
        return {"drop": "crc_mismatch", "raw": raw}
    if inner[0] & 0xFC:
        return {"drop": "reserved_flag", "raw": raw}
    return {"frame": inner.hex()}

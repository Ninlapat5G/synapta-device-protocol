# Test vectors ของ SDP v1

สัญญากลางระดับ byte: `sdp-core` (Rust) และ firmware ทุกภาษาต้องผ่านชุดเดียวกัน
ถ้าโค้ดไม่ตรงกับ vector ให้ถือว่าโค้ดผิดก่อน ถ้าแน่ใจว่า vector ผิด ให้แก้ `docs/wire-format.md` + `generate.py` แล้วรันใหม่

```
python spec/vectors/generate.py     # ต้องมี cryptography (pip install cryptography)
```

ค่า hex ทุกตัวเป็นตัวพิมพ์เล็ก ไม่มีช่องว่าง

| ไฟล์ | ทดสอบอะไร | วิธีใช้ในเทสต์ |
|---|---|---|
| `messages.json` | ข้อความถูกต้องทุกชนิด: `body`, เฟรม `serial`, เฟรม `tcp` (flags = 0) | สร้างข้อความตาม `diag` → เข้ารหัสต้องได้ `body` ตรงทุก byte, ถอด `body` ต้องได้ข้อความเดิม |
| `framing.json` | CRC, COBS, ผู้รับ serial กับเฟรมเสีย (CRC ผิด, ยาวเกิน, ตัดกลาง, log ปน) | ป้อน `stream` เข้าผู้รับที่ตั้ง `max_frame` → ต้องได้ `events` ตามลำดับ และค้าง `pending_bytes` |
| `decode.json` | กฎฝั่งผู้รับ: key สลับลำดับ, key ไม่รู้จัก, float 16/64, indefinite length, body ผิดรูป | `ok` → ต้องเท่ากับข้อความ `same_as`; `error` → รหัสที่ต้องตอบ และ `reply_id` |
| `crypto.json` | `key_proof`, session key, เฟรม AES-CCM, กันเล่นซ้ำ | ป้อน `device_receive_sequence` ตามลำดับเข้าผู้รับตัวเดียว |

**ผู้ใช้ตอนนี้:** `crates/sdp-core/tests/vectors.rs` (ครบทุกไฟล์) และ `firmware/arduino/test/run_vectors.py`
(ข้าม `crypto.json` จนกว่า firmware จะมีลิงก์ไร้สาย — เฟส 2)

**เทสต์ต้องครอบทุกชื่อ:** ให้วนทุกรายการในไฟล์ แล้วตกเมื่อเจอชื่อที่เทสต์ไม่รู้จัก vector ใหม่จะได้ไม่ถูกข้ามเงียบ ๆ

`refimpl.py` เป็นตัวอ้างอิงที่ใช้สร้าง vector เท่านั้น ตรวจกับค่าที่รู้คำตอบ (RFC 8949, check value ของ CRC,
ตัวอย่าง COBS) ก่อนสร้างทุกครั้ง

**ยังไม่มี:** เฟรม TCP ที่เสีย และการตัดท่อน BLE (เฟส 2–3) นโยบายฝั่ง host เช่นปฏิเสธการถอยไปไม่เข้ารหัส
ทดสอบในเทสต์พฤติกรรมของ `sdp-host` เพราะไม่ใช่เรื่องระดับ byte

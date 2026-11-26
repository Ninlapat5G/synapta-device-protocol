# Wire format — SDP v1

> ตัดสินแล้ว 2026-11-25 (decisions #10–#12) เป็นฐานของ test vectors และโค้ดเฟส 0
> แก้หลังจากนี้ต้องเพิ่มข้อใน decisions.md และอัปเดต test vectors พร้อมกัน

## ชั้นของหนึ่งเฟรม

```
serial : COBS( flags | [addr] | body | CRC-16 ) 0x00
TCP    : len(2, big-endian) | flags | body
BLE    : ตัดเป็นท่อนตาม MTU (header 1 ไบต์: bit7 = ท่อนสุดท้าย, bit0-6 = ลำดับ) แล้วต่อกลับเป็น flags | body
```

- **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF) คลุม `flags` ถึงท้าย `body` ใส่แบบ big-endian — มีเฉพาะ serial
- **`max_frame`** = เฟรมยาวสุดที่**อุปกรณ์รับได้** นับ `flags | [addr] | body` (ไม่นับ COBS, CRC, len) ต้องไม่ต่ำกว่า **128** ไบต์
  host ห้ามส่งเฟรมที่ยาวกว่านี้
- **ขาอุปกรณ์ → host จำกัดที่ 4096 ไบต์** ไม่ใช่ `max_frame` — host มี RAM เหลือ และอุปกรณ์ส่ง descriptor จาก flash
  ได้โดยไม่ต้องพักใน RAM ถ้าใช้ `max_frame` จำกัดทั้งสองขา Uno จะประกาศ capability ได้แค่ตัวเล็ก ๆ
  (descriptor ที่มี schema ของ field เดียวก็ยาวราว 100 ไบต์แล้ว)
- เปิดพอร์ตกลางทาง: ทิ้ง byte จนเจอ `0x00` แล้วเริ่มเฟรมใหม่ byte นอกเฟรมเก็บเป็น console log (transports.md กับดัก 4)
- **serial: ผู้ส่งใส่ `0x00` ทั้งหน้าและหลังเฟรม** ผู้รับข้ามท่อนว่าง — ถ้ามีแค่ตัวปิดท้าย ข้อความ `Serial.println`
  ที่พิมพ์ก่อนหน้าจะติดเป็นท่อนเดียวกับเฟรมถัดไป แล้วเฟรมจริงนั้นตก CRC หายไปทั้งเฟรม

**ผู้รับ serial ตรวจท่อนระหว่าง `0x00` ตามลำดับนี้ (ลำดับตามที่ตัวถอดแบบทีละ byte เจอได้จริง)** ท่อนที่ไม่ผ่านถูกทิ้ง (host เก็บเป็น console log พร้อมเหตุผล):

| ลำดับ | ตรวจ | เหตุผลที่ทิ้ง |
|---|---|---|
| 1 | ระหว่างถอด COBS ไปทีละ byte ยาวไม่เกินขีดของผู้รับ + 2 (อุปกรณ์: `max_frame`, host: 4096) — buffer มีแค่นี้ เกินแล้วทิ้งต่อจนถึง `0x00` ไม่เก็บ raw | `too_large` |
| 2 | ถอด COBS จบพอดี (ไม่มีรหัสชี้เลยท้ายท่อน) | `cobs_invalid` |
| 3 | ยาวอย่างน้อย 3 ไบต์ (flags + CRC) | `too_short` |
| 4 | CRC ตรง | `crc_mismatch` |
| 5 | ไม่มี bit สงวนใน `flags` | `reserved_flag` |

อุปกรณ์ที่ทิ้งเพราะ `too_large` ตอบ `error 6 frame_too_large` ด้วย `id` = 0 (อ่าน id ไม่ได้)
host ถือว่าเป็นคำตอบของคำขอที่ค้างอยู่ เพราะส่งได้ทีละหนึ่งอยู่แล้ว

### `flags` (1 ไบต์)

| bit | ความหมาย |
|---|---|
| 0 | `body` เข้ารหัส (ดู §การเข้ารหัส) |
| 1 | มี `addr` 1 ไบต์ตามมา (RS485 หลายอุปกรณ์บนสายเดียว) |
| 2–7 | สงวน ต้องเป็น 0 — **ผู้รับเจอ bit ที่ไม่รู้จัก ต้องทิ้งเฟรม** เพราะไม่รู้ว่าข้างหลังจัดวางแบบไหน |

## Body: CBOR (decisions #10)

ทุกข้อความเป็น CBOR array 3 ช่อง: **`[t, id, m]`**

- `t` — ชนิดข้อความ (uint ตามตารางข้างล่าง)
- `id` — uint 16 บิต จับคู่คำขอกับคำตอบ host วน 1..65535, `event` ใช้ 0
- `m` — map ที่ key เป็นเลข (uint) ความหมายของ key ขึ้นกับ `t`

**ทำไม array + key เลข:** key แบบข้อความ (`"name"`, `"device_id"`) กินที่ทุกเฟรม เฟรม 128 ไบต์บน Uno จะเหลือที่ใส่ข้อมูลจริงน้อย
และตำแหน่งตายตัวทำให้ parser บน MCU เป็นแค่ switch ไม่ต้องเทียบ string — ฝั่ง PC แปลงเป็นชื่อให้คนอ่านตอน debug

### กฎ encoding (ให้ test vectors เทียบ byte ได้)

ผู้ส่ง **ต้อง**:
- ใช้ความยาวแบบ definite และเลขในรูปสั้นสุด (RFC 8949 §4.2.1)
- float: 32 บิตเมื่อแทนค่าได้ตรง ไม่งั้น 64 บิต (AVR มีแค่ float 32 บิต) ไม่ส่ง 16 บิต
- เรียง key ใน map ตาม byte ของ key ที่เข้ารหัสแล้ว (RFC 8949 §4.2.1) — key เลขจึงเรียงน้อยไปมาก key ข้อความสั้นก่อนยาว
  ไม่ส่ง key ที่เป็นค่าว่าง/ไม่ใช้

ผู้รับ **ต้อง**:
- รับ key ได้ทุกลำดับ และ **ข้าม key ที่ไม่รู้จัก** — ทางเดียวที่จะเพิ่ม field ใหม่ได้โดยไม่ต้องขึ้น version
- รับเลขที่ไม่ได้อยู่ในรูปสั้นสุดได้ (กฎรูปสั้นสุดมีไว้ให้เทียบ byte ฝั่งส่ง ไม่ใช่เหตุให้ทิ้งข้อความ)
- รับ float ทั้ง 16/32/64 บิต
- ปฏิเสธ indefinite length (`error invalid_input`) — MCU ไม่ต้องมีโค้ดรองรับ
- ปฏิเสธ body ที่ไม่ใช่ CBOR item เดียวพอดี (ขาด หรือมี byte เกินท้าย) และ `id` เกิน 65535 (`error invalid_input`)
- `t` ที่ไม่รู้จัก ตอบ `error 7 unknown_message` (ตรวจหลังจากรู้ว่า body ถูกรูปแล้ว)
- **`id` ของคำตอบ error:** อ่าน array หัว 3 ช่อง, `t` เป็น uint และ `id` เป็น uint ≤ 65535 ได้ → ใช้ `id` นั้น ไม่งั้นใช้ 0
  ไม่ต้องอ่านทั้ง body ก่อนรู้ id — parser บน MCU อ่านทีละช่องอยู่แล้ว host ถือคำตอบ id 0 เป็นของคำขอที่ค้าง
- ข้อมูลไบนารีส่งเป็น CBOR byte string ตรง ไม่ต้อง base64

## ข้อความ

| t | ชื่อ | ทิศ | `m` |
|---|---|---|---|
| 1 | `hello` | host → dev | `0` sdp_min · `1` sdp_max · `2` host (text) · `3` host_nonce (bstr 8) |
| 2 | `hello_ack` | dev → host | `0` sdp_min · `1` sdp_max · `2` ver ที่เลือก · `3` device_id (bstr 8) · `4` model (text) · `5` fw (text) · `6` max_frame · `7` caps_hash (uint32) · `8` heartbeat_ms · `9` boot_id (bstr 4) · `10` paired (bool) · `11` dev_nonce (bstr 8, เมื่อ paired) · `12` key_proof (bstr 8, เมื่อ paired) |
| 3 | `describe` | host → dev | `0` page (uint, เริ่ม 0) |
| 4 | `describe_r` | dev → host | `0` caps (array ของ descriptor) · `1` events (array) · `2` next_page (ไม่มี = หน้าสุดท้าย) |
| 5 | `call` | host → dev | `0` name (text) · `1` input |
| 6 | `result` | dev → host | `0` output |
| 7 | `error` | dev → host | `0` code (uint) · `1` msg (text, ไม่บังคับ) · `2` detail (ไม่บังคับ) |
| 8 | `event` | dev → host | `0` name (text) · `1` data — `id` = 0 |
| 9 | `ping` | host → dev | ว่าง |
| 10 | `pong` | dev → host | `0` uptime_ms · `1` boot_id (bstr 4) |
| 11 | `pair` | host → dev | `0` psk (bstr 16) · `1` pairing_code (bstr 8, เลข ASCII เมื่อ firmware ขอรหัส) — **USB เท่านั้น** |
| 12 | `pair_ack` | dev → host | ว่าง |
| 13 | `unpair` | host → dev | ว่าง — **USB เท่านั้น** ตอบ `pair_ack` |
| 14 | `firmware_info` | host → dev | ว่าง อ่านหลัง `hello_ack` |
| 15 | `firmware_info_r` | dev → host | `0` project_id (text) · `1` build_id (text) · `2` pairing_mode (uint) · `3` boot_state (uint) |

### ตัวตนของเฟิร์มแวร์และการจับคู่ USB

- `device_id` ใน `hello_ack` ระบุบอร์ดเดิม ส่วน `project_id` และ `build_id` ระบุโปรเจกต์และ build ที่ฝังใน executable ทั้งสองเป็นตัวพิมพ์เล็ก ASCII อักษร/ตัวเลข/`-`/`_` ยาว 1–96 ตัว เริ่มด้วยอักษรหรือตัวเลข และข้ามชื่ออุปกรณ์สงวนของ Windows เช่น `con`, `aux`, `com1`.
- host เรียก `firmware_info` หลัง handshake ผ่าน USB หรือ session ไร้สายที่ยืนยัน PSK แล้ว อุปกรณ์ v1 เดิมตอบ `error 7` ให้ host ทำงานตามเดิมและแสดงว่าไม่ทราบ build.
- `pairing_mode` เป็น `0` ปิดการ provision, `1` ยืนยันผ่าน USB โดยไม่กรอกรหัส, หรือ `2` ยืนยันผ่าน USB พร้อมรหัส 8 หลัก เปิดรับ `pair` เฉพาะช่วง enrollment ที่บอร์ดเปิดไว้ ตรวจรหัสบนบอร์ดและจำกัดจำนวนครั้งก่อนบันทึก PSK. หลังจับคู่ ลิงก์ Wi-Fi ใช้ PSK ตามสเปกเดิม.
- `boot_state` เป็น `0` รัน build ที่ยืนยันแล้ว, `1` build ใหม่รอยืนยันหลัง self-test, หรือ `2` กลับไปรัน build ก่อนหน้าโดยมีหลักฐานจาก OTA. Host รายงาน `verified` เมื่อ `device_id` ตรง, `boot_id` เปลี่ยน และ build ที่ร้องขอกำลังรันแบบสถานะ `0`. รายงาน `rolled_back` เมื่อบอร์ดเดิมบูตใหม่ รายงานสถานะ `2` และ build ตรงกับรุ่นที่อ่านก่อนอัปเดต.

- `device_id` สุ่มครั้งแรกที่บูตแล้วเก็บใน flash/EEPROM ไม่เปลี่ยนจนกว่าจะล้าง
- `boot_id` สุ่มใหม่ทุกครั้งที่บูต — **ใช้ตัวนี้ตรวจว่าบอร์ดรีเซ็ต ไม่ใช่ `uptime_ms` ลดลง**
  เพราะ `millis()` ของ Arduino เป็น 32 บิต วนกลับเป็น 0 ทุก 49.7 วัน ถ้าใช้ uptime จะแจ้งว่า "บอร์ดรีเซ็ต" ทั้งที่ไม่ได้รีเซ็ต
- `caps_hash` = CRC-32 (IEEE) ของ descriptor ทุกตัวที่เข้ารหัสแล้ว ต่อกันตามลำดับที่ส่ง (ในแต่ละหน้า caps ก่อน events) — ไม่ใช่เรื่องความปลอดภัย แค่บอกว่า describe ใหม่ไหม
- describe แบ่งหน้าให้แต่ละหน้าไม่เกิน 4096 ไบต์ firmware lib ใส่ descriptor ทีละตัวต่อหน้าได้เสมอ
  descriptor ตัวเดียวที่ใหญ่เกิน = firmware lib ต้องแจ้งตั้งแต่บอร์ดเริ่มทำงาน ไม่ใช่ไปพังตอน describe (decisions #14)

### จังหวะการคุย

- host ส่งคำขอได้ **ทีละหนึ่ง** ต่ออุปกรณ์ รอคำตอบหรือหมดเวลาก่อนส่งตัวถัดไป (MCU มี buffer เดียว)
- **handler ห้ามบล็อกนานกว่า `heartbeat_ms`** — ระหว่างบล็อก firmware lib อ่าน `ping` ไม่ได้ ฝั่ง host จะคิดว่าบอร์ดหาย
  และ buffer UART ของ Uno มีแค่ 64 ไบต์ ข้อความที่ค้างจะล้นทิ้ง งานยาว (หมุนมอเตอร์ 10 วินาที) ให้ตอบ `result` ทันทีแล้วแจ้งจบด้วย `event`
- `event` ส่งได้ทุกเมื่อ ไม่ต้องรอคำขอ

### ตกลง version

- ทั้งสองฝั่งบอกช่วง `{min, max}` อุปกรณ์เลือกเลขสูงสุดที่อยู่ในทั้งสองช่วง ใส่ใน `hello_ack.2`
- ไม่มีเลขร่วม → `error 1 version_mismatch` โดย `detail` = `{0: min, 1: max}` ของอุปกรณ์
- **key 0–2 ของ `hello` และ `hello_ack` ห้ามเปลี่ยนความหมายในทุก version** — เป็นทางเดียวที่สองรุ่นจะรู้ว่าพูดกันได้ไหม
  key อื่นเพิ่มได้ (ผู้รับข้ามที่ไม่รู้จัก) ข้อนี้คือรุ่นแรก **v1**

## Capability descriptor

CBOR map key เลข แปลงเป็นรูปของ Synapta Protocol ที่ฝั่ง host (decisions #3):

| key | field | |
|---|---|---|
| 0 | `name` | text เช่น `led.set` |
| 1 | `description` | text — agent อ่านตัวนี้ตัดสินใจ เขียนให้ชัด |
| 2 | `input_schema` | schema (§Schema) ไม่มี = ไม่รับ input |
| 3 | `output_schema` | schema ไม่มี = ไม่มี output |
| 4 | `danger` | 0 `none` · 1 `actuator` · 2 `destructive` — ไม่มี = 0 |

`danger` เป็นแค่คำแนะนำ โปรดักต์ตั้งทับได้เสมอ (safety.md)

Event descriptor: `0` name · `1` description · `2` data_schema

**firmware เก็บ descriptor ใน flash** (PROGMEM บน AVR) แล้วส่งออกสายตรง ๆ ไม่ประกอบใน RAM —
Uno มี RAM 2 KB descriptor สามสี่ตัวที่ประกอบใน RAM ก็กินหมดแล้ว
(ไลบรารี Arduino เก็บ schema เป็น JSON แล้วแปลงเป็น CBOR ระหว่างส่ง — decisions #14)

## Schema (decisions #11)

schema เป็น CBOR map ที่ key เป็น **ข้อความ** ตามคำของ JSON Schema — แปลงเป็น JSON ได้ทันทีโดยไม่ต้องมีตารางแปล

**คำที่รองรับ:** `type` (object, boolean, integer, number, string, array) · `properties` · `required` · `minimum` · `maximum` ·
`enum` · `maxLength` · `items` · `maxItems` · `description` · `contentEncoding` (ค่าเดียว: `"base64"`)

**ข้อจำกัด**
- `input_schema` ชั้นบนสุดต้องเป็น `type: object` — ตรงกับ MCP tool input ไม่ต้องห่อเพิ่มตอนแปลง
- **ลึกไม่เกิน 3 ชั้น** (object → field → array/object ของค่าเดี่ยว) — parser แบบ recursive บน AVR ที่ลึกไม่จำกัดคือ stack ล้นแล้วบอร์ดรีเซ็ต
- ไบนารี: เขียนใน schema เป็น `{"type":"string","contentEncoding":"base64"}` บนสายเป็น byte string ดิบ
  host แปลงเป็น base64 ตอนส่งให้ agent (JSON ไม่มีชนิดไบนารี) และแปลงกลับตอนส่งลงบอร์ด
- คำอื่นนอกรายการ หรือเกินข้อจำกัด → host ปฏิเสธ descriptor ตอน describe พร้อมบอกว่าผิดตรงไหน ผู้เขียน firmware จะได้รู้ทันที

**ใครตรวจอะไร**
- **host** ตรวจ input เต็มชุดก่อนส่ง (agent ส่งผิดจะได้ error ที่อ่านรู้เรื่องโดยไม่ต้องไปถึงบอร์ด)
- **firmware** ตรวจแค่ชนิดและ `minimum`/`maximum`/`maxLength` — ด่านสุดท้ายกันค่าที่ทำของจริงพัง (PWM เกิน, index เกิน array)

## การเข้ารหัส (decisions #12)

ใช้บนลิงก์ไร้สาย (WiFi, BLE) กับอุปกรณ์ที่จับคู่แล้ว — USB ไม่เข้ารหัส

**จับคู่:** host ส่ง `pair` พร้อม PSK สุ่ม 16 ไบต์ผ่าน **USB** อุปกรณ์เก็บใน flash
firmware **ต้องปฏิเสธ `pair`/`unpair` ที่มาทางไร้สาย** (`error 9 forbidden_transport`) — ไม่งั้นใครในระยะสัญญาณก็จับคู่แทนเจ้าของได้

**เปิด session** (`hello` / `hello_ack` ไม่เข้ารหัสเสมอ — ต้องอ่านได้ก่อนรู้ว่ามีกุญแจไหม):
1. `hello` ส่ง `host_nonce` สุ่ม 8 ไบต์
2. อุปกรณ์ที่จับคู่แล้วตอบ `dev_nonce` สุ่ม 8 ไบต์ และ `key_proof` = HMAC-SHA256(PSK, `"proof"` ‖ host_nonce ‖ dev_nonce) 8 ไบต์แรก
   → host รู้ว่าอุปกรณ์มีกุญแจจริง (กันอุปกรณ์ปลอม)
3. session key = HMAC-SHA256(PSK, `"key"` ‖ host_nonce ‖ dev_nonce) 16 ไบต์แรก — ใหม่ทุก session
4. ข้อความถัดไปทั้งหมดเข้ารหัส คำสั่งแรกที่ถอดผ่าน = อุปกรณ์รู้ว่า host มีกุญแจจริง

**เฟรมที่เข้ารหัส** (`flags` bit 0 = 1): `body` = `counter(4, big-endian) | ciphertext | tag(8)`
- AES-128-CCM, nonce 13 ไบต์ = `ทิศ(1: 0 host→dev, 1 dev→host) | counter(4) | ศูนย์(8)`
- AAD = `flags` (และ `addr` ถ้ามี) — ไม่งั้นแก้ bit ใน flags ระหว่างทางได้โดยไม่มีใครรู้
- counter เริ่ม 1 ทุก session แยกตามทิศ ผู้รับ **ทิ้งเฟรมที่ counter ไม่มากกว่าตัวล่าสุด** — กันการอัดข้อความเก่ามาเล่นซ้ำ (สั่ง "เปิดปั๊ม" ซ้ำ)
- **ขยับ counter ล่าสุดเฉพาะเฟรมที่ถอดผ่าน** — ถ้าขยับก่อนตรวจ tag ผู้โจมตีส่งเฟรมปลอม counter สูง ๆ ครั้งเดียว
  เฟรมจริงที่ตามมาจะถูกทิ้งเป็น replay ทั้งหมด
- ค่าใช้จ่าย 12 ไบต์ต่อเฟรม (Uno ไม่เข้ารหัส จึงไม่กระทบเฟรม 128 ไบต์)

**กันการถอยไปไม่เข้ารหัส:** host ที่มี PSK ของ `device_id` นั้นแล้ว ต้อง **ปฏิเสธ** อุปกรณ์ที่ตอบ `paired = false` หรือส่งเฟรมไม่เข้ารหัสทางไร้สาย
ไม่งั้นผู้โจมตีแค่ปลอม `hello_ack` ว่ายังไม่จับคู่ ก็สั่งงานแบบไม่เข้ารหัสได้

อุปกรณ์ไร้สายที่เข้ารหัสไม่ได้ (Uno + โมดูลไร้สาย) → host ติดป้าย `link_secure = false` (decisions #9)

## Error code

| code | ชื่อ | ความหมาย |
|---|---|---|
| 1 | `version_mismatch` | ไม่มี version ร่วม — `detail` = ช่วงของอุปกรณ์ |
| 2 | `unknown_capability` | ไม่มีคำสั่งชื่อนี้ |
| 3 | `invalid_input` | input ไม่ตรง schema หรือ CBOR ผิดรูป |
| 4 | `busy` | กำลังทำงานที่ทำพร้อมกันไม่ได้ |
| 5 | `failed` | handler ทำงานแล้วพัง (`msg` บอกเหตุ) |
| 6 | `frame_too_large` | host ส่งเฟรมเกิน `max_frame` ของอุปกรณ์ |
| 7 | `unknown_message` | ไม่รู้จัก `t` นี้ |
| 8 | `auth_required` | อุปกรณ์จับคู่แล้วแต่ได้ข้อความไม่เข้ารหัสทางไร้สาย |
| 9 | `forbidden_transport` | คำสั่งนี้ทำผ่าน transport นี้ไม่ได้ (`pair` ทางไร้สาย) |

0 สงวนไว้ ไม่ใช้

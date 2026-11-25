# SynaptaDevice — ไลบรารี Arduino ของ SDP

ทำให้บอร์ด Arduino พูด SDP v1 ผ่าน USB serial เพื่อให้ AI agent ค้นหาได้ว่าบอร์ดทำอะไรได้บ้าง แล้วสั่งงานได้อย่างปลอดภัย

- รองรับ **Arduino Uno (AVR)** และ **ESP32** — ใช้ include ไฟล์เดียว `#include <SynaptaDevice.h>`
- ประกาศความสามารถด้วย JSON Schema ไลบรารีจัดการเฟรม COBS/CRC, CBOR, การแบ่งหน้า describe, การตรวจ input และ heartbeat ให้ทั้งหมด
- ตอบ byte ตรงกับ [`spec/vectors`](../../../spec/vectors/) (ทดสอบด้วย [`../test/run_vectors.py`](../test/run_vectors.py))

> SDP **ไม่ใช่ระบบความปลอดภัยเชิงหน้าที่** (functional safety) — งานที่คนอาจบาดเจ็บต้องมีวงจรตัดฉุกเฉินแยกจากซอฟต์แวร์

## ติดตั้ง

คัดลอกโฟลเดอร์ `SynaptaDevice` ไปไว้ใน `Documents/Arduino/libraries/` หรือคอมไพล์ด้วย `arduino-cli`:

```
arduino-cli compile -b esp32:esp32:esp32 --library firmware/arduino/SynaptaDevice firmware/arduino/SynaptaDevice/examples/LedDemo
```

## ตัวอย่าง

ตัวอย่างเต็มอยู่ที่ [`examples/LedDemo`](examples/LedDemo/LedDemo.ino) (สั่ง LED, อ่านอุณหภูมิ, แจ้งเมื่อกดปุ่ม)

```cpp
#include <SynaptaDevice.h>

SDP_CAPABILITY(ledSet, "led.set", "Turn the on-board LED on or off", SDP_ACTUATOR,
               R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})",
               SDP_NO_SCHEMA) {
  const bool on = call.input().getBool("on");
  digitalWrite(LED_PIN, on);
  call.output().add("on", on);
}

const SdpCapability* const CAPS[] = {&ledSet};
SynaptaDevice sdp;

void allOff() { digitalWrite(LED_PIN, LOW); }  // PC หลุด → ปิดทุกอย่าง

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  SdpConfig config;
  config.model = "esp32-devkit";
  config.firmware = "my-device 1.0.0";
  sdp.onLinkLost(allOff);  // ต้องตั้งก่อน begin() ถ้ามี capability ระดับ actuator
  sdp.begin(Serial, config, CAPS);
}

void loop() { sdp.loop(); }
```

ตรวจกับบอร์ดจริงจาก PC: `cargo run -p sdp-check -- --serial COM5` (Uno เติม `--reset dtr`)

## ประกาศความสามารถ

`SDP_CAPABILITY(ชื่อตัวแปร, "ชื่อ", "คำอธิบาย", ระดับอันตราย, input_schema, output_schema) { ...handler... }`

- **คำอธิบาย** คือสิ่งที่ agent อ่านเพื่อตัดสินใจ — เขียนให้ชัดว่าทำอะไร หน่วยอะไร
- **ระดับอันตราย**: `SDP_NONE` (อ่านค่า), `SDP_ACTUATOR` (ขยับของจริง), `SDP_DESTRUCTIVE` (ย้อนไม่ได้) — เป็นคำแนะนำ เจ้าของตั้งทับได้ที่ฝั่ง PC
- **schema** เป็นข้อความ JSON ใช้ `R"(...)"` จะได้ไม่ต้อง escape เครื่องหมายคำพูด — ไม่มีให้ใส่ `SDP_NO_SCHEMA`

event: `SDP_EVENT(ชื่อตัวแปร, "ชื่อ", "คำอธิบาย", data_schema);` แล้วส่งด้วย

```cpp
sdp.beginEvent(buttonPressed).add("count", presses);
sdp.sendEvent();  // ไม่ส่งถ้า PC ไม่ได้ต่ออยู่
```

### schema ที่รองรับ (wire-format.md §Schema)

คำ: `type` (object, boolean, integer, number, string, array), `properties`, `required`, `minimum`, `maximum`, `enum`,
`maxLength`, `items`, `maxItems`, `description`, `contentEncoding` (`"base64"` = ข้อมูลไบนารี)

- `input_schema` ชั้นบนต้องเป็น `"type":"object"` และซ้อนลึกไม่เกิน 3 ชั้น
- จำนวนเต็มใน schema ไม่เกิน 32 บิต
- schema ผิด → `begin()` คืน `false` พิมพ์เหตุผลทาง serial (เช่น `SDP: led.set input_schema: keyword not supported by SDP v1 (at char 52)`)
  และบอร์ดตอบทุกคำขอด้วย error 5 จนกว่าจะแก้

ก่อนเรียก handler ไลบรารีตรวจชนิด, `required`, `minimum`/`maximum`, `maxLength`, `maxItems` ให้แล้ว
— ส่งผิดได้ error 3 พร้อมข้อความเช่น `'duty' is above maximum` handler ไม่ต้องตรวจซ้ำ

## ใน handler

| ใช้ | ทำอะไร |
|---|---|
| `call.input().getBool / getInt / getNumber("key", ค่าเมื่อไม่มี)` | อ่าน input |
| `call.input().getText("key", buf, sizeof buf)` | คัดลอกข้อความพร้อม `'\0'` (ตัดที่ขอบตัวอักษร UTF-8 ถ้ายาวเกิน) |
| `call.input().getBytes("key", &len)` | ข้อมูลไบนารี (ชี้เข้า buffer รับ ใช้ได้จนจบ handler) |
| `call.output().add("key", ค่า)` | ใส่ output — ใส่ key **สั้นก่อนยาว แล้วตามตัวอักษร** (ผิดลำดับยังส่งได้ แต่มีคำเตือนใน log) |
| `beginObject("key")` / `beginArray("key")` / `item(ค่า)` / `end()` | output ซ้อนกัน (ลึกได้อีก 3 ชั้น) |
| `call.fail(SDP_BUSY, "motor still moving")` | ตอบ error แทนผลลัพธ์ (`SDP_FAILED`, `SDP_INVALID_INPUT` ก็ได้) |

ไม่เรียก `output()` เลย = ตอบผลลัพธ์ว่าง

## ข้อตกลงที่ต้องรู้

- **handler ห้ามบล็อกนานกว่า `heartbeatMs`** (ค่าเริ่มต้น 1000 ms) ระหว่างนั้นไลบรารีอ่าน serial ไม่ได้ PC จะคิดว่าบอร์ดหาย
  และ buffer UART ของ Uno มีแค่ 64 ไบต์ งานยาวให้ตอบทันทีแล้วแจ้งจบด้วย event
- **`onLinkLost()`** ถูกเรียกเมื่อไม่ได้ยิน PC เกิน 3 × `heartbeatMs` — ปิดเอาต์พุตทั้งหมดที่นี่
  capability ระดับ actuator/destructive ถูกปฏิเสธถ้าไม่ได้ตั้ง (decisions #14)
- พิมพ์ `Serial.print` ได้ตามปกตินอก handler — PC เก็บเป็น console log ของบอร์ด
- ห้ามเรียก `beginEvent()` ระหว่าง handler (ใช้ buffer เดียวกับ output)
- `device_id` สุ่มครั้งแรกแล้วเก็บไว้: Uno ใช้ **9 ไบต์ท้ายของ EEPROM**, ESP32 ใช้ NVS (namespace `sdp`)

## ค่าใน `SdpConfig`

| ค่า | เริ่มต้น (Uno / ESP32) | |
|---|---|---|
| `model`, `firmware` | `"arduino"`, `"unknown"` | ตั้งเสมอ — PC ใช้ `firmware` ตรวจว่าแฟลชสำเร็จ |
| `maxFrame` | 128 / 512 | เฟรมยาวสุดที่บอร์ดรับได้ (ต่ำสุด 128) จองตอน `begin()` |
| `outputBytes` | 64 / 512 | buffer ของ output และ data ของ event — เกินแล้วตอบ error 5 |
| `heartbeatMs` | 1000 | |
| `describePageBytes` | 4096 | |
| `address` | 0xFF (ไม่ใช้) | เลขของบอร์ดบนสาย RS485 หลายตัว |

## ขนาด

LedDemo บน Uno: flash ราว **24 KB จาก 32 KB**, RAM ราว 950 ไบต์รวม buffer (เหลือ ~1 KB ให้ stack และโค้ดของคุณ)
ส่วนที่ใหญ่สุดคือตัวแปลง JSON → CBOR และตัวตรวจ schema (~6 KB) — ESP32 ไม่มีปัญหาเรื่องนี้

## ยังไม่มีในรุ่นนี้

- ลิงก์ไร้สาย (WiFi/BLE) และการจับคู่/เข้ารหัส — เฟส 2–3 (`pair` ตอนนี้ตอบ error 5)
- บอร์ดอื่นนอกจาก AVR/ESP32 คอมไพล์ได้แต่ `device_id` จะสุ่มใหม่ทุกบูต

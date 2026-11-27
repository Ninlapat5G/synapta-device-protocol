# Synapta Device Protocol (SDP)

โปรโตคอลให้ AI agent สั่งงาน อ่านค่า และอัปโค้ดลงอุปกรณ์ที่ **ไม่มี OS** (MCU: ESP32, Arduino, STM32, RP2040 …)
ผ่านสาย USB / RS232 / RS485 หรือไร้สาย WiFi / Bluetooth

> สถานะ (2026-11-25): เฟส 0 เขียนครบแล้ว — สเปก v1, test vectors, `crates/sdp-core`, ไลบรารี Arduino, `sdp-sim`, `sdp-check`
> **ยังเหลือ:** ทดสอบกับ ESP32 ตัวจริงผ่าน USB (`sdp-check --serial COMx`) ซึ่งเป็นเกณฑ์ปิดเฟส 0 — ดู [roadmap](docs/roadmap.md)

| ส่วน | อยู่ที่ | คืออะไร |
|---|---|---|
| `sdp-core` | `crates/sdp-core/` | Rust: CBOR, เฟรม, ข้อความ, การเข้ารหัส — ไม่มี I/O, no_std ได้ |
| SynaptaDevice | `firmware/arduino/SynaptaDevice/` | ไลบรารี Arduino (Uno, ESP32) ให้บอร์ดพูด SDP — [วิธีใช้](firmware/arduino/SynaptaDevice/README.md) |
| `sdp-sim` | `tools/sdp-sim/` | อุปกรณ์จำลองบน TCP (ตอบเหมือนตัวอย่าง LedDemo) ไว้เทสต์ host โดยไม่มีบอร์ด |
| `sdp-check` | `tools/sdp-check/` | ตรวจลำดับเฟส 0 กับบอร์ดจริง (serial) หรือ `sdp-sim` (TCP) |

```
cargo test                                   # sdp-core + sdp-check คุยกับ sdp-sim ทั้งแบบเข้ารหัสและไม่เข้ารหัส
cargo clippy --workspace --all-targets -- -D warnings
python firmware/arduino/test/run_vectors.py  # ไลบรารี Arduino กับ spec/vectors (คอมไพล์ด้วย MSVC บน PC)
python spec/vectors/generate.py              # สร้าง vectors ใหม่หลังแก้สเปก

cargo run -p sdp-sim                         # อุปกรณ์จำลองที่ 127.0.0.1:7878 (กด Enter = กดปุ่ม)
cargo run -p sdp-check -- --tcp 127.0.0.1:7878
cargo run -p sdp-check -- --serial COM5      # บอร์ดจริงที่แฟลช examples/LedDemo (Uno: เติม --reset dtr)
```

## ทำไมต้องมี

ระบบ Synapta ตอนนี้คุยกับเครื่องที่มี OS เท่านั้น:

- **Synapta Connect** — agent บน Windows ให้ AI รันคำสั่ง ดูจอ ส่ง input ผ่านเครือข่าย (TLS)
- **Synapta Protocol** — เครือข่าย P2P ให้ node ที่มี OS ประกาศความสามารถและเรียก tool ข้ามเครื่อง (Rust + Tokio + Noise)

MCU รับของพวกนี้ไม่ไหว (AVR มี RAM 2 KB, ไม่มี TCP/TLS) SDP คือชั้นที่หายไป:
ภาษาสั้น ๆ ที่ MCU พูดได้ บอกได้ว่าตัวเองทำอะไรได้ และให้เครื่องที่มี OS เป็นประตูต่อเข้าโลก Synapta

## ภาพเดียว

```
 AI agent ──MCP──▶ SynaptaCode ─โปรเซสลูก─▶ sdp-host ──SDP──▶ MCU (firmware lib ของ SDP)
                   Synapta Connect ─ลิงก์ crate─▶ sdp-host ──SSH──▶ อุปกรณ์เครือข่าย
                        │                          │ transport: USB / WiFi / Bluetooth / SSH
                        │ สิทธิ์เจ้าของ · audit log    │ จองพอร์ต · แฟลช · toolchain
                        └─(ภายหลัง) ประกาศ MCU เป็น capability บน Synapta Protocol
```

ส่วน host (`sdp-host`) อยู่ใน repo นี้ ไม่ผูกกับโปรดักต์ใด — ดู [decisions #6](docs/decisions.md)

## อ่านอะไรก่อน

| เอกสาร | เรื่อง |
|---|---|
| [docs/decisions.md](docs/decisions.md) | สิ่งที่ตัดสินแล้ว และเหตุผล — อ่านก่อนเสนอเปลี่ยนโครง |
| [docs/architecture.md](docs/architecture.md) | อะไรอยู่ repo ไหน, ชั้นของระบบ, โครงโฟลเดอร์ที่เสนอ |
| [docs/wire-format.md](docs/wire-format.md) | สเปกข้อความบนสาย v1: เฟรม, CBOR, ข้อความ, schema, การเข้ารหัส |
| [spec/vectors/](spec/vectors/README.md) | test vectors ระดับ byte ที่ทุกฝั่งต้องผ่าน |
| [docs/transports.md](docs/transports.md) | serial, RS485, WiFi, BLE และกับดักของแต่ละแบบ |
| [tools/sdp-ota/GUIDE.md](tools/sdp-ota/GUIDE.md) | **เริ่มใช้ ESP32 ตรงนี้**: เตรียมบอร์ด เขียน `main/app.c` แล้ว Build และอัปโหลดผ่าน Wi-Fi (`sdpo guide`) |
| [docs/esp32-ota.md](docs/esp32-ota.md) | โมดูล ESP32 OTA สำหรับ SynaptaCode, Connect และ agent |
| [docs/flashing.md](docs/flashing.md) | อัปโค้ดผ่านสายและไร้สาย |
| [docs/safety.md](docs/safety.md) | สิทธิ์, heartbeat, สภาพปลอดภัยของอุปกรณ์ที่ขยับของจริง |
| [docs/roadmap.md](docs/roadmap.md) | เฟส และคำถามที่ยังเปิด |

## โปรเจกต์ที่เกี่ยวข้อง

- `D:\claude_code\Synapta Connect` (Rust) — ลิงก์ crate `sdp-host` ตรง ทำสิทธิ์ / audit log / หน้าอุปกรณ์ของตัวเอง
- `D:\claude_code\SynaptaCode` (Node/TypeScript) — เปิดโปรแกรม `sdp-host` เป็นโปรเซสลูก ทำ MCP tools และสิทธิ์ของตัวเอง
- `D:\claude_code\Synapta Protocol` — รูปแบบ capability (`name, description, input_schema, output_schema`)
  และการตกลง version `{min, max}` ของ SDP ลอกมาจากที่นั่น เพื่อให้มีรูปแบบเดียวทั้งระบบ
- `D:\claude_code\IIoT-app`, `Arduino WiFi of GDD`, `Arduino Bluetooth of GDD` — โปรเจกต์ MCU เดิม ใช้เป็นกรณีทดสอบจริงได้

## สัญญาอนุญาต

เลือกใช้ได้ตามสะดวกระหว่าง [MIT](LICENSE-MIT) หรือ [Apache-2.0](LICENSE-APACHE)

โค้ดที่ส่งเข้ามาใน repo นี้ถือว่าอนุญาตภายใต้ทั้งสองแบบเช่นกัน เว้นแต่ผู้ส่งจะระบุไว้เป็นอย่างอื่น

# Roadmap

## เฟส 0 — สเปกให้นิ่ง (repo นี้เท่านั้น)

1. ~~ตัดสินคำถามใน wire-format.md~~ — เสร็จ 2026-11-25 (decisions #10–#12)
2. ~~เขียน test vectors~~ — เสร็จ 2026-11-25 ที่ `spec/vectors/` (สร้างด้วย `generate.py`)
3. ~~`crates/sdp-core` ผ่าน test vectors~~ — เสร็จ 2026-11-25 (`cargo test`)
4. `firmware/arduino` ตัวแรก (ESP32 + Uno) ผ่าน test vectors และรันตัวอย่าง LED ได้จริง
   — โค้ดเสร็จ 2026-11-25: ผ่าน vectors บน PC (`firmware/arduino/test/run_vectors.py`) และคอมไพล์ Uno/ESP32 ผ่าน
   **ยังไม่ได้ลองบนบอร์ดจริง**
5. ~~`tools/sdp-sim` จำลองอุปกรณ์บน TCP ให้เทสต์ host ได้โดยไม่มีบอร์ด~~ — เสร็จ 2026-11-25
   พร้อม `tools/sdp-check` ที่รันลำดับข้างล่างได้ทั้งกับบอร์ดจริงและ sdp-sim

**เสร็จเมื่อ:** PC คุยกับ ESP32 จริงผ่าน USB ด้วยสคริปต์ทดสอบ: hello → describe → call → event → ping หาย → `on_link_lost` ทำงาน
— สคริปต์คือ `sdp-check --serial COMx` (ผ่านกับ sdp-sim แล้ว) **เหลือรันกับ ESP32 ที่แฟลช LedDemo**

## เฟส 1 — `sdp-host`: USB + แฟลชผ่านสาย + toolchain

ใน repo นี้:
- `crates/sdp-host`: registry, จองพอร์ตข้ามโปรเซส, transport USB serial, codec ดิบ (ทีละบรรทัด), hook `authorize`, `tools()` / `call_tool()` (decisions #13)
- แฟลชผ่าน arduino-cli/esptool ตาม flashing.md, จัดการ toolchain (ตรวจของที่มี → โฟลเดอร์กลาง → ดาวน์โหลด)
- `crates/sdp-host-bin`: JSON-RPC ผ่าน stdio

ในโปรดักต์:
- **SynaptaCode** เปิด `sdp-host` เป็นโปรเซสลูก + ส่งต่อ tool ให้ agent ผ่าน MCP
- **Connect** ลิงก์ crate + สิทธิ์ใน settings + หน้า panel, ทดสอบบน Windows Server 2016 (app01) และ `dumpbin /imports`

## เฟส 2 — WiFi + OTA ของ ESP32

- mDNS `_sdp._tcp`, PSK จากการจับคู่ผ่าน USB + AES-CCM, HTTP OTA แบบ A/B + เซ็น image

## เฟส 3 — Bluetooth

- BLE ของ ESP32 ใน `sdp-host` (Connect เรียกเป็นโปรเซสลูก เลี่ยง WinRT ใน exe หลัก), เข้ารหัสแบบเดียวกับ WiFi
- Bluetooth Classic (HC-05/06) ใช้ทาง serial ได้ตั้งแต่เฟส 1 — เฟสนี้แค่เพิ่มการค้นหา/จับคู่

## เฟส 4 — SSH

- อุปกรณ์เครือข่ายแบบ "ดิบ" ผ่าน `russh`: exec + shell แบบ interactive, จำ host key (ดู transports.md §SSH)

## ภายหลัง

- RS485 หลายอุปกรณ์, Modbus RTU/TCP (เมื่อมีงานที่ใช้)
- ESP32 ต่อ SynaptaCode ตรงด้วย SDP บน WSS (ไม่ผ่าน PC)
- Connect ประกาศอุปกรณ์ SDP เป็น capability บน Synapta Protocol
- ส่งออกเป็น UTCP manual เมื่อ UTCP นิ่ง (decisions #1)

## คำถามที่ตอบแล้ว (2026-11-25)

| คำถาม | คำตอบ |
|---|---|
| บอร์ดหลัก | USB, ESP32, Arduino Uno — ต้องขยายไปหลาย protocol ได้ → decisions #8 |
| เครื่องมือแฟลช | ใช้ของที่มี ไม่มีค่อยลง หลายเวอร์ชันในโฟลเดอร์กลาง agent เปลี่ยนเวอร์ชันได้ → decisions #7 |
| ความปลอดภัยเครือข่าย | การยืนยันตัวตนเป็นของโปรดักต์ ลิงก์ไร้สายเข้ารหัสเผื่อการดักจับ → decisions #9 |
| host อยู่ไหน | `sdp-host` ใน repo นี้ ใช้ได้ทั้ง Connect และ SynaptaCode → decisions #6 |

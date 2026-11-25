# สถาปัตยกรรม

## สี่ชั้น

แต่ละชั้นเปลี่ยนได้โดยไม่แตะชั้นอื่น

| ชั้น | หน้าที่ | อยู่ที่ |
|---|---|---|
| **Transport** | ท่อ byte: เปิด / ปิด / อ่าน / เขียน | `sdp-host` |
| **Framing** | ตัด byte เป็นข้อความ ตรวจ CRC | สเปก SDP + `sdp-core` + firmware lib |
| **Message** | hello / describe / call / event / error | สเปก SDP + `sdp-core` + firmware lib |
| **Host service** | registry, จองพอร์ต, แฟลช, toolchain, รายการ tool ของ agent | `sdp-host` |
| **Product** | สิทธิ์เจ้าของ, audit log, UI, ส่งต่อ MCP | Connect, SynaptaCode (แต่ละตัวทำของตัวเอง) |

อุปกรณ์ที่ไม่ได้พูด SDP (โมดูลสำเร็จรูป, PLC, อุปกรณ์เครือข่ายผ่าน SSH) ใช้ได้ผ่าน "codec ดิบ" ของ `sdp-host`:
ข้อความทีละบรรทัด, คำสั่ง SSH หรือ Modbus — agent ได้แค่อ่าน/เขียน ไม่มี describe ให้

## โปรดักต์ใช้ `sdp-host` อย่างไร (decisions #6)

```
Connect (Rust)        ──ลิงก์ crate──────────────▶ sdp-host ──▶ USB / WiFi / SSH
Connect (BLE)         ──โปรเซสลูก sdp-host (+BLE)──▶            ──▶ BLE
SynaptaCode (Node)    ──โปรเซสลูก, JSON-RPC stdio──▶ sdp-host ──▶ USB / WiFi / BLE / SSH
```

- ก่อนสั่งของอันตรายหรือแฟลช `sdp-host` ถาม `authorize` จากโปรดักต์ (stdio: ส่งคำขอกลับไปโปรเซสแม่) — ไม่มีคำตอบ = ปฏิเสธ
- สองโปรดักต์บนเครื่องเดียวกันแชร์โฟลเดอร์กลาง `%LOCALAPPDATA%\Synapta\` (toolchain, lock ของพอร์ต, ชื่อเล่นอุปกรณ์)

## เส้นทางของคำสั่งหนึ่งครั้ง

```
agent: device_call(device="bench-esp32", name="led.set", input={"on":true})
  → โปรดักต์ (Connect หรือ SynaptaCode) ตรวจสิทธิ์เจ้าของ
  → sdp-host registry: หา port ของ device ด้วย VID/PID/serial, จองพอร์ต
  → sdp-core: เข้ารหัส call → framing → serial write
  → MCU: firmware lib แกะข้อความ → เรียก handler "led.set" → ตอบ result
  → ย้อนทางเดิม → agent ได้ {"on":true}
```

## ตัวตนของอุปกรณ์

ห้ามใช้ชื่อพอร์ต (`COM5`) เป็นตัวตน — เลขเปลี่ยนเมื่อเสียบช่องอื่น หรือเมื่อบอร์ดเข้า bootloader
ใช้ตามลำดับ:
1. `device_id` ที่ firmware SDP ส่งมาใน `hello` (สุ่มครั้งแรกแล้วเก็บใน flash/EEPROM)
2. USB VID + PID + serial number
3. ที่อยู่เครือข่าย / BLE address (อุปกรณ์ไร้สาย)

เจ้าของตั้งชื่อเล่น (`bench-esp32`) ผูกกับตัวตนข้างบน เก็บในโฟลเดอร์กลาง — ตั้งจากโปรดักต์ไหนก็เห็นชื่อเดียวกัน

## โครงโฟลเดอร์ (มีแล้ว: `spec/vectors/`, `crates/sdp-core/`, `firmware/arduino/`, `tools/sdp-sim/`, `tools/sdp-check/`)

```
spec/                 สเปกที่เป็นทางการ (ย้ายจาก docs/wire-format.md เมื่อนิ่ง) + test vectors (byte ที่ถูกต้องของแต่ละข้อความ)
crates/sdp-core/      Rust: encode/decode + framing, no_std ได้, ไม่มี I/O
crates/sdp-host/      Rust: transport, registry, จองพอร์ต, แฟลช, toolchain — Connect ลิงก์ตรง
crates/sdp-host-bin/  โปรแกรม sdp-host: ห่อ sdp-host เป็น JSON-RPC ผ่าน stdio — SynaptaCode และ BLE ของ Connect ใช้
firmware/arduino/     ไลบรารี Arduino (AVR, ESP32) — include ไฟล์เดียว ใช้ RAM น้อย + test/ ที่รันกับ vectors บน PC
firmware/esp-idf/     component สำหรับ ESP-IDF (เพิ่ม OTA, WiFi, BLE)
firmware/micropython/ โมดูล Python
tools/sdp-sim/        อุปกรณ์จำลองบน PC (TCP) ไว้เทสต์ Connect โดยไม่ต้องมีบอร์ด
tools/sdp-check/      ตรวจลำดับของเฟส 0 กับบอร์ดจริง (serial) หรือ sdp-sim (TCP)
examples/             LED, เซ็นเซอร์, รีเลย์ — อย่างละไฟล์
```

**test vectors คือสัญญากลาง:** ทั้ง `sdp-core` และ firmware ทุกภาษาต้องผ่านชุดเดียวกัน กันการตีความสเปกไม่ตรงกัน

## สิ่งที่แต่ละโปรดักต์ต้องเพิ่ม (สรุปไว้ให้ session ฝั่งนั้น)

**Synapta Connect**
- ลิงก์ crate `sdp-host`, ใส่ hook `authorize` ที่อ่านสิทธิ์จาก `settings.rs`
  (อนุญาตอุปกรณ์รายตัว, อนุญาตแฟลชแยก — ปิดเป็นค่าเริ่มต้น), ลง audit log
- ส่งต่อ `tools()` / `call_tool()` ของ crate ให้ agent (decisions #13), หน้า "อุปกรณ์" ใน panel
- BLE: เรียกโปรแกรม `sdp-host` เป็นโปรเซสลูก
- ทุก Win32 API ใน `sdp-host` (ส่วนที่ลิงก์เข้า Connect) ต้องผ่าน `dumpbin /imports` ว่าไม่ใหม่กว่า Windows Server 2016 (1607)

**SynaptaCode**
- เปิดโปรแกรม `sdp-host` เป็นโปรเซสลูก (แบบ `pty-host`), ตอบ `authorize` จากระบบสิทธิ์ของตัวเอง
- MCP: ส่งต่อ `tools` / `call_tool` ของ `sdp-host` ไม่เขียน tool เอง (decisions #13)
  ได้ `device_list / describe / call / flash` + `toolchain_list / install / use / remove` มาเอง

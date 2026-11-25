// CRC แบบทีละ byte ไม่มีตาราง — ประหยัด flash บน Uno (ตาราง CRC-32 กิน 1 KB)
#pragma once

#include <stdint.h>

namespace sdplib {

// CRC-16/CCITT-FALSE: เริ่ม 0xFFFF ปิดท้ายเฟรม serial
uint16_t crc16Update(uint16_t crc, uint8_t byte);

// CRC-32 (IEEE): เริ่ม 0xFFFFFFFF แล้ว xor 0xFFFFFFFF ตอนจบ — ใช้ทำ caps_hash
uint32_t crc32Update(uint32_t crc, uint8_t byte);

}  // namespace sdplib

// สิ่งที่ต่างกันตามบอร์ด: ที่เก็บ device_id และแหล่งสุ่ม
#pragma once

#include <stdint.h>

namespace sdplib {

// device_id: สุ่มครั้งแรกที่บูตแล้วเก็บถาวร (AVR = EEPROM 9 ไบต์ท้าย, ESP32 = NVS) — คืน false ถ้าบอร์ดนี้เก็บถาวรไม่ได้
bool loadDeviceId(uint8_t id[8]);

void randomBytes(uint8_t* out, uint8_t len);

}  // namespace sdplib

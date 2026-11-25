#include "platform.h"

#if defined(SDP_HOST_TEST)
// เทสต์บน PC: ตัวทดสอบเป็นคนกำหนดค่า (ดู firmware/arduino/test/host/)

#elif defined(__AVR__)
#include <Arduino.h>
#include <avr/eeprom.h>

#include "crc.h"

namespace sdplib {

// AVR ไม่มีตัวสุ่มฮาร์ดแวร์: รวมสัญญาณรบกวนจาก ADC ขาลอย + จังหวะ micros() ผ่าน CRC-32
void randomBytes(uint8_t* out, uint8_t len) {
  uint32_t mix = 0xFFFFFFFFUL;
  for (uint8_t i = 0; i < len; i++) {
    for (uint8_t round = 0; round < 16; round++) {
      const uint16_t a = analogRead(round % 6);
      mix = crc32Update(mix, static_cast<uint8_t>(a));
      mix = crc32Update(mix, static_cast<uint8_t>(micros()));
    }
    out[i] = static_cast<uint8_t>(mix ^ (mix >> 8) ^ (mix >> 16) ^ (mix >> 24));
  }
}

bool loadDeviceId(uint8_t id[8]) {
  constexpr uint8_t kMagic = 0x5D;
  uint8_t* const base = reinterpret_cast<uint8_t*>(E2END - 8);  // 9 ไบต์ท้าย EEPROM: magic + id
  if (eeprom_read_byte(base) != kMagic) {
    randomBytes(id, 8);
    for (uint8_t i = 0; i < 8; i++) eeprom_update_byte(base + 1 + i, id[i]);
    eeprom_update_byte(base, kMagic);  // เขียน magic ทีหลัง: ไฟดับกลางทาง = สุ่มใหม่รอบหน้า ไม่ได้ id ครึ่ง ๆ
  }
  for (uint8_t i = 0; i < 8; i++) id[i] = eeprom_read_byte(base + 1 + i);
  return true;
}

}  // namespace sdplib

#elif defined(ESP32)
#include <Preferences.h>
#include <esp_random.h>

namespace sdplib {

void randomBytes(uint8_t* out, uint8_t len) { esp_fill_random(out, len); }

bool loadDeviceId(uint8_t id[8]) {
  Preferences prefs;
  if (!prefs.begin("sdp", false)) return false;
  if (prefs.getBytes("device_id", id, 8) != 8) {
    randomBytes(id, 8);
    prefs.putBytes("device_id", id, 8);
  }
  prefs.end();
  return true;
}

}  // namespace sdplib

#else
// บอร์ดอื่น: ยังไม่มีที่เก็บถาวร — device_id สุ่มใหม่ทุกบูต (host จะเห็นเป็นอุปกรณ์ใหม่ทุกครั้ง)
#include <Arduino.h>

#include "crc.h"

namespace sdplib {

void randomBytes(uint8_t* out, uint8_t len) {
  uint32_t mix = 0xFFFFFFFFUL;
  for (uint8_t i = 0; i < len; i++) {
    for (uint8_t round = 0; round < 16; round++) mix = crc32Update(mix, static_cast<uint8_t>(micros()));
    out[i] = static_cast<uint8_t>(mix);
  }
}

bool loadDeviceId(uint8_t id[8]) {
  randomBytes(id, 8);
  return false;
}

}  // namespace sdplib
#endif

// ส่งเฟรม serial: 0x00 | COBS(flags | [addr] | body | CRC-16) | 0x00 แบบทีละ byte
// ใช้ buffer แค่บล็อก COBS 254 ไบต์ จึงส่ง describe ยาวหลาย KB จาก flash ได้โดยไม่พักทั้งเฟรมใน RAM
#pragma once

#include <Print.h>
#include <stdint.h>

#include "sink.h"

namespace sdplib {

constexpr uint8_t kCobsBlock = 254;

class FrameSink : public Sink {
 public:
  explicit FrameSink(uint8_t* block = nullptr) : block_(block) {}

  void begin(Print& out);
  void put(uint8_t byte) override;  // byte ของ flags/addr/body — คิด CRC ไปด้วย
  void end();

 private:
  void cobsPut(uint8_t byte);
  void flush(uint8_t code);

  Print* out_ = nullptr;
  uint8_t* block_;
  uint8_t used_ = 0;
  bool afterFullBlock_ = false;
  uint16_t crc_ = 0xFFFF;
};

}  // namespace sdplib

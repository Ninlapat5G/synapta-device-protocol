// เขียน CBOR แบบสั้นสุดตามกฎ encoding ของ wire-format.md ลง Sink
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sink.h"

namespace sdplib {

enum Major : uint8_t { kUint = 0, kNint = 1, kBytes = 2, kText = 3, kArray = 4, kMap = 5, kSimple = 7 };

// เขียนหัว (major + ตัวเลข) แบบสั้นสุดลง out (ต้องมีที่ 5 ไบต์) คืนจำนวนไบต์
uint8_t cborHead(uint8_t* out, uint8_t major, uint32_t arg);

class CborWriter {
 public:
  explicit CborWriter(Sink& sink) : sink_(sink) {}

  void head(uint8_t major, uint32_t arg);
  void uint(uint32_t v) { head(kUint, v); }
  void sint(int32_t v);
  void boolean(bool v) { sink_.put(v ? 0xF5 : 0xF4); }
  void number(double v);  // float 32 บิตเมื่อแทนค่าได้ตรง ไม่งั้น 64
  void text(const char* s, size_t len);
  void text(const char* s);
  void flashText(const char* s);  // ข้อความใน flash (PROGMEM)
  void bytes(const uint8_t* data, size_t len);
  void array(size_t n) { head(kArray, static_cast<uint32_t>(n)); }
  void map(size_t n) { head(kMap, static_cast<uint32_t>(n)); }
  void raw(const uint8_t* data, size_t len) { sink_.write(data, len); }

 private:
  Sink& sink_;
};

}  // namespace sdplib

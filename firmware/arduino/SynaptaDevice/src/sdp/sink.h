// ปลายทางของ byte ที่เขียนออก — ให้ตัวเขียน CBOR ไม่ต้องรู้ว่าส่งลงสาย นับขนาด หรือคิด hash
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "crc.h"

namespace sdplib {

class Sink {
 public:
  virtual void put(uint8_t byte) = 0;
  void write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) put(data[i]);
  }

 protected:
  ~Sink() = default;
};

// นับขนาดและคิด CRC-32 ไปพร้อมกัน — ใช้ลองเขียน descriptor เพื่อแบ่งหน้าและทำ caps_hash โดยไม่ต้องพักใน RAM
class CountSink : public Sink {
 public:
  void put(uint8_t byte) override {
    count++;
    crc = crc32Update(crc, byte);
  }
  size_t count = 0;
  uint32_t crc = 0xFFFFFFFFUL;
};

}  // namespace sdplib

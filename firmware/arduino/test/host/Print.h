// Print / Stream ขั้นต่ำสำหรับคอมไพล์ไลบรารีบน PC — เฉพาะเมธอดที่ไลบรารีเรียกจริง
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t byte) = 0;
  size_t write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) write(data[i]);
    return len;
  }
  size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), strlen(text)); }
  size_t print(unsigned long value) {
    char buf[16];
    snprintf(buf, sizeof buf, "%lu", value);
    return print(buf);
  }
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
};

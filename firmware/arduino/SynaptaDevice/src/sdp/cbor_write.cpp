#include "cbor_write.h"

#include <float.h>
#include <string.h>

#include "flash.h"

namespace sdplib {

uint8_t cborHead(uint8_t* out, uint8_t major, uint32_t arg) {
  const uint8_t m = static_cast<uint8_t>(major << 5);
  if (arg < 24) {
    out[0] = static_cast<uint8_t>(m | arg);
    return 1;
  }
  if (arg <= 0xFF) {
    out[0] = m | 24;
    out[1] = static_cast<uint8_t>(arg);
    return 2;
  }
  if (arg <= 0xFFFF) {
    out[0] = m | 25;
    out[1] = static_cast<uint8_t>(arg >> 8);
    out[2] = static_cast<uint8_t>(arg);
    return 3;
  }
  out[0] = m | 26;
  out[1] = static_cast<uint8_t>(arg >> 24);
  out[2] = static_cast<uint8_t>(arg >> 16);
  out[3] = static_cast<uint8_t>(arg >> 8);
  out[4] = static_cast<uint8_t>(arg);
  return 5;
}

void CborWriter::head(uint8_t major, uint32_t arg) {
  uint8_t buf[5];
  sink_.write(buf, cborHead(buf, major, arg));
}

void CborWriter::sint(int32_t v) {
  if (v >= 0) {
    head(kUint, static_cast<uint32_t>(v));
  } else {
    head(kNint, static_cast<uint32_t>(-1 - v));  // CBOR เก็บค่าลบเป็น -1 - n
  }
}

static void putBigEndian(Sink& sink, const uint8_t* le, uint8_t n) {
  for (uint8_t i = n; i > 0; i--) sink.put(le[i - 1]);  // MCU ที่ใช้ทั้งหมดเป็น little-endian
}

void CborWriter::number(double v) {
  const float f = static_cast<float>(v);
#if DBL_MANT_DIG > FLT_MANT_DIG  // double 64 บิตจริง (AVR: double = float)
  if (static_cast<double>(f) != v && v == v) {
    uint8_t le[8];
    memcpy(le, &v, 8);
    sink_.put(0xFB);
    putBigEndian(sink_, le, 8);
    return;
  }
#endif
  uint8_t le[4];
  memcpy(le, &f, 4);
  sink_.put(0xFA);
  putBigEndian(sink_, le, 4);
}

void CborWriter::text(const char* s, size_t len) {
  head(kText, static_cast<uint32_t>(len));
  sink_.write(reinterpret_cast<const uint8_t*>(s), len);
}

void CborWriter::text(const char* s) { text(s, strlen(s)); }

void CborWriter::flashText(const char* s) {
  const size_t len = flashLen(s);
  head(kText, static_cast<uint32_t>(len));
  for (size_t i = 0; i < len; i++) sink_.put(flashByte(s + i));
}

void CborWriter::bytes(const uint8_t* data, size_t len) {
  head(kBytes, static_cast<uint32_t>(len));
  sink_.write(data, len);
}

}  // namespace sdplib

// อ่านข้อความที่เก็บใน flash — AVR ต้องอ่านผ่าน pgm_read_byte เพราะ flash กับ RAM คนละ address space
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__AVR__)
#include <avr/pgmspace.h>
#define SDP_FLASH PROGMEM
namespace sdplib {
inline uint8_t flashByte(const char* p) { return pgm_read_byte(p); }
inline size_t flashLen(const char* p) { return strlen_P(p); }
inline void flashCopy(void* dst, const void* src, size_t n) { memcpy_P(dst, src, n); }
}  // namespace sdplib
#else
#define SDP_FLASH
namespace sdplib {
inline uint8_t flashByte(const char* p) { return static_cast<uint8_t>(*p); }
inline size_t flashLen(const char* p) { return strlen(p); }
inline void flashCopy(void* dst, const void* src, size_t n) { memcpy(dst, src, n); }
}  // namespace sdplib
#endif

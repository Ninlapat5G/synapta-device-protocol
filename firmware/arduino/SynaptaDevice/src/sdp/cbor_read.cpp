#include "cbor_read.h"

#include <float.h>
#include <math.h>
#include <string.h>

namespace sdplib {

static uint8_t argBytes(uint8_t info) { return info < 24 ? 0 : static_cast<uint8_t>(1u << (info - 24)); }

// ตัวเลข 8 ไบต์ที่เกิน 32 บิตอ่านเป็น 0xFFFFFFFF — ทุกที่ที่ใช้ค่านี้ (ความยาว, id, int32) ปฏิเสธค่าขนาดนั้นอยู่แล้ว
// เลี่ยงเลข 64 บิตซึ่งกิน flash ของ AVR มาก
static uint32_t readBigEndian(const uint8_t* p, uint8_t n) {
  if (n == 8) {
    if (p[0] | p[1] | p[2] | p[3]) return 0xFFFFFFFFUL;
    p += 4;
    n = 4;
  }
  uint32_t v = 0;
  for (uint8_t i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

bool readHead(const uint8_t* p, const uint8_t* end, uint8_t* major, uint32_t* arg, const uint8_t** next) {
  if (p >= end) return false;
  const uint8_t info = p[0] & 0x1F;
  if (info >= 28) return false;
  const uint8_t n = argBytes(info);
  if (static_cast<size_t>(end - p) < 1u + n) return false;
  *major = static_cast<uint8_t>(p[0] >> 5);
  *arg = n ? readBigEndian(p + 1, n) : info;
  *next = p + 1 + n;
  return true;
}

bool wellFormed(const uint8_t* p, const uint8_t* end, const uint8_t** next, uint8_t depth) {
  if (p >= end || depth > kMaxDepth) return false;
  const uint8_t major = static_cast<uint8_t>(p[0] >> 5);
  const uint8_t info = p[0] & 0x1F;
  if (info >= 28) return false;  // 28–30 สงวน, 31 = indefinite length ที่สเปกห้าม
  const uint8_t n = argBytes(info);
  if (static_cast<size_t>(end - p) < 1u + n) return false;
  const uint32_t arg = n ? readBigEndian(p + 1, n) : info;
  p += 1 + n;
  const uint32_t left = static_cast<uint32_t>(end - p);

  switch (major) {
    case 0:
    case 1:
      break;
    case 2:
    case 3:
      if (arg > left) return false;
      p += arg;
      break;
    case 4:
    case 5: {
      // สมาชิกทุกตัวยาวอย่างน้อย 1 ไบต์ — จำนวนที่อ้างเกินข้อมูลที่เหลือ ตัดทิ้งก่อนวนลูป
      if (arg > left || (major == 5 && arg > left / 2)) return false;
      const uint32_t items = major == 5 ? arg * 2 : arg;
      for (uint32_t i = 0; i < items; i++) {
        if (!wellFormed(p, end, &p, static_cast<uint8_t>(depth + 1))) return false;
      }
      break;
    }
    case 7:
      if (!(info == 20 || info == 21 || info == 22 || (info >= 25 && info <= 27))) return false;
      break;
    default:  // tag (6) ไม่อยู่ในสเปก
      return false;
  }
  *next = p;
  return true;
}

uint32_t CborRef::arg() const {
  const uint8_t info = p[0] & 0x1F;
  const uint8_t n = argBytes(info);
  return n ? readBigEndian(p + 1, n) : info;
}

const uint8_t* CborRef::payload() const { return p + 1 + argBytes(p[0] & 0x1F); }

CborRef CborRef::next() const {
  const uint8_t* q = payload();
  switch (major()) {
    case 2:
    case 3:
      return CborRef{q + static_cast<size_t>(arg())};
    case 4:
    case 5: {
      CborRef item{q};
      const uint32_t items = major() == 5 ? arg() * 2 : arg();
      for (uint32_t i = 0; i < items; i++) item = item.next();
      return item;
    }
    default:
      return CborRef{q};  // เลข, bool, float: ค่าอยู่ในหัวแล้ว
  }
}

bool CborRef::asInt(int32_t* out) const {
  const uint32_t a = arg();
  if (a > 0x7FFFFFFFUL) return false;  // ค่าลบเล็กสุดที่รับ = -2^31
  *out = major() == 0 ? static_cast<int32_t>(a) : -1 - static_cast<int32_t>(a);
  return true;
}

static double halfToDouble(uint16_t h) {
  const int exp = (h >> 10) & 0x1F;
  const int mant = h & 0x3FF;
  double v;
  if (exp == 0) {
    v = ldexp(mant, -24);
  } else if (exp == 31) {
    v = mant ? NAN : INFINITY;
  } else {
    v = ldexp(mant + 1024, exp - 25);
  }
  return (h & 0x8000) ? -v : v;
}

double CborRef::asDouble() const {
  if (major() == 0) return static_cast<double>(arg());
  if (major() == 1) return -1.0 - static_cast<double>(arg());
  const uint8_t* b = p + 1;
  if (p[0] == 0xF9) return halfToDouble(static_cast<uint16_t>((b[0] << 8) | b[1]));
  if (p[0] == 0xFA) {
    const uint32_t bits = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                          (static_cast<uint32_t>(b[2]) << 8) | b[3];
    float f;
    memcpy(&f, &bits, 4);
    return f;
  }
#if DBL_MANT_DIG > FLT_MANT_DIG  // double 64 บิตจริง (AVR: double = float)
  uint64_t bits = 0;
  for (uint8_t i = 0; i < 8; i++) bits = (bits << 8) | b[i];
  double d;
  memcpy(&d, &bits, 8);
  return d;
#else
  // AVR: double = float 32 บิต — ประกอบจากเลขชี้กำลังกับ mantissa 23 บิตบน (ความละเอียดเท่า float อยู่แล้ว)
  const int exp = ((b[0] & 0x7F) << 4) | (b[1] >> 4);
  const uint32_t top = (static_cast<uint32_t>(b[1] & 0x0F) << 19) | (static_cast<uint32_t>(b[2]) << 11) |
                       (static_cast<uint32_t>(b[3]) << 3) | (b[4] >> 5);  // 23 บิตบนของ mantissa
  const double mant = ldexp(static_cast<double>(top), -23);
  const double v = exp == 0 ? ldexp(mant, -1022) : exp == 0x7FF ? (top ? NAN : INFINITY) : ldexp(1.0 + mant, exp - 1023);
  return (b[0] & 0x80) ? -v : v;
#endif
}

bool CborRef::textEquals(const char* s, size_t len) const {
  return isText() && arg() == len && memcmp(payload(), s, len) == 0;
}

CborRef CborRef::mapGet(const char* key) const {
  const size_t len = strlen(key);
  CborRef k = first();
  for (uint32_t i = 0; i < arg(); i++) {
    const CborRef v = k.next();
    if (k.textEquals(key, len)) return v;
    k = v.next();
  }
  return CborRef{};
}

CborRef CborRef::mapGetUint(uint32_t key) const {
  CborRef k = first();
  for (uint32_t i = 0; i < arg(); i++) {
    const CborRef v = k.next();
    if (k.major() == 0 && k.arg() == key) return v;
    k = v.next();
  }
  return CborRef{};
}

}  // namespace sdplib

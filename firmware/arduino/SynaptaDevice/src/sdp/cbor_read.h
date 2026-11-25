// อ่าน CBOR ที่อยู่ใน buffer รับ — ตรวจรูปครั้งเดียวด้วย wellFormed() แล้วฟังก์ชันอื่นอ่านต่อได้โดยไม่ต้องเช็กขอบซ้ำ
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sdplib {

constexpr uint8_t kMaxDepth = 16;  // ตรงกับ sdp-core — recursive บน AVR ลึกไม่จำกัด = stack ล้นแล้วรีเซ็ต

// ตรวจว่า [p, end) ขึ้นต้นด้วย item ที่ถูกรูป: definite length, ความยาวไม่เกินข้อมูลที่มี, ไม่มี tag
// ผ่านแล้ว *next = ตำแหน่งถัดจาก item
bool wellFormed(const uint8_t* p, const uint8_t* end, const uint8_t** next, uint8_t depth = 0);

// อ่านหัวของ item ตัวเดียว (ยังไม่ตรวจเนื้อ) — false ถ้าขาด, indefinite หรือรหัสสงวน
bool readHead(const uint8_t* p, const uint8_t* end, uint8_t* major, uint32_t* arg, const uint8_t** next);

// มุมมองของ item หนึ่งตัวที่ผ่าน wellFormed() แล้ว
struct CborRef {
  const uint8_t* p;

  CborRef() : p(nullptr) {}
  explicit CborRef(const uint8_t* at) : p(at) {}

  bool valid() const { return p != nullptr; }
  uint8_t major() const { return static_cast<uint8_t>(p[0] >> 5); }
  uint32_t arg() const;  // ตัวเลขในหัว: ค่า, ความยาว หรือจำนวนสมาชิก (เกิน 32 บิต = 0xFFFFFFFF)
  const uint8_t* payload() const;     // ถัดจากหัว
  CborRef next() const;               // item ถัดไป

  bool isBool() const { return p[0] == 0xF4 || p[0] == 0xF5; }
  bool isInt() const { return major() <= 1; }
  bool isFloat() const { return p[0] == 0xF9 || p[0] == 0xFA || p[0] == 0xFB; }
  bool isNumber() const { return isInt() || isFloat(); }
  bool isText() const { return major() == 3; }
  bool isBytes() const { return major() == 2; }
  bool isArray() const { return major() == 4; }
  bool isMap() const { return major() == 5; }

  bool asBool() const { return p[0] == 0xF5; }
  bool asInt(int32_t* out) const;     // false ถ้าเกินช่วง int32
  double asDouble() const;
  bool textEquals(const char* s, size_t len) const;

  CborRef first() const { return CborRef{payload()}; }  // สมาชิกตัวแรกของ array/map
  CborRef mapGet(const char* key) const;                 // key ข้อความ (RAM) ไม่เจอ = !valid()
  CborRef mapGetUint(uint32_t key) const;                  // key ตัวเลข
};

}  // namespace sdplib

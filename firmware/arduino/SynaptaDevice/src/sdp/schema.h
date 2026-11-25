// schema ของ capability (wire-format.md §Schema)
//   lint()     — ตรวจตอน begin() ว่าผู้เขียน firmware เขียน schema ถูกตามที่สเปกรองรับ แจ้งทันทีทาง console
//   validate() — ด่านสุดท้ายก่อนเรียก handler: ตรวจแค่ชนิด, minimum/maximum, maxLength, maxItems, required
#pragma once

#include <stdint.h>

#include "cbor_read.h"

namespace sdplib {

constexpr uint8_t kMaxSchemaLevels = 3;

// ปัญหาที่เจอ: message อยู่ใน flash, where = ตำแหน่งใน JSON (lint) หรือ key ของ field ที่ผิด (validate) — อาจเป็น nullptr
struct Problem {
  const char* message = nullptr;
  const char* where = nullptr;
};

// mustBeObject = schema ชั้นบนสุดต้องเป็น type object (input_schema)
bool lintSchema(const char* json, bool mustBeObject, Problem* problem);

bool validateInput(const char* schema, CborRef input, Problem* problem);

}  // namespace sdplib

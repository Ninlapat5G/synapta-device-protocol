// อ่าน JSON ที่เก็บใน flash ทีละตัวอักษร โดยไม่คัดลอกลง RAM — ใช้กับ schema ของ capability
//
// ทุกตำแหน่ง (const char*) ชี้เข้าไปในข้อความ JSON ใน flash
// ฟังก์ชันที่ "ตรวจ" คืน nullptr เมื่อรูปแบบผิด ส่วนฟังก์ชันอ่านอื่น ๆ ถือว่าผ่าน skipValue() มาแล้ว
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cbor_write.h"

namespace sdplib {
namespace json {

constexpr uint8_t kMaxNesting = 16;

char at(const char* p);
const char* skipWs(const char* p);

// ตรวจและข้ามค่าหนึ่งตัว (ซ้อนลึกไม่เกิน kMaxNesting) คืนตำแหน่งถัดไป
const char* skipValue(const char* p, uint8_t depth = 0);

// ข้อความ: p ชี้ที่ '"' — คืนตำแหน่งหลัง '"' ปิด และความยาวหลังถอด escape เป็น UTF-8
const char* stringEnd(const char* p, size_t* utf8Len);
bool hasEscape(const char* p);                     // key ที่มี '\' ไม่รองรับ (ต้องเทียบ/เรียงแบบตรงตัว)
size_t rawLen(const char* p);                      // ความยาวข้อความที่ไม่มี escape
bool equalsFlash(const char* p, const char* text);  // text อยู่ใน flash
bool equalsRam(const char* p, const char* text, size_t len);

// สมาชิกของ object: firstKey/nextKey คืน nullptr เมื่อหมด
const char* firstKey(const char* object);
const char* valueOf(const char* key);
const char* nextKey(const char* key);
const char* find(const char* object, const char* flashKey);  // คืนตำแหน่งค่า หรือ nullptr

// สมาชิกของ array
const char* firstItem(const char* array);
const char* nextItem(const char* item);

size_t count(const char* container);  // จำนวนสมาชิกของ object หรือ array

// ตัวเลข: isInt = ไม่มีจุดหรือ e และอยู่ในช่วงที่ CBOR uint32/nint32 แทนได้
bool readNumber(const char* p, double* value, bool* isInt, bool* negative, uint32_t* magnitude);

// เขียนค่า JSON เป็น CBOR — key ใน object เรียงตามกฎ encoding (สั้นก่อนยาว แล้วเทียบ byte)
void emit(const char* p, CborWriter& w);

}  // namespace json
}  // namespace sdplib

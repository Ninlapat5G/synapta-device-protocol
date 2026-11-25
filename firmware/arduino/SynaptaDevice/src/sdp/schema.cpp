#include "schema.h"

#include "flash.h"
#include "json.h"

namespace sdplib {

namespace {

const char kType[] SDP_FLASH = "type";
const char kProperties[] SDP_FLASH = "properties";
const char kRequired[] SDP_FLASH = "required";
const char kMinimum[] SDP_FLASH = "minimum";
const char kMaximum[] SDP_FLASH = "maximum";
const char kEnum[] SDP_FLASH = "enum";
const char kMaxLength[] SDP_FLASH = "maxLength";
const char kItems[] SDP_FLASH = "items";
const char kMaxItems[] SDP_FLASH = "maxItems";
const char kDescription[] SDP_FLASH = "description";
const char kContentEncoding[] SDP_FLASH = "contentEncoding";
const char kBase64[] SDP_FLASH = "base64";

const char kObject[] SDP_FLASH = "object";
const char kBoolean[] SDP_FLASH = "boolean";
const char kInteger[] SDP_FLASH = "integer";
const char kNumber[] SDP_FLASH = "number";
const char kString[] SDP_FLASH = "string";
const char kArray[] SDP_FLASH = "array";

// ข้อความ lint — ผู้เขียน firmware อ่านใน console
const char kBadJson[] SDP_FLASH = "invalid JSON";
const char kNotObject[] SDP_FLASH = "schema must be a JSON object";
const char kTopNotObject[] SDP_FLASH = "input_schema must have \"type\":\"object\"";
const char kTooDeep[] SDP_FLASH = "schema nested deeper than 3 levels";
const char kEscapedKey[] SDP_FLASH = "escape sequences in keys are not supported";
const char kDuplicateKey[] SDP_FLASH = "duplicate key";
const char kUnknownKeyword[] SDP_FLASH = "keyword not supported by SDP v1";
const char kMissingType[] SDP_FLASH = "missing \"type\"";
const char kBadType[] SDP_FLASH = "\"type\" must be object, boolean, integer, number, string or array";
const char kBadProperties[] SDP_FLASH = "\"properties\" must be an object and needs \"type\":\"object\"";
const char kBadRequired[] SDP_FLASH = "\"required\" must list names from \"properties\"";
const char kBadNumber[] SDP_FLASH = "must be a number (integers up to 32 bits)";
const char kBadCount[] SDP_FLASH = "must be a non-negative integer";
const char kBadEnum[] SDP_FLASH = "\"enum\" must be a non-empty array";
const char kBadItems[] SDP_FLASH = "\"items\" needs \"type\":\"array\"";
const char kBadString[] SDP_FLASH = "must be a string";
const char kBadEncoding[] SDP_FLASH = "\"contentEncoding\" must be \"base64\" with \"type\":\"string\"";

// ข้อความ validate — ส่งกลับใน error 3 ต่อท้ายชื่อ field
const char kMustBeObject[] SDP_FLASH = "must be an object";
const char kMustBeBoolean[] SDP_FLASH = "must be boolean";
const char kMustBeInteger[] SDP_FLASH = "must be an integer";
const char kMustBeNumber[] SDP_FLASH = "must be a number";
const char kMustBeString[] SDP_FLASH = "must be a string";
const char kMustBeBytes[] SDP_FLASH = "must be bytes";
const char kMustBeArray[] SDP_FLASH = "must be an array";
const char kBelowMinimum[] SDP_FLASH = "is below minimum";
const char kAboveMaximum[] SDP_FLASH = "is above maximum";
const char kTooLong[] SDP_FLASH = "is longer than maxLength";
const char kTooManyItems[] SDP_FLASH = "has more items than maxItems";
const char kMissing[] SDP_FLASH = "is required";

const char kOutOfRange[] SDP_FLASH = "is outside the 32-bit integer range";

const char* const kKeywords[] = {kType,      kProperties, kRequired, kMinimum,     kMaximum,        kEnum,
                                 kMaxLength, kItems,      kMaxItems, kDescription, kContentEncoding};
const char* const kTypes[] = {kObject, kBoolean, kInteger, kNumber, kString, kArray};

bool fail(Problem* problem, const char* message, const char* where) {
  problem->message = message;
  problem->where = where;
  return false;
}

bool isKeyword(const char* key) {
  for (const char* kw : kKeywords) {
    if (json::equalsFlash(key, kw)) return true;
  }
  return false;
}

bool sameKey(const char* a, const char* b) {
  const size_t len = json::rawLen(a);
  if (len != json::rawLen(b)) return false;
  for (size_t i = 1; i <= len; i++) {
    if (json::at(a + i) != json::at(b + i)) return false;
  }
  return true;
}

// key ของ object ต้องไม่มี escape และไม่ซ้ำ — ไม่งั้นเรียง key ตามกฎ encoding ไม่ได้
bool lintKeys(const char* object, Problem* problem) {
  for (const char* k = json::firstKey(object); k; k = json::nextKey(k)) {
    if (json::hasEscape(k)) return fail(problem, kEscapedKey, k);
    for (const char* other = json::firstKey(object); other != k; other = json::nextKey(other)) {
      if (sameKey(k, other)) return fail(problem, kDuplicateKey, k);
    }
  }
  return true;
}

// ตัวเลขที่ firmware เขียนเป็น CBOR ได้ — wantCount = ต้องเป็นจำนวนเต็มไม่ติดลบ
bool isNumber(const char* value, bool wantCount) {
  double v;
  bool isInt, negative;
  uint32_t mag;
  const char c = json::at(value);
  if (c != '-' && (c < '0' || c > '9')) return false;
  if (!json::readNumber(value, &v, &isInt, &negative, &mag)) return false;
  return !wantCount || (isInt && !negative);
}

bool typeIs(const char* schema, const char* name) {
  const char* t = json::find(schema, kType);
  return t && json::at(t) == '"' && json::equalsFlash(t, name);
}

bool lintLevel(const char* schema, uint8_t level, Problem* problem) {
  if (level > kMaxSchemaLevels) return fail(problem, kTooDeep, schema);
  if (json::at(schema) != '{') return fail(problem, kNotObject, schema);
  if (!lintKeys(schema, problem)) return false;

  const char* type = json::find(schema, kType);
  if (!type) return fail(problem, kMissingType, schema);
  bool knownType = false;
  for (const char* name : kTypes) {
    if (json::at(type) == '"' && json::equalsFlash(type, name)) knownType = true;
  }
  if (!knownType) return fail(problem, kBadType, type);

  for (const char* k = json::firstKey(schema); k; k = json::nextKey(k)) {
    const char* v = json::valueOf(k);
    if (!isKeyword(k)) return fail(problem, kUnknownKeyword, k);
    if (json::equalsFlash(k, kProperties)) {
      if (json::at(v) != '{' || !typeIs(schema, kObject)) return fail(problem, kBadProperties, k);
      if (!lintKeys(v, problem)) return false;
      for (const char* p = json::firstKey(v); p; p = json::nextKey(p)) {
        if (!lintLevel(json::valueOf(p), static_cast<uint8_t>(level + 1), problem)) return false;
      }
    } else if (json::equalsFlash(k, kRequired)) {
      const char* props = json::find(schema, kProperties);
      if (json::at(v) != '[' || !props) return fail(problem, kBadRequired, k);
      for (const char* item = json::firstItem(v); item; item = json::nextItem(item)) {
        if (json::at(item) != '"' || json::hasEscape(item)) return fail(problem, kBadRequired, item);
        bool listed = false;
        for (const char* p = json::firstKey(props); p; p = json::nextKey(p)) listed = listed || sameKey(p, item);
        if (!listed) return fail(problem, kBadRequired, item);
      }
    } else if (json::equalsFlash(k, kMinimum) || json::equalsFlash(k, kMaximum)) {
      if (!isNumber(v, false)) return fail(problem, kBadNumber, v);
    } else if (json::equalsFlash(k, kMaxLength) || json::equalsFlash(k, kMaxItems)) {
      if (!isNumber(v, true)) return fail(problem, kBadCount, v);
    } else if (json::equalsFlash(k, kEnum)) {
      if (json::at(v) != '[' || !json::firstItem(v)) return fail(problem, kBadEnum, v);
    } else if (json::equalsFlash(k, kItems)) {
      if (!typeIs(schema, kArray)) return fail(problem, kBadItems, k);
      if (!lintLevel(v, static_cast<uint8_t>(level + 1), problem)) return false;
    } else if (json::equalsFlash(k, kDescription)) {
      if (json::at(v) != '"') return fail(problem, kBadString, v);
    } else if (json::equalsFlash(k, kContentEncoding)) {
      if (json::at(v) != '"' || !json::equalsFlash(v, kBase64) || !typeIs(schema, kString)) {
        return fail(problem, kBadEncoding, k);
      }
    }
  }
  return true;
}

// ---------- validate ----------

double numberOf(const char* value) {
  double v;
  bool isInt, negative;
  uint32_t mag;
  json::readNumber(value, &v, &isInt, &negative, &mag);
  return v;
}

uint32_t countOf(const char* value) { return static_cast<uint32_t>(numberOf(value)); }

// จำนวนตัวอักษร (code point) ของข้อความ UTF-8 — maxLength ของ JSON Schema นับแบบนี้
uint32_t codePoints(CborRef text) {
  const uint8_t* p = text.payload();
  uint32_t n = 0;
  for (uint32_t i = 0; i < text.arg(); i++) {
    if ((p[i] & 0xC0) != 0x80) n++;
  }
  return n;
}

bool validateValue(const char* schema, CborRef v, const char* key, Problem* problem) {
  const char* type = json::find(schema, kType);
  if (json::equalsFlash(type, kObject)) {
    if (!v.isMap()) return fail(problem, kMustBeObject, key);
    const char* props = json::find(schema, kProperties);
    if (props) {
      for (const char* p = json::firstKey(props); p; p = json::nextKey(p)) {
        CborRef k = v.first();
        for (uint32_t i = 0; i < v.arg(); i++) {
          const CborRef value = k.next();
          if (k.isText() && json::equalsRam(p, reinterpret_cast<const char*>(k.payload()), static_cast<size_t>(k.arg()))) {
            if (!validateValue(json::valueOf(p), value, p, problem)) return false;
            break;
          }
          k = value.next();
        }
      }
    }
    const char* required = json::find(schema, kRequired);
    if (required) {
      for (const char* name = json::firstItem(required); name; name = json::nextItem(name)) {
        bool present = false;
        CborRef k = v.first();
        for (uint32_t i = 0; i < v.arg() && !present; i++) {
          present = k.isText() && json::equalsRam(name, reinterpret_cast<const char*>(k.payload()), static_cast<size_t>(k.arg()));
          k = k.next().next();
        }
        if (!present) return fail(problem, kMissing, name);
      }
    }
    return true;
  }
  if (json::equalsFlash(type, kBoolean)) return v.isBool() || fail(problem, kMustBeBoolean, key);
  if (json::equalsFlash(type, kInteger) || json::equalsFlash(type, kNumber)) {
    const bool integer = json::equalsFlash(type, kInteger);
    if (integer ? !v.isInt() : !v.isNumber()) return fail(problem, integer ? kMustBeInteger : kMustBeNumber, key);
    const double x = v.asDouble();
    const char* min = json::find(schema, kMinimum);
    const char* max = json::find(schema, kMaximum);
    if (min && x < numberOf(min)) return fail(problem, kBelowMinimum, key);
    if (max && x > numberOf(max)) return fail(problem, kAboveMaximum, key);
    int32_t ignored;
    if (integer && !v.asInt(&ignored)) return fail(problem, kOutOfRange, key);
    return true;
  }
  if (json::equalsFlash(type, kString)) {
    const char* maxLength = json::find(schema, kMaxLength);
    if (json::find(schema, kContentEncoding)) {
      if (!v.isBytes()) return fail(problem, kMustBeBytes, key);
      // maxLength นับตัวอักษร base64 ที่ host เห็น → ข้อมูลดิบยาวได้ไม่เกิน 3/4 ของนั้น
      if (maxLength && v.arg() > countOf(maxLength) * 3 / 4) return fail(problem, kTooLong, key);
      return true;
    }
    if (!v.isText()) return fail(problem, kMustBeString, key);
    if (maxLength && codePoints(v) > countOf(maxLength)) return fail(problem, kTooLong, key);
    return true;
  }
  // array
  if (!v.isArray()) return fail(problem, kMustBeArray, key);
  const char* maxItems = json::find(schema, kMaxItems);
  if (maxItems && v.arg() > countOf(maxItems)) return fail(problem, kTooManyItems, key);
  const char* items = json::find(schema, kItems);
  if (items) {
    CborRef item = v.first();
    for (uint32_t i = 0; i < v.arg(); i++) {
      if (!validateValue(items, item, key, problem)) return false;
      item = item.next();
    }
  }
  return true;
}

}  // namespace

bool lintSchema(const char* text, bool mustBeObject, Problem* problem) {
  const char* start = json::skipWs(text);
  const char* end = json::skipValue(start);
  if (!end || json::at(json::skipWs(end)) != '\0') return fail(problem, kBadJson, end ? end : start);
  if (!lintLevel(start, 1, problem)) return false;
  if (mustBeObject && !typeIs(start, kObject)) return fail(problem, kTopNotObject, start);
  return true;
}

bool validateInput(const char* schema, CborRef input, Problem* problem) {
  return validateValue(json::skipWs(schema), input, nullptr, problem);
}

}  // namespace sdplib

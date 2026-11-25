#include "json.h"

#include "flash.h"

namespace sdplib {
namespace json {

char at(const char* p) { return static_cast<char>(flashByte(p)); }

const char* skipWs(const char* p) {
  for (;;) {
    const char c = at(p);
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return p;
    p++;
  }
}

static int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int32_t hex4(const char* p) {
  int32_t v = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const int d = hexDigit(at(p + i));
    if (d < 0) return -1;
    v = (v << 4) | d;
  }
  return v;
}

static uint8_t utf8Len(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

// ถอด escape หนึ่งตัวที่ q (ชี้ที่ '\') — คืนตำแหน่งถัดไป และ code point ใน *cp; nullptr = escape ผิด
static const char* decodeEscape(const char* q, uint32_t* cp) {
  switch (at(q + 1)) {
    case '"': *cp = '"'; return q + 2;
    case '\\': *cp = '\\'; return q + 2;
    case '/': *cp = '/'; return q + 2;
    case 'b': *cp = '\b'; return q + 2;
    case 'f': *cp = '\f'; return q + 2;
    case 'n': *cp = '\n'; return q + 2;
    case 'r': *cp = '\r'; return q + 2;
    case 't': *cp = '\t'; return q + 2;
    case 'u': {
      const int32_t hi = hex4(q + 2);
      if (hi < 0 || (hi >= 0xDC00 && hi <= 0xDFFF)) return nullptr;
      if (hi < 0xD800 || hi > 0xDBFF) {
        *cp = static_cast<uint32_t>(hi);
        return q + 6;
      }
      // surrogate pair: ต้องมี \uDC00–\uDFFF ตามมาทันที
      if (at(q + 6) != '\\' || at(q + 7) != 'u') return nullptr;
      const int32_t lo = hex4(q + 8);
      if (lo < 0xDC00 || lo > 0xDFFF) return nullptr;
      *cp = 0x10000 + ((static_cast<uint32_t>(hi) - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00);
      return q + 12;
    }
    default:
      return nullptr;
  }
}

const char* stringEnd(const char* p, size_t* utf8Len_) {
  size_t len = 0;
  const char* q = p + 1;
  for (;;) {
    const char c = at(q);
    if (c == '"') break;
    if (static_cast<uint8_t>(c) < 0x20) return nullptr;  // รวม '\0' = ข้อความไม่ปิด
    if (c == '\\') {
      uint32_t cp;
      q = decodeEscape(q, &cp);
      if (!q) return nullptr;
      len += utf8Len(cp);
    } else {
      len++;  // UTF-8 หลายไบต์ผ่านไปทีละไบต์ตามเดิม
      q++;
    }
  }
  if (utf8Len_) *utf8Len_ = len;
  return q + 1;
}

bool hasEscape(const char* p) {
  for (const char* q = p + 1; at(q) != '"'; q++) {
    if (at(q) == '\\') return true;
  }
  return false;
}

size_t rawLen(const char* p) {
  size_t n = 0;
  while (at(p + 1 + n) != '"') n++;
  return n;
}

bool equalsFlash(const char* p, const char* text) {
  size_t i = 0;
  for (; flashByte(text + i) != 0; i++) {
    if (at(p + 1 + i) != static_cast<char>(flashByte(text + i))) return false;
  }
  return at(p + 1 + i) == '"';
}

bool equalsRam(const char* p, const char* text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = at(p + 1 + i);
    if (c == '"' || c == '\\' || c == 0 || c != text[i]) return false;
  }
  return at(p + 1 + len) == '"';
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static const char* numberEnd(const char* p) {
  if (at(p) == '-') p++;
  if (at(p) == '0') {
    p++;
  } else if (isDigit(at(p))) {
    while (isDigit(at(p))) p++;
  } else {
    return nullptr;
  }
  if (at(p) == '.') {
    p++;
    if (!isDigit(at(p))) return nullptr;
    while (isDigit(at(p))) p++;
  }
  if (at(p) == 'e' || at(p) == 'E') {
    p++;
    if (at(p) == '+' || at(p) == '-') p++;
    if (!isDigit(at(p))) return nullptr;
    while (isDigit(at(p))) p++;
  }
  return p;
}

static const char* literalEnd(const char* p, const char* word) {
  for (; *word; word++, p++) {
    if (at(p) != *word) return nullptr;
  }
  return p;
}

const char* skipValue(const char* p, uint8_t depth) {
  p = skipWs(p);
  const char c = at(p);
  if (c == '{' || c == '[') {
    if (depth >= kMaxNesting) return nullptr;
    const char close = c == '{' ? '}' : ']';
    p = skipWs(p + 1);
    if (at(p) == close) return p + 1;
    for (;;) {
      if (c == '{') {
        if (at(p) != '"') return nullptr;
        p = stringEnd(p, nullptr);
        if (!p) return nullptr;
        p = skipWs(p);
        if (at(p) != ':') return nullptr;
        p++;
      }
      p = skipValue(p, static_cast<uint8_t>(depth + 1));
      if (!p) return nullptr;
      p = skipWs(p);
      if (at(p) == close) return p + 1;
      if (at(p) != ',') return nullptr;
      p = skipWs(p + 1);
    }
  }
  if (c == '"') return stringEnd(p, nullptr);
  if (c == 't') return literalEnd(p, "true");
  if (c == 'f') return literalEnd(p, "false");
  if (c == 'n') return literalEnd(p, "null");
  return numberEnd(p);
}

const char* firstKey(const char* object) {
  const char* p = skipWs(object + 1);
  return at(p) == '}' ? nullptr : p;
}

const char* valueOf(const char* key) {
  const char* p = skipWs(stringEnd(key, nullptr));
  return skipWs(p + 1);  // ข้าม ':'
}

const char* nextKey(const char* key) {
  const char* p = skipWs(skipValue(valueOf(key)));
  return at(p) == ',' ? skipWs(p + 1) : nullptr;
}

const char* find(const char* object, const char* flashKey) {
  for (const char* k = firstKey(object); k; k = nextKey(k)) {
    if (equalsFlash(k, flashKey)) return valueOf(k);
  }
  return nullptr;
}

const char* firstItem(const char* array) {
  const char* p = skipWs(array + 1);
  return at(p) == ']' ? nullptr : p;
}

const char* nextItem(const char* item) {
  const char* p = skipWs(skipValue(item));
  return at(p) == ',' ? skipWs(p + 1) : nullptr;
}

size_t count(const char* container) {
  size_t n = 0;
  if (at(container) == '{') {
    for (const char* k = firstKey(container); k; k = nextKey(k)) n++;
  } else {
    for (const char* i = firstItem(container); i; i = nextItem(i)) n++;
  }
  return n;
}

bool readNumber(const char* p, double* value, bool* isInt, bool* negative, uint32_t* magnitude) {
  const char* end = numberEnd(p);
  if (!end) return false;
  *negative = at(p) == '-';
  if (*negative) p++;

  double mantissa = 0;
  uint32_t mag = 0;
  bool fits = true;
  int exp10 = 0;
  *isInt = true;
  for (; isDigit(at(p)); p++) {
    const uint8_t d = static_cast<uint8_t>(at(p) - '0');
    mantissa = mantissa * 10 + d;
    if (mag > (0xFFFFFFFFUL - d) / 10) fits = false;
    mag = mag * 10 + d;
  }
  if (at(p) == '.') {
    *isInt = false;
    for (p++; isDigit(at(p)); p++) {
      mantissa = mantissa * 10 + (at(p) - '0');
      exp10--;
    }
  }
  if (at(p) == 'e' || at(p) == 'E') {
    *isInt = false;
    p++;
    const bool expNegative = at(p) == '-';
    if (at(p) == '+' || at(p) == '-') p++;
    int e = 0;
    for (; isDigit(at(p)); p++) {
      if (e < 1000) e = e * 10 + (at(p) - '0');
    }
    exp10 += expNegative ? -e : e;
  }
  // หารด้วย 10^k ที่แทนได้ตรง → ได้ค่าใกล้สุดเหมือนตัวแปลงของ PC (เช่น 0.1, 23.5)
  double scale = 1;
  for (int i = exp10 < 0 ? -exp10 : exp10; i > 0; i--) scale *= 10;
  double v = exp10 < 0 ? mantissa / scale : mantissa * scale;
  *value = *negative ? -v : v;
  *magnitude = mag;
  if (*isInt && !fits) return false;  // จำนวนเต็มเกิน 32 บิต — firmware เขียนเป็น CBOR ไม่ได้
  return true;
}

static void emitString(const char* p, CborWriter& w) {
  size_t len;
  stringEnd(p, &len);
  w.head(kText, static_cast<uint32_t>(len));
  for (const char* q = p + 1; at(q) != '"';) {
    if (at(q) != '\\') {
      const uint8_t b = static_cast<uint8_t>(at(q++));
      w.raw(&b, 1);
      continue;
    }
    uint32_t cp;
    q = decodeEscape(q, &cp);
    uint8_t buf[4];
    const uint8_t n = utf8Len(cp);
    if (n == 1) {
      buf[0] = static_cast<uint8_t>(cp);
    } else if (n == 2) {
      buf[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
      buf[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else if (n == 3) {
      buf[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
      buf[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      buf[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    } else {
      buf[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
      buf[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
      buf[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      buf[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    }
    w.raw(buf, n);
  }
}

// ลำดับ key ตามกฎ encoding: หัว CBOR ของข้อความยาวขึ้นตามความยาว จึงเทียบความยาวก่อน แล้วเทียบ byte
static bool keyLess(const char* a, const char* b) {
  const size_t la = rawLen(a);
  const size_t lb = rawLen(b);
  if (la != lb) return la < lb;
  for (size_t i = 1; i <= la; i++) {
    const uint8_t ca = static_cast<uint8_t>(at(a + i));
    const uint8_t cb = static_cast<uint8_t>(at(b + i));
    if (ca != cb) return ca < cb;
  }
  return false;
}

void emit(const char* p, CborWriter& w) {
  p = skipWs(p);
  switch (at(p)) {
    case '{': {
      const size_t n = count(p);
      w.map(n);
      // เลือก key ที่น้อยสุดซึ่งมากกว่าตัวก่อนหน้า ทีละตัว — เรียงได้โดยไม่ต้องมี RAM เก็บรายการ key
      const char* last = nullptr;
      for (size_t i = 0; i < n; i++) {
        const char* best = nullptr;
        for (const char* k = firstKey(p); k; k = nextKey(k)) {
          if ((!last || keyLess(last, k)) && (!best || keyLess(k, best))) best = k;
        }
        const size_t len = rawLen(best);
        w.head(kText, static_cast<uint32_t>(len));
        for (size_t j = 1; j <= len; j++) {
          const uint8_t b = static_cast<uint8_t>(at(best + j));
          w.raw(&b, 1);
        }
        emit(valueOf(best), w);
        last = best;
      }
      break;
    }
    case '[':
      w.array(count(p));
      for (const char* item = firstItem(p); item; item = nextItem(item)) emit(item, w);
      break;
    case '"':
      emitString(p, w);
      break;
    case 't':
      w.boolean(true);
      break;
    case 'f':
      w.boolean(false);
      break;
    case 'n': {
      const uint8_t null = 0xF6;
      w.raw(&null, 1);
      break;
    }
    default: {
      double v;
      bool isInt, negative;
      uint32_t mag;
      readNumber(p, &v, &isInt, &negative, &mag);
      if (!isInt) {
        w.number(v);
      } else if (negative && mag > 0) {
        w.head(kNint, mag - 1);
      } else {
        w.head(kUint, mag);
      }
    }
  }
}

}  // namespace json
}  // namespace sdplib

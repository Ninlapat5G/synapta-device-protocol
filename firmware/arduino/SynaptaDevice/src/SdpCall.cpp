// ค่าที่ handler อ่าน (SdpInput) และเขียน (SdpOutput) ของหนึ่งคำสั่ง
#include <string.h>

#include "SynaptaDevice.h"

using sdplib::CborRef;

// ---------- SdpInput ----------

CborRef SdpInput::value(const char* key) const { return map_.valid() ? map_.mapGet(key) : CborRef{}; }

bool SdpInput::has(const char* key) const { return value(key).valid(); }

bool SdpInput::getBool(const char* key, bool fallback) const {
  const CborRef v = value(key);
  return v.valid() && v.isBool() ? v.asBool() : fallback;
}

long SdpInput::getInt(const char* key, long fallback) const {
  const CborRef v = value(key);
  int32_t out;
  return v.valid() && v.isInt() && v.asInt(&out) ? static_cast<long>(out) : fallback;
}

double SdpInput::getNumber(const char* key, double fallback) const {
  const CborRef v = value(key);
  return v.valid() && v.isNumber() ? v.asDouble() : fallback;
}

size_t SdpInput::getText(const char* key, char* out, size_t capacity) const {
  const CborRef v = value(key);
  if (capacity) out[0] = '\0';
  if (!v.valid() || !v.isText()) return 0;
  const size_t len = static_cast<size_t>(v.arg());
  if (capacity) {
    size_t n = len < capacity - 1 ? len : capacity - 1;
    // ตัดที่ขอบตัวอักษร — ไม่ทิ้งครึ่งตัวของ UTF-8 หลายไบต์ไว้ท้าย
    while (n < len && n > 0 && (v.payload()[n] & 0xC0) == 0x80) n--;
    memcpy(out, v.payload(), n);
    out[n] = '\0';
  }
  return len;
}

const uint8_t* SdpInput::getBytes(const char* key, size_t* len) const {
  const CborRef v = value(key);
  if (!v.valid() || !v.isBytes()) return nullptr;
  *len = static_cast<size_t>(v.arg());
  return v.payload();
}

// ---------- SdpOutput ----------

void SdpOutput::reset(uint8_t* buffer, uint16_t capacity) {
  buf_ = buffer;
  cap_ = capacity;
  len_ = 0;
  depth_ = 0;
  overflow_ = false;
  misordered_ = false;
  levels_[0] = Level{0, 0, nullptr, true};
  const uint8_t placeholder = 0xA0;  // หัว map ชั่วคราว — แก้เป็นจำนวนจริงตอน finish()
  put(&placeholder, 1);
}

void SdpOutput::put(const uint8_t* data, size_t len) {
  if (overflow_ || len_ + len > cap_) {
    overflow_ = true;
    return;
  }
  memcpy(buf_ + len_, data, len);
  len_ = static_cast<uint16_t>(len_ + len);
}

void SdpOutput::head(uint8_t major, uint32_t arg) {
  uint8_t h[5];
  put(h, sdplib::cborHead(h, major, arg));
}

// ลำดับ key ตามกฎ encoding: สั้นก่อนยาว แล้วเทียบ byte
static bool keyBefore(const char* a, const char* b) {
  const size_t la = strlen(a);
  const size_t lb = strlen(b);
  if (la != lb) return la < lb;
  return memcmp(a, b, la) < 0;
}

bool SdpOutput::entry(const char* key) {
  Level& level = levels_[depth_];
  if (level.isMap != (key != nullptr)) {  // map ต้องมี key, array ห้ามมี
    overflow_ = true;
    return false;
  }
  if (key) {
    if (level.lastKey && !keyBefore(level.lastKey, key)) misordered_ = true;
    level.lastKey = key;
    head(sdplib::kText, static_cast<uint32_t>(strlen(key)));
    put(reinterpret_cast<const uint8_t*>(key), strlen(key));
  }
  level.count++;
  return !overflow_;
}

SdpOutput& SdpOutput::add(const char* key, bool value) {
  if (entry(key)) {
    const uint8_t b = value ? 0xF5 : 0xF4;
    put(&b, 1);
  }
  return *this;
}

SdpOutput& SdpOutput::add(const char* key, long value) {
  if (entry(key)) {
    if (value >= 0) {
      head(sdplib::kUint, static_cast<uint32_t>(value));
    } else {
      head(sdplib::kNint, static_cast<uint32_t>(-1 - value));
    }
  }
  return *this;
}

SdpOutput& SdpOutput::add(const char* key, unsigned long value) {
  if (entry(key)) head(sdplib::kUint, static_cast<uint32_t>(value));
  return *this;
}

// ตัวเขียนชั่วคราวที่ชี้ลง buffer ของ SdpOutput — ใช้กฎเลือก float 32/64 บิตตัวเดียวกับ CborWriter
namespace {
class OutputSink : public sdplib::Sink {
 public:
  explicit OutputSink(uint8_t* tmp) : tmp_(tmp) {}
  void put(uint8_t b) override { tmp_[n++] = b; }
  uint8_t* tmp_;
  uint8_t n = 0;
};
}  // namespace

SdpOutput& SdpOutput::add(const char* key, double value) {
  if (entry(key)) {
    uint8_t tmp[9];
    OutputSink sink(tmp);
    sdplib::CborWriter(sink).number(value);
    put(tmp, sink.n);
  }
  return *this;
}

SdpOutput& SdpOutput::add(const char* key, const char* text) {
  if (entry(key)) {
    head(sdplib::kText, static_cast<uint32_t>(strlen(text)));
    put(reinterpret_cast<const uint8_t*>(text), strlen(text));
  }
  return *this;
}

SdpOutput& SdpOutput::addBytes(const char* key, const uint8_t* data, size_t len) {
  if (entry(key)) {
    head(sdplib::kBytes, static_cast<uint32_t>(len));
    put(data, len);
  }
  return *this;
}

void SdpOutput::open(const char* key, bool isMap) {
  if (!entry(key)) return;
  if (depth_ + 1 >= kMaxLevels) {
    overflow_ = true;
    return;
  }
  const uint8_t placeholder = isMap ? 0xA0 : 0x80;
  levels_[++depth_] = Level{len_, 0, nullptr, isMap};
  put(&placeholder, 1);
}

SdpOutput& SdpOutput::beginObject(const char* key) {
  open(key, true);
  return *this;
}

SdpOutput& SdpOutput::beginArray(const char* key) {
  open(key, false);
  return *this;
}

SdpOutput& SdpOutput::item(bool value) { return add(nullptr, value); }
SdpOutput& SdpOutput::item(long value) { return add(nullptr, value); }
SdpOutput& SdpOutput::item(double value) { return add(nullptr, value); }
SdpOutput& SdpOutput::item(const char* text) { return add(nullptr, text); }

// ปิดชั้นปัจจุบัน: เขียนจำนวนสมาชิกจริงลงหัว ถ้าหัวยาวกว่า 1 ไบต์ (≥ 24 ตัว) ขยับข้อมูลหลังหัวออกไป
static bool closeLevel(uint8_t* buf, uint16_t* len, uint16_t cap, uint16_t headerPos, uint16_t count, bool isMap) {
  uint8_t h[5];
  const uint8_t n = sdplib::cborHead(h, isMap ? sdplib::kMap : sdplib::kArray, count);
  if (n > 1) {
    if (*len + n - 1 > cap) return false;
    memmove(buf + headerPos + n, buf + headerPos + 1, *len - headerPos - 1);
    *len = static_cast<uint16_t>(*len + n - 1);
  }
  memcpy(buf + headerPos, h, n);
  return true;
}

SdpOutput& SdpOutput::end() {
  if (depth_ == 0) {
    overflow_ = true;  // end() เกินจำนวน begin
    return *this;
  }
  const Level& level = levels_[depth_--];
  if (!overflow_ && !closeLevel(buf_, &len_, cap_, level.headerPos, level.count, level.isMap)) overflow_ = true;
  return *this;
}

bool SdpOutput::finish() {
  while (depth_ > 0) end();
  if (!overflow_ && !closeLevel(buf_, &len_, cap_, 0, levels_[0].count, true)) overflow_ = true;
  return !overflow_;
}

// ---------- SdpCall ----------

void SdpCall::fail(uint8_t code, const char* message) {
  errorCode_ = code;
  errorMessage_ = message;
  errorInFlash_ = false;
}

void SdpCall::fail(uint8_t code, const __FlashStringHelper* message) {
  errorCode_ = code;
  errorMessage_ = reinterpret_cast<const char*>(message);
  errorInFlash_ = true;
}

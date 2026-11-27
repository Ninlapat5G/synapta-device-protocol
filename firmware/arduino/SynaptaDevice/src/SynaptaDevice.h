// SynaptaDevice — ทำให้บอร์ด Arduino พูด SDP v1 ผ่าน serial ให้ AI agent ค้นหาและสั่งงานได้
//
// ตัวอย่างเต็มอยู่ที่ examples/LedDemo — โครงย่อ:
//
//   SDP_CAPABILITY(ledSet, "led.set", "Turn the LED on or off", SDP_ACTUATOR,
//                  R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})",
//                  SDP_NO_SCHEMA) {
//     digitalWrite(LED_PIN, call.input().getBool("on"));
//   }
//   const SdpCapability* const CAPS[] = {&ledSet};
//   SynaptaDevice sdp;
//   void setup() { Serial.begin(115200); sdp.onLinkLost(allOff); sdp.begin(Serial, CONFIG, CAPS); }
//   void loop()  { sdp.loop(); }
//
// ข้อตกลงที่ผู้เขียน firmware ต้องรู้
// - handler ห้ามบล็อกนานกว่า heartbeat_ms — งานยาวให้ตอบทันทีแล้วแจ้งจบด้วย event
// - อุปกรณ์ที่มี capability ระดับ actuator/destructive ต้องเรียก onLinkLost() ก่อน begin()
//   ไม่งั้นคำสั่งพวกนั้นถูกปฏิเสธ (error 5) — กันปั๊ม/มอเตอร์ค้างเมื่อ PC หลุด
// - ห้ามพิมพ์ log ระหว่าง handler กำลังเขียน output — พิมพ์ได้ทุกที่อื่น (host เก็บเป็น console log)
#pragma once

#include <Arduino.h>

#include "sdp/cbor_read.h"
#include "sdp/cbor_write.h"
#include "sdp/flash.h"
#include "sdp/frame_rx.h"
#include "sdp/frame_tx.h"
#include "sdp/schema.h"

#define SYNAPTA_DEVICE_VERSION "0.3.0"

enum class SdpDanger : uint8_t { None = 0, Actuator = 1, Destructive = 2 };
#define SDP_NONE SdpDanger::None
#define SDP_ACTUATOR SdpDanger::Actuator
#define SDP_DESTRUCTIVE SdpDanger::Destructive

// รหัส error ของสเปก — handler ใช้ SDP_BUSY / SDP_FAILED / SDP_INVALID_INPUT กับ call.fail()
enum SdpErrorCode : uint8_t {
  SDP_VERSION_MISMATCH = 1,
  SDP_UNKNOWN_CAPABILITY = 2,
  SDP_INVALID_INPUT = 3,
  SDP_BUSY = 4,
  SDP_FAILED = 5,
  SDP_FRAME_TOO_LARGE = 6,
  SDP_UNKNOWN_MESSAGE = 7,
  SDP_AUTH_REQUIRED = 8,
  SDP_FORBIDDEN_TRANSPORT = 9,
};

class SdpCall;
using SdpHandler = void (*)(SdpCall& call);

// descriptor เก็บใน flash ทั้งก้อน (ข้อความ schema เป็น JSON) — ช่องว่าง "" = ไม่มี
struct SdpCapability {
  const char* name;
  const char* description;
  const char* inputSchema;
  const char* outputSchema;
  SdpDanger danger;
  SdpHandler handler;
};

struct SdpEvent {
  const char* name;
  const char* description;
  const char* dataSchema;
};

namespace sdplib {
// descriptor อยู่ใน flash — AVR ต้องคัดลอกออกมาก่อนอ่าน field
inline SdpCapability loadCapability(const SdpCapability* p) {
  SdpCapability c;
  flashCopy(&c, p, sizeof c);
  return c;
}
inline SdpEvent loadEvent(const SdpEvent* p) {
  SdpEvent e;
  flashCopy(&e, p, sizeof e);
  return e;
}
inline bool hasText(const char* flashText) { return flashByte(flashText) != 0; }

// ชิ้นของข้อความ error ที่ต่อกันตอนส่ง — ไม่ต้องประกอบใน RAM
struct TextPart {
  const char* text;
  size_t len;
  bool inFlash;
};
}  // namespace sdplib

#define SDP_NO_SCHEMA ""

// ประกาศ capability + เริ่มเขียน handler ต่อท้ายได้เลย (ตัวแปรชื่อ call)
#define SDP_CAPABILITY(id, name, description, danger, inputSchema, outputSchema)          \
  void id##_run(SdpCall& call);                                                         \
  const char id##_name[] SDP_FLASH = name;                                              \
  const char id##_description[] SDP_FLASH = description;                                \
  const char id##_input[] SDP_FLASH = inputSchema;                                      \
  const char id##_output[] SDP_FLASH = outputSchema;                                    \
  const SdpCapability id SDP_FLASH = {id##_name,   id##_description, id##_input,        \
                                      id##_output, danger,           id##_run};         \
  void id##_run(SdpCall& call)

#define SDP_EVENT(id, name, description, dataSchema)       \
  const char id##_name[] SDP_FLASH = name;                \
  const char id##_description[] SDP_FLASH = description;  \
  const char id##_data[] SDP_FLASH = dataSchema;          \
  const SdpEvent id SDP_FLASH = {id##_name, id##_description, id##_data}

// ค่าที่ host ส่งมากับ call — ผ่านการตรวจชนิดและขอบเขตตาม input_schema แล้วก่อนถึง handler
class SdpInput {
 public:
  bool has(const char* key) const;
  bool getBool(const char* key, bool fallback = false) const;
  long getInt(const char* key, long fallback = 0) const;
  double getNumber(const char* key, double fallback = 0) const;
  // คัดลอกข้อความพร้อม '\0' (ตัดถ้ายาวเกิน) คืนความยาวเต็มของข้อความ — ไม่มี key = 0
  size_t getText(const char* key, char* out, size_t capacity) const;
  // ชี้เข้า buffer รับ ใช้ได้จนกว่า handler จะจบ — ไม่มี key = nullptr
  const uint8_t* getBytes(const char* key, size_t* len) const;
  sdplib::CborRef raw() const { return map_; }

 private:
  friend class SynaptaDevice;
  sdplib::CborRef value(const char* key) const;
  sdplib::CborRef map_;
};

// ตัวประกอบ output / data ของ event เป็น CBOR map ใน RAM (ขนาด SdpConfig::outputBytes)
// ใส่ key ตามลำดับ: สั้นก่อนยาว แล้วเรียงตามตัวอักษร — ผิดลำดับยังส่งได้แต่จะมีคำเตือนใน log
class SdpOutput {
 public:
  SdpOutput& add(const char* key, bool value);
  SdpOutput& add(const char* key, int value) { return add(key, static_cast<long>(value)); }
  SdpOutput& add(const char* key, long value);
  SdpOutput& add(const char* key, unsigned int value) { return add(key, static_cast<unsigned long>(value)); }
  SdpOutput& add(const char* key, unsigned long value);
  SdpOutput& add(const char* key, float value) { return add(key, static_cast<double>(value)); }
  SdpOutput& add(const char* key, double value);
  SdpOutput& add(const char* key, const char* text);
  SdpOutput& addBytes(const char* key, const uint8_t* data, size_t len);
  SdpOutput& beginObject(const char* key);
  SdpOutput& beginArray(const char* key);
  // สมาชิกของ array ที่เปิดอยู่
  SdpOutput& item(bool value);
  SdpOutput& item(int value) { return item(static_cast<long>(value)); }
  SdpOutput& item(long value);
  SdpOutput& item(double value);
  SdpOutput& item(const char* text);
  SdpOutput& end();

 private:
  friend class SynaptaDevice;
  struct Level {
    uint16_t headerPos;
    uint16_t count;
    const char* lastKey;
    bool isMap;
  };
  static constexpr uint8_t kMaxLevels = 4;  // map ชั้นนอก + ซ้อนได้อีก 3

  void reset(uint8_t* buffer, uint16_t capacity);
  bool finish();  // ปิดทุกชั้น คืน false ถ้าล้น buffer หรือซ้อนเกิน
  bool entry(const char* key);
  void open(const char* key, bool isMap);
  void head(uint8_t major, uint32_t arg);
  void put(const uint8_t* data, size_t len);

  uint8_t* buf_ = nullptr;
  uint16_t cap_ = 0;
  uint16_t len_ = 0;
  Level levels_[kMaxLevels];
  uint8_t depth_ = 0;
  bool overflow_ = false;
  bool misordered_ = false;
};

// หนึ่งคำสั่งที่กำลังทำ — handler อ่าน input() เขียน output() หรือ fail()
class SdpCall {
 public:
  const SdpInput& input() const { return input_; }
  SdpOutput& output() {
    hasOutput_ = true;
    return output_;
  }
  void fail(uint8_t code, const char* message);
  void fail(uint8_t code, const __FlashStringHelper* message);

 private:
  friend class SynaptaDevice;
  SdpCall(const SdpInput& input, SdpOutput& output) : input_(input), output_(output) {}

  const SdpInput& input_;
  SdpOutput& output_;
  bool hasOutput_ = false;
  uint8_t errorCode_ = 0;
  const char* errorMessage_ = nullptr;
  bool errorInFlash_ = false;
};

enum class SdpUsbPairing : uint8_t { Confirm = 1, Code = 2 };
enum class SdpBootState : uint8_t { Running = 0, Pending = 1, RolledBack = 2 };
// ใช้กับ Stream USB เท่านั้น callback ต้องตรวจช่วง enrollment และตรวจรหัสพร้อมจำกัดจำนวนครั้งก่อนบันทึก PSK
// psk มี 16 ไบต์, code มี 8 หลัก ASCII หรือ nullptr, คืน 0 เมื่อบันทึกสำเร็จ หรือ SdpErrorCode
using SdpUsbProvision = uint8_t (*)(const uint8_t* psk, const uint8_t* code);

struct SdpConfig {
  const char* model = "arduino";     // ชื่อรุ่นบอร์ด เช่น "esp32-devkit"
  const char* firmware = "unknown";  // ชื่อ + เวอร์ชัน firmware — host ใช้ตรวจว่าแฟลชสำเร็จ
#if defined(__AVR__)
  uint16_t maxFrame = 128;     // เฟรมยาวสุดที่รับได้ (ต่ำสุด 128)
  uint16_t outputBytes = 64;   // buffer ของ output / data ของ event
#else
  uint16_t maxFrame = 512;
  uint16_t outputBytes = 512;
#endif
  uint16_t heartbeatMs = 1000;         // host ส่ง ping ทุกเท่านี้ — ไม่ได้ยินเกิน 3 เท่า = link หลุด
  uint16_t describePageBytes = 4096;   // ขนาดหน้าของ describe (ขาอุปกรณ์ → host จำกัด 4096)
  uint8_t address = 0xFF;              // RS485 หลายอุปกรณ์: เลขของบอร์ดนี้ (0xFF = ไม่ใช้)
  const char* projectId = nullptr;    // ค่าใน build manifest; nullptr ทั้งคู่สำหรับแอป v1 เดิม
  const char* buildId = nullptr;
  SdpUsbPairing usbPairing = SdpUsbPairing::Confirm;
  SdpUsbProvision usbProvision = nullptr;
  SdpBootState bootState = SdpBootState::Running;
};

class SynaptaDevice {
 public:
  template <size_t NC, size_t NE>
  bool begin(Stream& port, const SdpConfig& config, const SdpCapability* const (&caps)[NC],
             const SdpEvent* const (&events)[NE]) {
    return begin(port, config, caps, NC, events, NE);
  }
  template <size_t NC>
  bool begin(Stream& port, const SdpConfig& config, const SdpCapability* const (&caps)[NC]) {
    return begin(port, config, caps, NC, nullptr, 0);
  }
  // คืน false ถ้า schema ผิดหรือจอง buffer ไม่ได้ — เหตุผลพิมพ์ทาง port และ host จะได้ error ตอน hello
  bool begin(Stream& port, const SdpConfig& config, const SdpCapability* const* caps, size_t capCount,
             const SdpEvent* const* events, size_t eventCount);

  // เรียกเมื่อไม่ได้ยิน host เกิน 3 × heartbeat_ms — ปิดเอาต์พุตทั้งหมดที่นี่ (ต้องเรียกก่อน begin)
  void onLinkLost(void (*handler)()) { linkLost_ = handler; }

  void loop();  // เรียกทุกรอบของ loop() — อ่าน serial, ตอบคำสั่ง, จับเวลา heartbeat

  bool linked() const { return linked_; }  // host คุยอยู่ (ยังไม่หลุด)
  uint32_t capsHash() const { return capsHash_; }
  void setBootState(SdpBootState state) { config_.bootState = state; }

  // ส่ง event: เขียน data ลง SdpOutput ที่ได้ แล้ว sendEvent() — ไม่ส่งถ้า host ไม่ได้ต่ออยู่
  // ห้ามเรียกระหว่าง handler ของ call (ใช้ buffer เดียวกับ output)
  SdpOutput& beginEvent(const SdpEvent& event);
  bool sendEvent();

 private:
  // describe.cpp — descriptor, caps_hash, การแบ่งหน้า, ตรวจ schema ตอน begin
  bool lintDescriptors();
  void writeDescriptor(sdplib::Sink& sink, size_t index) const;
  size_t descriptorSize(size_t index) const;
  size_t pageEnd(size_t start, size_t budget) const;
  void handleDescribe(uint16_t id, sdplib::CborRef m);
  void logProblem(size_t index, const char* field, const char* schema, const sdplib::Problem& problem);

  // SynaptaDevice.cpp — รับเฟรมและตอบข้อความ
  void handleFrame(const uint8_t* inner, uint16_t len);
  void handleMessage(const uint8_t* body, const uint8_t* end);
  void handleHello(uint16_t id, sdplib::CborRef m);
  void handleFirmwareInfo(uint16_t id);
  void handlePair(uint16_t id, sdplib::CborRef m);
  void handleCall(uint16_t id, sdplib::CborRef m);
  void sendCallError(uint16_t id, const SdpCall& call);
  sdplib::CborWriter startReply(uint8_t type, uint16_t id, size_t mapEntries);
  void finishReply() { frame_.end(); }
  void sendError(uint16_t id, uint8_t code);
  void sendError(uint16_t id, uint8_t code, const sdplib::TextPart* parts, uint8_t count);
  void sendError(uint16_t id, uint8_t code, const char* flashMessage);
  void logFlash(const char* text);

  Stream* port_ = nullptr;
  SdpConfig config_{};
  const SdpCapability* const* caps_ = nullptr;
  size_t capCount_ = 0;
  const SdpEvent* const* events_ = nullptr;
  size_t eventCount_ = 0;
  void (*linkLost_)() = nullptr;

  uint8_t* rxBuffer_ = nullptr;     // max_frame + 2 (CRC)
  uint8_t* blockBuffer_ = nullptr;  // บล็อก COBS ขาส่ง
  uint8_t* outBuffer_ = nullptr;    // output ของ call / data ของ event
  sdplib::FrameReceiver rx_;
  sdplib::FrameSink frame_;
  SdpOutput output_;
  bool inHandler_ = false;
  const SdpEvent* pendingEvent_ = nullptr;

  uint8_t deviceId_[8] = {0};
  uint8_t bootId_[4] = {0};
  uint32_t capsHash_ = 0;
  bool ready_ = false;  // schema ผ่านและจอง buffer ได้
  bool linked_ = false;
  uint32_t lastHeardMs_ = 0;
  bool replyAddr_ = false;  // คำขอมี addr → คำตอบใส่ addr ของเราด้วย
};

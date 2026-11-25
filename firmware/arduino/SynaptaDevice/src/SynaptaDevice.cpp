// รับเฟรมจาก serial แล้วตอบตามชนิดข้อความ (wire-format.md §ข้อความ) + จับเวลา heartbeat (safety.md)
#include "SynaptaDevice.h"

#include <stdlib.h>
#include <string.h>

#include "sdp/json.h"
#include "sdp/platform.h"

using sdplib::CborRef;
using sdplib::CborWriter;
using sdplib::TextPart;

namespace {

constexpr uint8_t kFlagEncrypted = 0x01;
constexpr uint8_t kFlagAddr = 0x02;
constexpr uint8_t kSdpVersion = 1;  // ช่วงที่รองรับ: v1 ถึง v1
constexpr uint16_t kMinMaxFrame = 128;

enum MessageType : uint8_t {
  kHello = 1,
  kHelloAck = 2,
  kDescribe = 3,
  kCall = 5,
  kResult = 6,
  kError = 7,
  kEvent = 8,
  kPing = 9,
  kPong = 10,
  kPair = 11,
  kUnpair = 13,
};

const uint8_t kEmptyMap[] = {0xA0};

const char kNoBuffers[] SDP_FLASH = "SDP: not enough RAM for buffers - lower maxFrame or outputBytes\n";
const char kNoPersistentId[] SDP_FLASH = "SDP: this board cannot store device_id - it changes every boot\n";
const char kLinkLost[] SDP_FLASH = "SDP: link lost - onLinkLost() called\n";
const char kKeyOrder[] SDP_FLASH = "SDP: output keys out of order - add shorter keys first, then alphabetical\n";
const char kEventTooLarge[] SDP_FLASH = "SDP: event data larger than outputBytes - not sent\n";

const char kNotReady[] SDP_FLASH = "firmware descriptor error - see device serial log";
const char kNoCapability[] SDP_FLASH = "no capability '";
const char kQuote[] SDP_FLASH = "'";
const char kQuoteSpace[] SDP_FLASH = "' ";
const char kInputPrefix[] SDP_FLASH = "input ";
const char kTakesNoInput[] SDP_FLASH = "this capability takes no input";
const char kNeedsLinkLost[] SDP_FLASH = "firmware must set onLinkLost() before exposing actuator capabilities";
const char kOutputTooLarge[] SDP_FLASH = "output larger than outputBytes in SdpConfig";
const char kNoPairing[] SDP_FLASH = "pairing is not supported by this firmware (no wireless link)";
const char kEncrypted[] SDP_FLASH = "encrypted frames are not supported on this link";

bool flashEqualsRam(const char* flashText, const uint8_t* ram, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (sdplib::flashByte(flashText + i) != ram[i]) return false;
  }
  return sdplib::flashByte(flashText + len) == 0;
}

}  // namespace

bool SynaptaDevice::begin(Stream& port, const SdpConfig& config, const SdpCapability* const* caps, size_t capCount,
                          const SdpEvent* const* events, size_t eventCount) {
  port_ = &port;
  config_ = config;
  if (config_.maxFrame < kMinMaxFrame) config_.maxFrame = kMinMaxFrame;
  caps_ = caps;
  capCount_ = capCount;
  events_ = events;
  eventCount_ = eventCount;

  // จองครั้งเดียวตอนเริ่ม ไม่คืน — heap ไม่แตกเป็นชิ้นระหว่างทำงาน
  free(rxBuffer_);
  free(blockBuffer_);
  free(outBuffer_);
  rxBuffer_ = static_cast<uint8_t*>(malloc(config_.maxFrame + 2u));
  blockBuffer_ = static_cast<uint8_t*>(malloc(sdplib::kCobsBlock));
  outBuffer_ = static_cast<uint8_t*>(malloc(config_.outputBytes));
  if (!rxBuffer_ || !blockBuffer_ || !outBuffer_) {
    logFlash(kNoBuffers);
    ready_ = false;
    return false;
  }
  rx_ = sdplib::FrameReceiver(rxBuffer_, config_.maxFrame);
  frame_ = sdplib::FrameSink(blockBuffer_);

  if (!sdplib::loadDeviceId(deviceId_)) logFlash(kNoPersistentId);
  sdplib::randomBytes(bootId_, sizeof bootId_);

  ready_ = lintDescriptors();
  if (ready_) {  // schema ผิดรูปเขียนเป็น CBOR ไม่ได้ — ไม่ ready ก็ไม่มีใครได้ describe อยู่แล้ว
    sdplib::CountSink all;
    for (size_t i = 0; i < capCount_ + eventCount_; i++) writeDescriptor(all, i);
    capsHash_ = ~all.crc;
  }
  lastHeardMs_ = millis();
  return ready_;
}

void SynaptaDevice::loop() {
  if (!port_ || !rxBuffer_) return;
  while (port_->available() > 0) {
    const sdplib::RxResult r = rx_.push(static_cast<uint8_t>(port_->read()));
    if (r == sdplib::RxResult::kFrame) {
      handleFrame(rx_.frame(), rx_.frameLen());
    } else if (r == sdplib::RxResult::kTooLarge) {
      replyAddr_ = false;
      sendError(0, SDP_FRAME_TOO_LARGE);  // อ่าน id ของเฟรมที่ทิ้งไม่ได้ → id 0
    }
  }
  if (linked_ && millis() - lastHeardMs_ > 3UL * config_.heartbeatMs) {
    linked_ = false;
    if (linkLost_) linkLost_();
    logFlash(kLinkLost);
  }
}

void SynaptaDevice::handleFrame(const uint8_t* inner, uint16_t len) {
  const uint8_t flags = inner[0];
  const uint8_t headerLen = (flags & kFlagAddr) ? 2 : 1;
  if (len < headerLen) return;
  if (flags & kFlagAddr) {
    if (config_.address == 0xFF || inner[1] != config_.address) return;  // ของอุปกรณ์อื่นบนสายเดียวกัน
  }
  replyAddr_ = (flags & kFlagAddr) != 0;
  lastHeardMs_ = millis();
  linked_ = true;
  if (flags & kFlagEncrypted) {
    sendError(0, SDP_INVALID_INPUT, kEncrypted);
    return;
  }
  handleMessage(inner + headerLen, inner + len);
}

void SynaptaDevice::handleMessage(const uint8_t* body, const uint8_t* end) {
  // หัว [t, id, ...] — อ่าน id ไม่ได้ ตอบด้วย id 0 (wire-format.md: id ของคำตอบ error)
  uint8_t major;
  uint32_t arg;
  const uint8_t* p = body;
  if (!sdplib::readHead(p, end, &major, &arg, &p) || major != sdplib::kArray || arg != 3) return sendError(0, SDP_INVALID_INPUT);
  if (!sdplib::readHead(p, end, &major, &arg, &p) || major != sdplib::kUint) return sendError(0, SDP_INVALID_INPUT);
  const uint32_t type = arg;
  if (!sdplib::readHead(p, end, &major, &arg, &p) || major != sdplib::kUint || arg > 0xFFFF) {
    return sendError(0, SDP_INVALID_INPUT);
  }
  const uint16_t id = static_cast<uint16_t>(arg);

  const uint8_t* next;
  if (!sdplib::wellFormed(p, end, &next) || next != end) return sendError(id, SDP_INVALID_INPUT);
  const CborRef m{p};
  if (!m.isMap()) return sendError(id, SDP_INVALID_INPUT);

  if (!ready_) return sendError(id, SDP_FAILED, kNotReady);
  switch (type) {
    case kHello:
      return handleHello(id, m);
    case kDescribe:
      return handleDescribe(id, m);
    case kCall:
      return handleCall(id, m);
    case kPing: {
      CborWriter w = startReply(kPong, id, 2);
      w.uint(0);
      w.uint(millis());
      w.uint(1);
      w.bytes(bootId_, sizeof bootId_);
      return finishReply();
    }
    case kPair:
    case kUnpair:
      return sendError(id, SDP_FAILED, kNoPairing);
    default:
      return sendError(id, SDP_UNKNOWN_MESSAGE);
  }
}

void SynaptaDevice::handleHello(uint16_t id, CborRef m) {
  const CborRef min = m.mapGetUint(0);
  const CborRef max = m.mapGetUint(1);
  if (!min.valid() || !max.valid() || min.major() != sdplib::kUint || max.major() != sdplib::kUint) {
    return sendError(id, SDP_INVALID_INPUT);
  }
  if (min.arg() > kSdpVersion || max.arg() < kSdpVersion || min.arg() > max.arg()) {
    CborWriter w = startReply(kError, id, 2);
    w.uint(0);
    w.uint(SDP_VERSION_MISMATCH);
    w.uint(2);  // detail = ช่วงของอุปกรณ์
    w.map(2);
    w.uint(0);
    w.uint(kSdpVersion);
    w.uint(1);
    w.uint(kSdpVersion);
    return finishReply();
  }
  CborWriter w = startReply(kHelloAck, id, 11);
  w.uint(0);
  w.uint(kSdpVersion);
  w.uint(1);
  w.uint(kSdpVersion);
  w.uint(2);
  w.uint(kSdpVersion);
  w.uint(3);
  w.bytes(deviceId_, sizeof deviceId_);
  w.uint(4);
  w.text(config_.model);
  w.uint(5);
  w.text(config_.firmware);
  w.uint(6);
  w.uint(config_.maxFrame);
  w.uint(7);
  w.uint(capsHash_);
  w.uint(8);
  w.uint(config_.heartbeatMs);
  w.uint(9);
  w.bytes(bootId_, sizeof bootId_);
  w.uint(10);
  w.boolean(false);  // ยังไม่มีลิงก์ไร้สาย จึงไม่มีการจับคู่
  finishReply();
}

void SynaptaDevice::handleCall(uint16_t id, CborRef m) {
  const CborRef name = m.mapGetUint(0);
  if (!name.valid() || !name.isText()) return sendError(id, SDP_INVALID_INPUT);
  const size_t nameLen = static_cast<size_t>(name.arg());

  size_t index = 0;
  SdpCapability cap{};
  for (; index < capCount_; index++) {
    cap = sdplib::loadCapability(caps_[index]);
    if (flashEqualsRam(cap.name, name.payload(), nameLen)) break;
  }
  if (index == capCount_) {
    const TextPart parts[] = {{kNoCapability, sdplib::flashLen(kNoCapability), true},
                              {reinterpret_cast<const char*>(name.payload()), nameLen, false},
                              {kQuote, 1, true}};
    return sendError(id, SDP_UNKNOWN_CAPABILITY, parts, 3);
  }
  if (cap.danger != SdpDanger::None && !linkLost_) return sendError(id, SDP_FAILED, kNeedsLinkLost);

  const CborRef given = m.mapGetUint(1);
  const CborRef input = given.valid() ? given : CborRef{kEmptyMap};
  if (!sdplib::hasText(cap.inputSchema)) {
    if (!input.isMap() || input.arg() != 0) return sendError(id, SDP_INVALID_INPUT, kTakesNoInput);
  } else {
    sdplib::Problem problem;
    if (!sdplib::validateInput(cap.inputSchema, input, &problem)) {
      // "'on' must be boolean" — ชื่อ field อ่านตรงจาก schema ใน flash
      if (problem.where) {
        const TextPart parts[] = {{kQuote, 1, true},
                                  {problem.where + 1, sdplib::json::rawLen(problem.where), true},
                                  {kQuoteSpace, 2, true},
                                  {problem.message, sdplib::flashLen(problem.message), true}};
        return sendError(id, SDP_INVALID_INPUT, parts, 4);
      }
      const TextPart parts[] = {{kInputPrefix, 6, true}, {problem.message, sdplib::flashLen(problem.message), true}};
      return sendError(id, SDP_INVALID_INPUT, parts, 2);
    }
  }

  SdpInput in;
  in.map_ = input;
  output_.reset(outBuffer_, config_.outputBytes);
  SdpCall call(in, output_);
  inHandler_ = true;
  cap.handler(call);
  inHandler_ = false;

  if (call.errorCode_) return sendCallError(id, call);
  if (!call.hasOutput_) {
    startReply(kResult, id, 0);
    return finishReply();
  }
  if (!output_.finish()) return sendError(id, SDP_FAILED, kOutputTooLarge);
  if (output_.misordered_) logFlash(kKeyOrder);
  CborWriter w = startReply(kResult, id, 1);
  w.uint(0);
  w.raw(outBuffer_, output_.len_);
  finishReply();
}

void SynaptaDevice::sendCallError(uint16_t id, const SdpCall& call) {
  if (!call.errorMessage_) return sendError(id, call.errorCode_);
  const size_t len = call.errorInFlash_ ? sdplib::flashLen(call.errorMessage_) : strlen(call.errorMessage_);
  const TextPart part = {call.errorMessage_, len, call.errorInFlash_};
  sendError(id, call.errorCode_, &part, 1);
}

SdpOutput& SynaptaDevice::beginEvent(const SdpEvent& event) {
  static SdpOutput discarded;  // ใช้ตอนเรียกผิดที่ — เขียนอะไรลงไปก็ล้นทิ้ง
  pendingEvent_ = nullptr;
  if (inHandler_ || !outBuffer_) {
    discarded.reset(nullptr, 0);
    return discarded;
  }
  for (size_t i = 0; i < eventCount_; i++) {
    if (events_[i] == &event) pendingEvent_ = &event;
  }
  output_.reset(outBuffer_, config_.outputBytes);
  return output_;
}

bool SynaptaDevice::sendEvent() {
  const SdpEvent* event = pendingEvent_;
  pendingEvent_ = nullptr;
  if (!event || !ready_) return false;
  if (!output_.finish()) {
    logFlash(kEventTooLarge);
    return false;
  }
  if (!linked_) return false;  // ไม่มีใครฟัง — ไม่ส่งให้ TX buffer ของ Uno เต็มแล้วบล็อก loop
  if (output_.misordered_) logFlash(kKeyOrder);
  const bool hasData = output_.levels_[0].count > 0;
  replyAddr_ = config_.address != 0xFF;  // บนสาย RS485 host ต้องรู้ว่า event มาจากใคร
  CborWriter w = startReply(kEvent, 0, hasData ? 2 : 1);
  w.uint(0);
  w.flashText(sdplib::loadEvent(event).name);
  if (hasData) {
    w.uint(1);
    w.raw(outBuffer_, output_.len_);
  }
  finishReply();
  return true;
}

CborWriter SynaptaDevice::startReply(uint8_t type, uint16_t id, size_t mapEntries) {
  frame_.begin(*port_);
  frame_.put(replyAddr_ ? kFlagAddr : 0);
  if (replyAddr_) frame_.put(config_.address);
  CborWriter w(frame_);
  w.array(3);
  w.uint(type);
  w.uint(id);
  w.map(mapEntries);
  return w;
}

void SynaptaDevice::sendError(uint16_t id, uint8_t code) {
  CborWriter w = startReply(kError, id, 1);
  w.uint(0);
  w.uint(code);
  finishReply();
}

void SynaptaDevice::sendError(uint16_t id, uint8_t code, const TextPart* parts, uint8_t count) {
  size_t total = 0;
  for (uint8_t i = 0; i < count; i++) total += parts[i].len;
  CborWriter w = startReply(kError, id, 2);
  w.uint(0);
  w.uint(code);
  w.uint(1);
  w.head(sdplib::kText, static_cast<uint32_t>(total));
  for (uint8_t i = 0; i < count; i++) {
    for (size_t j = 0; j < parts[i].len; j++) {
      frame_.put(parts[i].inFlash ? sdplib::flashByte(parts[i].text + j) : static_cast<uint8_t>(parts[i].text[j]));
    }
  }
  finishReply();
}

void SynaptaDevice::sendError(uint16_t id, uint8_t code, const char* flashMessage) {
  const TextPart part = {flashMessage, sdplib::flashLen(flashMessage), true};
  sendError(id, code, &part, 1);
}

void SynaptaDevice::logFlash(const char* text) {
  if (!port_) return;
  for (size_t i = 0; sdplib::flashByte(text + i) != 0; i++) port_->write(sdplib::flashByte(text + i));
}

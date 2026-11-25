// รับเฟรม serial ทีละ byte: ถอด COBS ลง buffer ขนาด max_frame + 2 แล้วตรวจตามลำดับใน wire-format.md
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sdplib {

enum class RxResult : uint8_t {
  kNone,          // ยังไม่จบท่อน หรือท่อนว่าง
  kFrame,         // ได้เฟรมดี: frame() / frameLen() = flags | [addr] | body
  kTooLarge,      // อุปกรณ์ต้องตอบ error 6
  kCobsInvalid,
  kTooShort,
  kCrcMismatch,
  kReservedFlag,
};

class FrameReceiver {
 public:
  // buffer ต้องยาว maxFrame + 2 (เผื่อ CRC)
  FrameReceiver(uint8_t* buffer = nullptr, uint16_t maxFrame = 0)
      : buf_(buffer), limit_(static_cast<uint16_t>(maxFrame + 2)) {}

  RxResult push(uint8_t byte);
  const uint8_t* frame() const { return buf_; }
  uint16_t frameLen() const { return frameLen_; }
  uint16_t pendingLen() const { return seen_; }  // byte ของท่อนปัจจุบันที่ยังไม่เจอ 0x00

 private:
  void append(uint8_t byte);
  RxResult endChunk();

  uint8_t* buf_;
  uint16_t limit_;
  uint16_t len_ = 0;
  uint16_t frameLen_ = 0;
  uint16_t seen_ = 0;
  uint8_t remaining_ = 0;
  bool zeroPending_ = false;
  bool overflow_ = false;
};

}  // namespace sdplib

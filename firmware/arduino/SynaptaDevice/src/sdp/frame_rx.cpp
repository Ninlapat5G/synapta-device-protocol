#include "frame_rx.h"

#include "crc.h"

namespace sdplib {

RxResult FrameReceiver::push(uint8_t byte) {
  if (byte == 0) return endChunk();
  if (seen_ < 0xFFFF) seen_++;
  if (overflow_) return RxResult::kNone;  // เกินแล้ว ทิ้งจนถึง 0x00
  if (remaining_ == 0) {                   // byte นี้คือรหัสของบล็อกใหม่
    if (zeroPending_) append(0);
    remaining_ = static_cast<uint8_t>(byte - 1);
    zeroPending_ = byte != 0xFF;
  } else {
    append(byte);
    remaining_--;
  }
  return RxResult::kNone;
}

void FrameReceiver::append(uint8_t byte) {
  if (len_ >= limit_) {
    overflow_ = true;
    return;
  }
  buf_[len_++] = byte;
}

RxResult FrameReceiver::endChunk() {
  const bool empty = seen_ == 0;
  const bool overflow = overflow_;
  const bool complete = remaining_ == 0;
  seen_ = 0;
  remaining_ = 0;
  zeroPending_ = false;
  overflow_ = false;
  const uint16_t len = len_;
  len_ = 0;

  if (empty) return RxResult::kNone;
  if (overflow) return RxResult::kTooLarge;
  if (!complete) return RxResult::kCobsInvalid;
  if (len < 3) return RxResult::kTooShort;
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len - 2; i++) crc = crc16Update(crc, buf_[i]);
  if (buf_[len - 2] != static_cast<uint8_t>(crc >> 8) || buf_[len - 1] != static_cast<uint8_t>(crc)) {
    return RxResult::kCrcMismatch;
  }
  if (buf_[0] & 0xFC) return RxResult::kReservedFlag;
  frameLen_ = static_cast<uint16_t>(len - 2);  // buffer ยังอยู่จนกว่าจะ push byte ถัดไป
  return RxResult::kFrame;
}

}  // namespace sdplib

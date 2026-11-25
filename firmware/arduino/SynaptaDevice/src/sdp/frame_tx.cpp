#include "frame_tx.h"

#include "crc.h"

namespace sdplib {

void FrameSink::begin(Print& out) {
  out_ = &out;
  used_ = 0;
  afterFullBlock_ = false;
  crc_ = 0xFFFF;
  out_->write(static_cast<uint8_t>(0));  // ตัดข้อความ log ที่พิมพ์ค้างไว้ออกจากเฟรมนี้
}

void FrameSink::put(uint8_t byte) {
  crc_ = crc16Update(crc_, byte);
  cobsPut(byte);
}

void FrameSink::end() {
  const uint16_t crc = crc_;
  cobsPut(static_cast<uint8_t>(crc >> 8));
  cobsPut(static_cast<uint8_t>(crc));
  // บล็อกเต็มที่จบพอดีท้ายข้อมูล ไม่ต้องมีรหัสปิดท้าย (ตรงกับ cobs_encode ใน refimpl.py)
  if (!(afterFullBlock_ && used_ == 0)) flush(static_cast<uint8_t>(used_ + 1));
  out_->write(static_cast<uint8_t>(0));
}

void FrameSink::cobsPut(uint8_t byte) {
  afterFullBlock_ = false;
  if (byte == 0) {
    flush(static_cast<uint8_t>(used_ + 1));
    return;
  }
  block_[used_++] = byte;
  if (used_ == kCobsBlock) {
    flush(0xFF);
    afterFullBlock_ = true;
  }
}

void FrameSink::flush(uint8_t code) {
  out_->write(code);
  out_->write(block_, used_);
  used_ = 0;
}

}  // namespace sdplib

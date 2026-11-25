//! ห่อและแกะเฟรมบนสาย: serial (COBS + CRC-16) และ TCP (ความยาว 2 ไบต์)
//!
//! "inner" ในไฟล์นี้หมายถึง `flags | [addr] | body` ตาม wire-format.md §ชั้นของหนึ่งเฟรม

use alloc::vec::Vec;

use crate::cobs;
use crate::crc::crc16_ccitt_false;

pub const FLAG_ENCRYPTED: u8 = 0x01;
pub const FLAG_ADDR: u8 = 0x02;
const FLAGS_RESERVED: u8 = 0xFC;

/// `0x00 COBS(inner | CRC) 0x00` — 0x00 นำหน้ากัน log ที่พิมพ์ก่อนหน้าติดมากับเฟรม
pub fn encode_serial(inner: &[u8]) -> Vec<u8> {
    let mut with_crc = Vec::with_capacity(inner.len() + 2);
    with_crc.extend_from_slice(inner);
    with_crc.extend_from_slice(&crc16_ccitt_false(inner).to_be_bytes());

    let mut out = Vec::with_capacity(with_crc.len() + with_crc.len() / 254 + 3);
    out.push(0);
    out.extend_from_slice(&cobs::encode(&with_crc));
    out.push(0);
    out
}

/// `len(2, big-endian) | inner` — ไม่มี CRC เพราะ TCP ตรวจให้แล้ว
pub fn encode_tcp(inner: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(inner.len() + 2);
    out.extend_from_slice(&(inner.len() as u16).to_be_bytes());
    out.extend_from_slice(inner);
    out
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DropReason {
    TooLarge,
    CobsInvalid,
    TooShort,
    CrcMismatch,
    ReservedFlag,
}

impl DropReason {
    pub fn as_str(self) -> &'static str {
        match self {
            DropReason::TooLarge => "too_large",
            DropReason::CobsInvalid => "cobs_invalid",
            DropReason::TooShort => "too_short",
            DropReason::CrcMismatch => "crc_mismatch",
            DropReason::ReservedFlag => "reserved_flag",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SerialEvent {
    /// inner ที่ผ่านทุกด่าน
    Frame(Vec<u8>),
    /// ท่อนที่ทิ้ง — `raw` คือ byte ก่อนถอด COBS (มักเป็น log ของบอร์ด) ว่างเมื่อ `TooLarge`
    Drop { reason: DropReason, raw: Vec<u8> },
}

/// ผู้รับ serial ที่ป้อนทีละ byte และถอด COBS ไปพร้อมกัน ลำดับการตรวจตาม wire-format.md
pub struct SerialReceiver {
    limit: usize,
    decoded: Vec<u8>,
    raw: Vec<u8>,
    raw_len: usize,
    block_left: u8,
    zero_pending: bool,
    overflow: bool,
}

impl SerialReceiver {
    /// `max_inner` = ขีดของผู้รับ: `max_frame` ของอุปกรณ์ หรือ `HOST_RX_LIMIT` ฝั่ง host
    pub fn new(max_inner: usize) -> Self {
        SerialReceiver {
            limit: max_inner + 2,
            decoded: Vec::new(),
            raw: Vec::new(),
            raw_len: 0,
            block_left: 0,
            zero_pending: false,
            overflow: false,
        }
    }

    /// จำนวน byte ที่รับมาแล้วแต่ยังไม่เจอ 0x00 ปิดท่อน
    pub fn pending_len(&self) -> usize {
        self.raw_len
    }

    pub fn push(&mut self, byte: u8) -> Option<SerialEvent> {
        if byte == 0 {
            return self.end_chunk();
        }
        self.raw_len += 1;
        if self.overflow {
            return None;
        }
        self.raw.push(byte);

        if self.block_left == 0 {
            // byte นี้คือรหัสของบล็อกใหม่ — 0x00 ของบล็อกก่อนใส่ตอนนี้ เพราะบล็อกสุดท้ายไม่มี 0x00 ตาม
            if self.zero_pending {
                self.append(0);
            }
            self.block_left = byte - 1;
            self.zero_pending = byte != 0xFF;
        } else {
            self.append(byte);
            self.block_left -= 1;
        }
        None
    }

    fn append(&mut self, byte: u8) {
        if self.decoded.len() == self.limit {
            self.overflow = true;
            self.raw.clear();
            return;
        }
        self.decoded.push(byte);
    }

    fn end_chunk(&mut self) -> Option<SerialEvent> {
        let empty = self.raw_len == 0;
        let overflow = self.overflow;
        let complete = self.block_left == 0;
        let raw = core::mem::take(&mut self.raw);
        let data = core::mem::take(&mut self.decoded);
        self.raw_len = 0;
        self.block_left = 0;
        self.zero_pending = false;
        self.overflow = false;

        if empty {
            return None;
        }
        match check_chunk(overflow, complete, &data) {
            Ok(()) => Some(SerialEvent::Frame(data[..data.len() - 2].to_vec())),
            Err(reason) => Some(SerialEvent::Drop { reason, raw }),
        }
    }
}

/// ด่านตรวจของท่อนที่ถอดเสร็จ เรียงตามตารางใน wire-format.md
fn check_chunk(overflow: bool, complete: bool, data: &[u8]) -> Result<(), DropReason> {
    if overflow {
        return Err(DropReason::TooLarge);
    }
    if !complete {
        return Err(DropReason::CobsInvalid);
    }
    if data.len() < 3 {
        return Err(DropReason::TooShort);
    }
    let (inner, crc) = data.split_at(data.len() - 2);
    if crc16_ccitt_false(inner).to_be_bytes() != crc {
        return Err(DropReason::CrcMismatch);
    }
    if inner[0] & FLAGS_RESERVED != 0 {
        return Err(DropReason::ReservedFlag);
    }
    Ok(())
}

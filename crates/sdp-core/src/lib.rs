//! SDP v1 ส่วนที่ไม่มี I/O: CBOR, เฟรม, ข้อความ และการเข้ารหัสลิงก์
//!
//! สเปกอยู่ที่ `docs/wire-format.md` และต้องผ่าน `spec/vectors/` ทุกตัว
//! ไม่มี I/O และเป็น `no_std` (ใช้ `alloc`) เพื่อให้ firmware ที่เขียนด้วย Rust ใช้ได้ในอนาคต

#![no_std]
#![forbid(unsafe_code)]

extern crate alloc;

pub mod cbor;
pub mod cobs;
pub mod crc;
#[cfg(feature = "crypto")]
pub mod crypto;
pub mod descriptor;
pub mod frame;
pub mod message;

pub use cbor::Value;
pub use message::{Envelope, Message, MessageError};

/// ขาอุปกรณ์ → host ยาวได้ถึงเท่านี้ (นับ `flags | [addr] | body`) ไม่ขึ้นกับ `max_frame` ของอุปกรณ์
pub const HOST_RX_LIMIT: usize = 4096;

/// `max_frame` ต่ำสุดที่อุปกรณ์ต้องรับได้
pub const MIN_MAX_FRAME: usize = 128;

//! อุปกรณ์ SDP จำลองบน TCP — ทดสอบ host (sdp-host, Connect, SynaptaCode) ได้โดยไม่ต้องมีบอร์ด
//!
//! ตอบเหมือน `examples/LedDemo` ของไลบรารี Arduino: `led.set`, `temp.read`, event `button.pressed`

pub mod device;
pub mod server;

pub use server::{serve, Notice, Options};

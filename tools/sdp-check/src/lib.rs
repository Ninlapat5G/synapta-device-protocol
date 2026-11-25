//! sdp-check: ตรวจว่าบอร์ด (หรือ sdp-sim) พูด SDP ถูกครบลำดับของเฟส 0
//!
//! hello → describe → call → event → ping หาย → on_link_lost

pub mod conn;
pub mod link;
pub mod script;
pub mod show;

pub use conn::Connection;
pub use script::{run, CheckOptions, Line, PRESS_PROMPT};

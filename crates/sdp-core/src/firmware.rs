//! Firmware identity and host-side upload verification for SDP v1.

use alloc::string::String;

use crate::message::HelloAck;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PairingMode {
    Disabled = 0,
    UsbConfirm = 1,
    UsbCode = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum BootState {
    Running = 0,
    Pending = 1,
    RolledBack = 2,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FirmwareInfo {
    pub project_id: String,
    pub build_id: String,
    pub pairing_mode: PairingMode,
    pub boot_state: BootState,
}

pub(crate) fn valid_source_id(id: &str) -> bool {
    fn alphanumeric(byte: u8) -> bool { byte.is_ascii_lowercase() || byte.is_ascii_digit() }

    let bytes = id.as_bytes();
    bytes.len() <= 96
        && bytes.first().is_some_and(|&byte| alphanumeric(byte))
        && bytes.iter().all(|&byte| alphanumeric(byte) || byte == b'-' || byte == b'_')
        && !matches!(id, "con" | "prn" | "aux" | "nul")
        && !(bytes.len() == 4 && (id.starts_with("com") || id.starts_with("lpt")) && bytes[3].is_ascii_digit())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UploadOutcome {
    Verified,
    Failed,
    RolledBack,
    Unknown,
}

/// Identity captured before upload. Call with a fresh, authenticated handshake
/// on Wi-Fi, or the same USB device, after it restarts.
pub struct UploadExpectation<'a> {
    pub previous_hello: &'a HelloAck,
    pub previous_firmware: &'a FirmwareInfo,
    pub project_id: &'a str,
    pub build_id: &'a str,
}

impl UploadExpectation<'_> {
    pub fn verify(&self, hello: &HelloAck, firmware: &FirmwareInfo) -> UploadOutcome {
        if hello.device_id != self.previous_hello.device_id || hello.boot_id == self.previous_hello.boot_id {
            return UploadOutcome::Unknown;
        }
        match firmware.boot_state {
            BootState::Running if firmware.project_id == self.project_id && firmware.build_id == self.build_id => UploadOutcome::Verified,
            BootState::RolledBack if firmware.project_id == self.previous_firmware.project_id
                && firmware.build_id == self.previous_firmware.build_id => UploadOutcome::RolledBack,
            _ => UploadOutcome::Unknown,
        }
    }
}

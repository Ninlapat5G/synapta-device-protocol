//! ข้อความ SDP: แปลงระหว่าง body (`[t, id, m]` แบบ CBOR) กับชนิดข้อมูลของ Rust
//!
//! ตาราง `t` และ key ของ `m` ตาม wire-format.md §ข้อความ — แก้ที่นี่ต้องแก้สเปกและ test vectors พร้อมกัน

use alloc::string::String;
use alloc::vec::Vec;

use crate::cbor::{DecodeError, Reader, Value};
use crate::descriptor::{Capability, EventInfo};
use crate::firmware::{valid_source_id, BootState, FirmwareInfo, PairingMode};

/// รหัส error บนสาย (wire-format.md §Error code)
pub mod code {
    pub const VERSION_MISMATCH: u16 = 1;
    pub const UNKNOWN_CAPABILITY: u16 = 2;
    pub const INVALID_INPUT: u16 = 3;
    pub const BUSY: u16 = 4;
    pub const FAILED: u16 = 5;
    pub const FRAME_TOO_LARGE: u16 = 6;
    pub const UNKNOWN_MESSAGE: u16 = 7;
    pub const AUTH_REQUIRED: u16 = 8;
    pub const FORBIDDEN_TRANSPORT: u16 = 9;
}

#[derive(Debug, Clone, PartialEq)]
pub struct Envelope {
    pub id: u16,
    pub msg: Message,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Hello {
    pub sdp_min: u16,
    pub sdp_max: u16,
    pub host: String,
    pub host_nonce: [u8; 8],
}

#[derive(Debug, Clone, PartialEq)]
pub struct HelloAck {
    pub sdp_min: u16,
    pub sdp_max: u16,
    pub version: u16,
    pub device_id: [u8; 8],
    pub model: String,
    pub fw: String,
    pub max_frame: u32,
    pub caps_hash: u32,
    pub heartbeat_ms: u32,
    pub boot_id: [u8; 4],
    /// `Some` เมื่อจับคู่แล้ว
    pub pairing: Option<PairProof>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PairProof {
    pub dev_nonce: [u8; 8],
    pub key_proof: [u8; 8],
}

#[derive(Debug, Clone, PartialEq)]
pub struct DescribeR {
    pub caps: Vec<Capability>,
    pub events: Vec<EventInfo>,
    pub next_page: Option<u32>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ErrorMsg {
    pub code: u16,
    pub msg: Option<String>,
    pub detail: Option<Value>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Message {
    Hello(Hello),
    HelloAck(HelloAck),
    Describe { page: u32 },
    DescribeR(DescribeR),
    Call { name: String, input: Option<Value> },
    Result { output: Option<Value> },
    Error(ErrorMsg),
    Event { name: String, data: Option<Value> },
    Ping,
    Pong { uptime_ms: u64, boot_id: [u8; 4] },
    Pair { psk: [u8; 16] },
    PairWithCode { psk: [u8; 16], code: [u8; 8] },
    PairAck,
    Unpair,
    FirmwareInfo,
    FirmwareInfoR(FirmwareInfo),
}

/// ถอดไม่ได้ — อุปกรณ์ตอบ `error` ด้วย `code()` และ `reply_id()`
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageError {
    /// body ผิดรูป หรือ field ที่ต้องมีหาย/ผิดชนิด — `reply_id` = 0 เมื่ออ่าน id ไม่ได้
    Invalid { reply_id: u16 },
    /// body ถูกรูปแต่ไม่รู้จัก `t` นี้
    UnknownMessage { id: u16 },
}

impl MessageError {
    pub fn code(self) -> u16 {
        match self {
            MessageError::Invalid { .. } => code::INVALID_INPUT,
            MessageError::UnknownMessage { .. } => code::UNKNOWN_MESSAGE,
        }
    }

    pub fn reply_id(self) -> u16 {
        match self {
            MessageError::Invalid { reply_id } => reply_id,
            MessageError::UnknownMessage { id } => id,
        }
    }
}

impl Message {
    pub fn type_code(&self) -> u64 {
        match self {
            Message::Hello(_) => 1,
            Message::HelloAck(_) => 2,
            Message::Describe { .. } => 3,
            Message::DescribeR(_) => 4,
            Message::Call { .. } => 5,
            Message::Result { .. } => 6,
            Message::Error(_) => 7,
            Message::Event { .. } => 8,
            Message::Ping => 9,
            Message::Pong { .. } => 10,
            Message::Pair { .. } | Message::PairWithCode { .. } => 11,
            Message::PairAck => 12,
            Message::Unpair => 13,
            Message::FirmwareInfo => 14,
            Message::FirmwareInfoR(_) => 15,
        }
    }

    fn to_map(&self) -> Vec<(Value, Value)> {
        let mut m = MapBuilder::default();
        match self {
            Message::Hello(h) => {
                m.uint(0, h.sdp_min).uint(1, h.sdp_max).text(2, &h.host).bytes(3, &h.host_nonce);
            }
            Message::HelloAck(a) => {
                m.uint(0, a.sdp_min).uint(1, a.sdp_max).uint(2, a.version).bytes(3, &a.device_id);
                m.text(4, &a.model).text(5, &a.fw).uint(6, a.max_frame).uint(7, a.caps_hash);
                m.uint(8, a.heartbeat_ms).bytes(9, &a.boot_id).put(10, Value::Bool(a.pairing.is_some()));
                if let Some(p) = &a.pairing {
                    m.bytes(11, &p.dev_nonce).bytes(12, &p.key_proof);
                }
            }
            Message::Describe { page } => {
                m.uint(0, *page);
            }
            Message::DescribeR(d) => {
                if !d.caps.is_empty() {
                    m.put(0, Value::Array(d.caps.iter().map(Capability::to_value).collect()));
                }
                if !d.events.is_empty() {
                    m.put(1, Value::Array(d.events.iter().map(EventInfo::to_value).collect()));
                }
                if let Some(p) = d.next_page {
                    m.uint(2, p);
                }
            }
            Message::Call { name, input } => {
                m.text(0, name).opt(1, input);
            }
            Message::Result { output } => {
                m.opt(0, output);
            }
            Message::Error(e) => {
                m.uint(0, e.code);
                if let Some(msg) = &e.msg {
                    m.text(1, msg);
                }
                m.opt(2, &e.detail);
            }
            Message::Event { name, data } => {
                m.text(0, name).opt(1, data);
            }
            Message::Pong { uptime_ms, boot_id } => {
                m.uint(0, *uptime_ms).bytes(1, boot_id);
            }
            Message::Pair { psk } => {
                m.bytes(0, psk);
            }
            Message::PairWithCode { psk, code } => {
                m.bytes(0, psk).bytes(1, code);
            }
            Message::FirmwareInfoR(info) => {
                m.text(0, &info.project_id).text(1, &info.build_id)
                    .uint(2, info.pairing_mode as u8).uint(3, info.boot_state as u8);
            }
            Message::Ping | Message::PairAck | Message::Unpair | Message::FirmwareInfo => {}
        }
        m.0
    }

    /// `m` ของข้อความที่รู้จัก `t` แล้ว — `None` = field ที่ต้องมีหายหรือผิดชนิด
    fn from_map(t: u64, m: &Value) -> Option<Message> {
        let msg = match t {
            1 => Message::Hello(Hello {
                sdp_min: uint(m, 0)?,
                sdp_max: uint(m, 1)?,
                host: text(m, 2)?,
                host_nonce: fixed(m, 3)?,
            }),
            2 => Message::HelloAck(HelloAck {
                sdp_min: uint(m, 0)?,
                sdp_max: uint(m, 1)?,
                version: uint(m, 2)?,
                device_id: fixed(m, 3)?,
                model: text(m, 4)?,
                fw: text(m, 5)?,
                max_frame: uint(m, 6)?,
                caps_hash: uint(m, 7)?,
                heartbeat_ms: uint(m, 8)?,
                boot_id: fixed(m, 9)?,
                pairing: match m.get(10)? {
                    Value::Bool(false) => None,
                    Value::Bool(true) => Some(PairProof { dev_nonce: fixed(m, 11)?, key_proof: fixed(m, 12)? }),
                    _ => return None,
                },
            }),
            3 => Message::Describe { page: uint(m, 0)? },
            4 => Message::DescribeR(DescribeR {
                caps: list(m, 0, Capability::from_value)?,
                events: list(m, 1, EventInfo::from_value)?,
                next_page: match m.get(2) {
                    None => None,
                    Some(v) => Some(u32::try_from(v.as_u64()?).ok()?),
                },
            }),
            5 => Message::Call { name: text(m, 0)?, input: m.get(1).cloned() },
            6 => Message::Result { output: m.get(0).cloned() },
            7 => Message::Error(ErrorMsg {
                code: uint(m, 0)?,
                msg: match m.get(1) {
                    None => None,
                    Some(v) => Some(String::from(v.as_text()?)),
                },
                detail: m.get(2).cloned(),
            }),
            8 => Message::Event { name: text(m, 0)?, data: m.get(1).cloned() },
            9 => Message::Ping,
            10 => Message::Pong { uptime_ms: uint(m, 0)?, boot_id: fixed(m, 1)? },
            11 => {
                let psk = fixed(m, 0)?;
                if m.get(1).is_some() {
                    let code: [u8; 8] = fixed(m, 1)?;
                    if !code.iter().all(u8::is_ascii_digit) { return None; }
                    Message::PairWithCode { psk, code }
                } else {
                    Message::Pair { psk }
                }
            }
            12 => Message::PairAck,
            13 => Message::Unpair,
            14 => Message::FirmwareInfo,
            15 => {
                let project_id = text(m, 0)?;
                let build_id = text(m, 1)?;
                if !valid_source_id(&project_id) || !valid_source_id(&build_id) {
                    return None;
                }
                Message::FirmwareInfoR(FirmwareInfo {
                    project_id,
                    build_id,
                    pairing_mode: match uint::<u8>(m, 2)? {
                        0 => PairingMode::Disabled,
                        1 => PairingMode::UsbConfirm,
                        2 => PairingMode::UsbCode,
                        _ => return None,
                    },
                    boot_state: match uint::<u8>(m, 3)? {
                        0 => BootState::Running,
                        1 => BootState::Pending,
                        2 => BootState::RolledBack,
                        _ => return None,
                    },
                })
            }
            _ => unreachable!("from_map เรียกเฉพาะ t ที่รู้จัก"),
        };
        Some(msg)
    }
}

const KNOWN_TYPES: core::ops::RangeInclusive<u64> = 1..=15;

impl Envelope {
    pub fn new(id: u16, msg: Message) -> Self {
        Envelope { id, msg }
    }

    pub fn encode(&self) -> Vec<u8> {
        Value::Array(alloc::vec![
            Value::Uint(self.msg.type_code()),
            Value::Uint(u64::from(self.id)),
            Value::Map(self.msg.to_map()),
        ])
        .to_bytes()
    }

    /// อ่านหัว `[t, id` ก่อน เพื่อให้รู้ `id` ที่จะตอบแม้ส่วนที่เหลือของ body จะเสีย (wire-format.md §กฎ encoding)
    pub fn decode(body: &[u8]) -> Result<Envelope, MessageError> {
        let mut reader = Reader::new(body);
        let (t, id) = read_header(&mut reader).map_err(|_| MessageError::Invalid { reply_id: 0 })?;
        let invalid = MessageError::Invalid { reply_id: id };

        let m = reader.value().map_err(|_| invalid)?;
        reader.finish().map_err(|_| invalid)?;
        if m.as_map().is_none() {
            return Err(invalid);
        }
        if !KNOWN_TYPES.contains(&t) {
            return Err(MessageError::UnknownMessage { id });
        }
        let msg = Message::from_map(t, &m).ok_or(invalid)?;
        Ok(Envelope { id, msg })
    }
}

fn read_header(reader: &mut Reader) -> Result<(u64, u16), DecodeError> {
    let head = reader.head()?;
    if head.major != 4 || head.arg != 3 {
        return Err(DecodeError::Unsupported);
    }
    let t = reader.value()?.as_u64().ok_or(DecodeError::Unsupported)?;
    let id = reader.value()?.as_u64().ok_or(DecodeError::Unsupported)?;
    let id = u16::try_from(id).map_err(|_| DecodeError::Unsupported)?;
    Ok((t, id))
}

// ---------- ตัวช่วยอ่าน/เขียน `m` ----------

#[derive(Default)]
struct MapBuilder(Vec<(Value, Value)>);

impl MapBuilder {
    fn put(&mut self, key: u64, v: Value) -> &mut Self {
        self.0.push((Value::Uint(key), v));
        self
    }
    fn uint(&mut self, key: u64, n: impl Into<u64>) -> &mut Self {
        self.put(key, Value::Uint(n.into()))
    }
    fn text(&mut self, key: u64, s: &str) -> &mut Self {
        self.put(key, Value::text(s))
    }
    fn bytes(&mut self, key: u64, b: &[u8]) -> &mut Self {
        self.put(key, Value::Bytes(b.to_vec()))
    }
    fn opt(&mut self, key: u64, v: &Option<Value>) -> &mut Self {
        if let Some(v) = v {
            self.put(key, v.clone());
        }
        self
    }
}

fn uint<T: TryFrom<u64>>(m: &Value, key: u64) -> Option<T> {
    T::try_from(m.get(key)?.as_u64()?).ok()
}

fn text(m: &Value, key: u64) -> Option<String> {
    Some(String::from(m.get(key)?.as_text()?))
}

fn fixed<const N: usize>(m: &Value, key: u64) -> Option<[u8; N]> {
    m.get(key)?.as_bytes()?.try_into().ok()
}

/// array ของ descriptor — ไม่มี key = ว่าง (ผู้ส่งไม่ส่ง array ว่าง)
fn list<T>(m: &Value, key: u64, parse: fn(&Value) -> Option<T>) -> Option<Vec<T>> {
    match m.get(key) {
        None => Some(Vec::new()),
        Some(Value::Array(items)) => items.iter().map(parse).collect(),
        Some(_) => None,
    }
}

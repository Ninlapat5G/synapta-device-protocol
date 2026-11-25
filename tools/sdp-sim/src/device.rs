//! ตัวอุปกรณ์จำลอง: ตอบข้อความแบบเดียวกับ firmware/arduino/SynaptaDevice/examples/LedDemo บน ESP32
//!
//! ไม่มี I/O ในไฟล์นี้ — server.rs เป็นคนรับส่งเฟรม เข้ารหัส และจับเวลา

use std::time::Instant;

use sdp_core::descriptor::{Capability, Danger, EventInfo};
use sdp_core::message::{code, DescribeR, ErrorMsg, Hello, HelloAck, PairProof};
use sdp_core::{Message, Value};

const SDP_VERSION: u16 = 1;
pub const MAX_FRAME: usize = 512;

pub struct Device {
    pub device_id: [u8; 8],
    pub boot_id: [u8; 4],
    pub heartbeat_ms: u32,
    started: Instant,
    led_on: bool,
    presses: u64,
}

fn text(s: &str) -> Value {
    Value::text(s)
}

fn object(fields: &[(&str, Value)]) -> Value {
    Value::Map(fields.iter().map(|(k, v)| (text(k), v.clone())).collect())
}

fn error(code: u16, msg: Option<String>) -> Message {
    Message::Error(ErrorMsg { code, msg, detail: None })
}

/// capability และ event ชุดเดียวกับ LedDemo — caps_hash จึงตรงกับ spec/vectors
pub fn descriptors() -> (Vec<Capability>, Vec<EventInfo>) {
    let led = Capability {
        name: "led.set".into(),
        description: Some("Turn the on-board LED on or off".into()),
        input_schema: Some(object(&[
            ("type", text("object")),
            ("properties", object(&[("on", object(&[("type", text("boolean"))]))])),
            ("required", Value::Array(vec![text("on")])),
        ])),
        output_schema: None,
        danger: Danger::Actuator,
    };
    let temp = Capability {
        name: "temp.read".into(),
        description: Some("Read board temperature in Celsius".into()),
        input_schema: None,
        output_schema: Some(object(&[
            ("type", text("object")),
            ("properties", object(&[("celsius", object(&[("type", text("number"))]))])),
        ])),
        danger: Danger::None,
    };
    let button = EventInfo {
        name: "button.pressed".into(),
        description: Some("BOOT button pressed".into()),
        data_schema: Some(object(&[
            ("type", text("object")),
            ("properties", object(&[("count", object(&[("type", text("integer")), ("minimum", Value::Uint(0))]))])),
        ])),
    };
    (vec![led, temp], vec![button])
}

impl Device {
    pub fn new(device_id: [u8; 8], boot_id: [u8; 4], heartbeat_ms: u32) -> Self {
        Device { device_id, boot_id, heartbeat_ms, started: Instant::now(), led_on: false, presses: 0 }
    }

    pub fn led_on(&self) -> bool {
        self.led_on
    }

    /// `pairing` = พิสูจน์ว่ามี PSK (server.rs คิดให้เพราะต้องเปิด session พร้อมกัน)
    pub fn hello(&self, hello: &Hello, pairing: Option<PairProof>) -> Message {
        if hello.sdp_min > SDP_VERSION || hello.sdp_max < SDP_VERSION || hello.sdp_min > hello.sdp_max {
            let range = Value::Map(vec![
                (Value::Uint(0), Value::Uint(SDP_VERSION.into())),
                (Value::Uint(1), Value::Uint(SDP_VERSION.into())),
            ]);
            return Message::Error(ErrorMsg { code: code::VERSION_MISMATCH, msg: None, detail: Some(range) });
        }
        let (caps, events) = descriptors();
        Message::HelloAck(HelloAck {
            sdp_min: SDP_VERSION,
            sdp_max: SDP_VERSION,
            version: SDP_VERSION,
            device_id: self.device_id,
            model: "sdp-sim".into(),
            fw: concat!("sdp-sim ", env!("CARGO_PKG_VERSION")).into(),
            max_frame: MAX_FRAME as u32,
            caps_hash: sdp_core::descriptor::caps_hash([(&caps[..], &events[..])]),
            heartbeat_ms: self.heartbeat_ms,
            boot_id: self.boot_id,
            pairing,
        })
    }

    /// ข้อความอื่นนอกจาก hello — `wireless` = มาทาง TCP (ห้ามจับคู่)
    pub fn handle(&mut self, msg: Message, wireless: bool) -> Message {
        match msg {
            Message::Describe { page: 0 } => {
                let (caps, events) = descriptors();
                Message::DescribeR(DescribeR { caps, events, next_page: None })
            }
            Message::Describe { .. } => error(code::INVALID_INPUT, None),
            Message::Call { name, input } => self.call(&name, input),
            Message::Ping => Message::Pong {
                uptime_ms: self.started.elapsed().as_millis() as u64 & 0xFFFF_FFFF,
                boot_id: self.boot_id,
            },
            Message::Pair { .. } | Message::Unpair if wireless => error(code::FORBIDDEN_TRANSPORT, None),
            Message::Pair { .. } | Message::Unpair => Message::PairAck,
            _ => error(code::UNKNOWN_MESSAGE, None),
        }
    }

    fn call(&mut self, name: &str, input: Option<Value>) -> Message {
        match name {
            "led.set" => {
                let on = match input.as_ref().and_then(|i| i.get_text("on")) {
                    Some(Value::Bool(on)) => *on,
                    Some(_) => return error(code::INVALID_INPUT, Some("'on' must be boolean".into())),
                    None => return error(code::INVALID_INPUT, Some("'on' is required".into())),
                };
                self.led_on = on;
                Message::Result { output: Some(object(&[("on", Value::Bool(on))])) }
            }
            "temp.read" => Message::Result { output: Some(object(&[("celsius", Value::Float(23.5))])) },
            _ => error(code::UNKNOWN_CAPABILITY, Some(format!("no capability '{name}'"))),
        }
    }

    /// host หลุด: ปิดทุกอย่างที่ agent อาจสั่งค้างไว้ (onLinkLost ของ LedDemo)
    pub fn link_lost(&mut self) {
        self.led_on = false;
    }

    pub fn press_button(&mut self) -> Message {
        self.presses += 1;
        Message::Event { name: "button.pressed".into(), data: Some(object(&[("count", Value::Uint(self.presses))])) }
    }
}

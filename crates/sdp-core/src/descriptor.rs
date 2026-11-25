//! คำอธิบายความสามารถ (capability) และ event ที่อุปกรณ์ส่งใน `describe_r`

use alloc::string::String;
use alloc::vec::Vec;

use crate::cbor::Value;
use crate::crc::crc32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Danger {
    None,
    Actuator,
    Destructive,
}

impl Danger {
    /// ค่าที่ไม่รู้จัก (จากสเปกรุ่นหลัง) ถือเป็น `Destructive` — ไม่รู้ว่าอันตรายแค่ไหน ให้ถามสิทธิ์ไว้ก่อน
    pub fn from_code(code: u64) -> Danger {
        match code {
            0 => Danger::None,
            1 => Danger::Actuator,
            _ => Danger::Destructive,
        }
    }

    pub fn code(self) -> u64 {
        match self {
            Danger::None => 0,
            Danger::Actuator => 1,
            Danger::Destructive => 2,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Capability {
    pub name: String,
    pub description: Option<String>,
    pub input_schema: Option<Value>,
    pub output_schema: Option<Value>,
    pub danger: Danger,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EventInfo {
    pub name: String,
    pub description: Option<String>,
    pub data_schema: Option<Value>,
}

impl Capability {
    pub fn to_value(&self) -> Value {
        let mut m = Vec::new();
        m.push((Value::Uint(0), Value::Text(self.name.clone())));
        push_opt_text(&mut m, 1, &self.description);
        push_opt(&mut m, 2, &self.input_schema);
        push_opt(&mut m, 3, &self.output_schema);
        if self.danger != Danger::None {
            m.push((Value::Uint(4), Value::Uint(self.danger.code())));
        }
        Value::Map(m)
    }

    pub fn from_value(v: &Value) -> Option<Capability> {
        Some(Capability {
            name: String::from(v.get(0)?.as_text()?),
            description: opt_text(v, 1)?,
            input_schema: v.get(2).cloned(),
            output_schema: v.get(3).cloned(),
            danger: match v.get(4) {
                None => Danger::None,
                Some(d) => Danger::from_code(d.as_u64()?),
            },
        })
    }
}

impl EventInfo {
    pub fn to_value(&self) -> Value {
        let mut m = Vec::new();
        m.push((Value::Uint(0), Value::Text(self.name.clone())));
        push_opt_text(&mut m, 1, &self.description);
        push_opt(&mut m, 2, &self.data_schema);
        Value::Map(m)
    }

    pub fn from_value(v: &Value) -> Option<EventInfo> {
        Some(EventInfo {
            name: String::from(v.get(0)?.as_text()?),
            description: opt_text(v, 1)?,
            data_schema: v.get(2).cloned(),
        })
    }
}

/// `caps_hash` ของชุด descriptor ตามลำดับที่ส่ง (แต่ละหน้า caps ก่อน events)
///
/// ใช้ฝั่งที่เป็นเจ้าของ descriptor (ตัวจำลอง, firmware ที่เป็น Rust) — host ใช้ค่าจาก `hello_ack` เป็น key ของ cache
/// ไม่ต้องคำนวณเอง เพราะ descriptor ที่ถอดมาไม่เก็บ key ที่ไม่รู้จัก เข้ารหัสใหม่แล้วอาจไม่ตรงกับ byte ที่อุปกรณ์ส่ง
pub fn caps_hash<'a>(pages: impl IntoIterator<Item = (&'a [Capability], &'a [EventInfo])>) -> u32 {
    let mut all = Vec::new();
    for (caps, events) in pages {
        caps.iter().for_each(|c| c.to_value().encode(&mut all));
        events.iter().for_each(|e| e.to_value().encode(&mut all));
    }
    crc32(&all)
}

fn push_opt(m: &mut Vec<(Value, Value)>, key: u64, v: &Option<Value>) {
    if let Some(v) = v {
        m.push((Value::Uint(key), v.clone()));
    }
}

fn push_opt_text(m: &mut Vec<(Value, Value)>, key: u64, v: &Option<String>) {
    if let Some(s) = v {
        m.push((Value::Uint(key), Value::Text(s.clone())));
    }
}

/// `Some(None)` = ไม่มี key, `None` = มีแต่ชนิดผิด
fn opt_text(v: &Value, key: u64) -> Option<Option<String>> {
    match v.get(key) {
        None => Some(None),
        Some(t) => Some(Some(String::from(t.as_text()?))),
    }
}

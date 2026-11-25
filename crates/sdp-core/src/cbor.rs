//! CBOR ชุดที่ SDP ใช้: เข้ารหัสแบบ deterministic และถอดแบบเข้มตาม wire-format.md §กฎ encoding
//!
//! เขียนเองแทนใช้ crate เพราะต้องคุมกฎเฉพาะของ SDP: ปฏิเสธ indefinite length, ปฏิเสธ byte เกินท้าย,
//! จำกัดความลึก, และรับเลขรูปไม่สั้นสุดได้ — crate ทั่วไปตั้งค่าเหล่านี้ไม่ครบ

use alloc::string::String;
use alloc::vec::Vec;

/// ซ้อนลึกได้เท่านี้ — กันข้อความที่ซ้อนไม่จำกัดทำ stack ล้น (schema จริงลึกไม่เกิน 3 ชั้น บวกโครงข้อความอีกไม่กี่ชั้น)
pub const MAX_DEPTH: usize = 16;

#[derive(Debug, Clone)]
pub enum Value {
    Uint(u64),
    /// ค่าจริง = -1 - n (แทนได้ถึง -2^64 ตาม CBOR)
    Nint(u64),
    Bytes(Vec<u8>),
    Text(String),
    Array(Vec<Value>),
    /// เก็บตามลำดับที่ถอดมา ตอนเข้ารหัสจะเรียงใหม่ตามกฎ deterministic
    Map(Vec<(Value, Value)>),
    Bool(bool),
    Null,
    Float(f64),
}

/// map เทียบแบบไม่สนลำดับ key — CBOR map ไม่มีลำดับ ผู้ส่งต่างรุ่นเรียงต่างกันได้แต่ความหมายเดียวกัน
impl PartialEq for Value {
    fn eq(&self, other: &Value) -> bool {
        match (self, other) {
            (Value::Uint(a), Value::Uint(b)) | (Value::Nint(a), Value::Nint(b)) => a == b,
            (Value::Bytes(a), Value::Bytes(b)) => a == b,
            (Value::Text(a), Value::Text(b)) => a == b,
            (Value::Array(a), Value::Array(b)) => a == b,
            (Value::Map(a), Value::Map(b)) => {
                a.len() == b.len() && a.iter().all(|(k, v)| b.iter().any(|(k2, v2)| k == k2 && v == v2))
            }
            (Value::Bool(a), Value::Bool(b)) => a == b,
            (Value::Null, Value::Null) => true,
            (Value::Float(a), Value::Float(b)) => a == b,
            _ => false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    Truncated,
    Indefinite,
    TrailingBytes,
    /// tag, simple value ที่ไม่รองรับ หรือรหัส additional info ที่สงวน
    Unsupported,
    TooDeep,
    InvalidUtf8,
}

impl Value {
    pub fn text(s: &str) -> Value {
        Value::Text(String::from(s))
    }

    pub fn int(n: i64) -> Value {
        if n >= 0 {
            Value::Uint(n as u64)
        } else {
            Value::Nint((-1 - n) as u64)
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Value::Uint(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_text(&self) -> Option<&str> {
        match self {
            Value::Text(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::Bytes(b) => Some(b),
            _ => None,
        }
    }

    pub fn as_map(&self) -> Option<&[(Value, Value)]> {
        match self {
            Value::Map(m) => Some(m),
            _ => None,
        }
    }

    /// หาค่าใน map ด้วย key ที่เป็นเลข
    pub fn get(&self, key: u64) -> Option<&Value> {
        self.as_map()?.iter().find(|(k, _)| *k == Value::Uint(key)).map(|(_, v)| v)
    }

    /// หาค่าใน map ด้วย key ที่เป็นข้อความ
    pub fn get_text(&self, key: &str) -> Option<&Value> {
        self.as_map()?.iter().find(|(k, _)| k.as_text() == Some(key)).map(|(_, v)| v)
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::new();
        self.encode(&mut out);
        out
    }

    pub fn encode(&self, out: &mut Vec<u8>) {
        match self {
            Value::Uint(n) => write_head(out, 0, *n),
            Value::Nint(n) => write_head(out, 1, *n),
            Value::Bytes(b) => {
                write_head(out, 2, b.len() as u64);
                out.extend_from_slice(b);
            }
            Value::Text(s) => {
                write_head(out, 3, s.len() as u64);
                out.extend_from_slice(s.as_bytes());
            }
            Value::Array(items) => {
                write_head(out, 4, items.len() as u64);
                for item in items {
                    item.encode(out);
                }
            }
            Value::Map(pairs) => {
                // เรียงตาม byte ของ key ที่เข้ารหัสแล้ว (RFC 8949 §4.2.1)
                let mut encoded: Vec<(Vec<u8>, &Value)> = pairs.iter().map(|(k, v)| (k.to_bytes(), v)).collect();
                encoded.sort_by(|a, b| a.0.cmp(&b.0));
                write_head(out, 5, encoded.len() as u64);
                for (key, value) in encoded {
                    out.extend_from_slice(&key);
                    value.encode(out);
                }
            }
            Value::Bool(false) => out.push(0xF4),
            Value::Bool(true) => out.push(0xF5),
            Value::Null => out.push(0xF6),
            Value::Float(f) => {
                // 32 บิตเมื่อแทนค่าได้ตรง ไม่งั้น 64 (AVR มีแค่ float 32 บิต)
                let narrow = *f as f32;
                if f64::from(narrow) == *f || f.is_nan() {
                    out.push(0xFA);
                    out.extend_from_slice(&narrow.to_be_bytes());
                } else {
                    out.push(0xFB);
                    out.extend_from_slice(&f.to_be_bytes());
                }
            }
        }
    }

    /// ถอด CBOR item เดียวที่ต้องจบพอดีท้าย `bytes`
    pub fn decode(bytes: &[u8]) -> Result<Value, DecodeError> {
        let mut reader = Reader::new(bytes);
        let value = reader.value()?;
        reader.finish()?;
        Ok(value)
    }
}

fn write_head(out: &mut Vec<u8>, major: u8, n: u64) {
    let major = major << 5;
    if n < 24 {
        out.push(major | n as u8);
    } else if n <= 0xFF {
        out.extend_from_slice(&[major | 24, n as u8]);
    } else if n <= 0xFFFF {
        out.push(major | 25);
        out.extend_from_slice(&(n as u16).to_be_bytes());
    } else if n <= 0xFFFF_FFFF {
        out.push(major | 26);
        out.extend_from_slice(&(n as u32).to_be_bytes());
    } else {
        out.push(major | 27);
        out.extend_from_slice(&n.to_be_bytes());
    }
}

/// หัวของ item หนึ่งตัว: major type + ตัวเลข (ความยาว ค่า หรือ bit ของ float)
#[derive(Debug, Clone, Copy)]
pub struct Head {
    pub major: u8,
    pub info: u8,
    pub arg: u64,
}

/// ตัวอ่านทีละ item — `message` ใช้อ่านหัวข้อความทีละช่องเพื่อรู้ `id` ก่อนทั้ง body จะผ่าน
pub struct Reader<'a> {
    bytes: &'a [u8],
    pos: usize,
    depth: usize,
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Reader { bytes, pos: 0, depth: 0 }
    }

    pub fn finish(&self) -> Result<(), DecodeError> {
        if self.pos == self.bytes.len() {
            Ok(())
        } else {
            Err(DecodeError::TrailingBytes)
        }
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], DecodeError> {
        if self.bytes.len() - self.pos < n {
            return Err(DecodeError::Truncated);
        }
        let slice = &self.bytes[self.pos..self.pos + n];
        self.pos += n;
        Ok(slice)
    }

    fn remaining(&self) -> usize {
        self.bytes.len() - self.pos
    }

    pub fn head(&mut self) -> Result<Head, DecodeError> {
        let first = self.take(1)?[0];
        let major = first >> 5;
        let info = first & 0x1F;
        let arg = match info {
            0..=23 => u64::from(info),
            24 => u64::from(self.take(1)?[0]),
            25 => u64::from(u16::from_be_bytes(self.take(2)?.try_into().unwrap())),
            26 => u64::from(u32::from_be_bytes(self.take(4)?.try_into().unwrap())),
            27 => u64::from_be_bytes(self.take(8)?.try_into().unwrap()),
            31 => return Err(DecodeError::Indefinite),
            _ => return Err(DecodeError::Unsupported),
        };
        Ok(Head { major, info, arg })
    }

    /// ความยาวที่ประกาศต้องไม่เกิน byte ที่เหลือ — กันข้อความปลอมสั่งจอง RAM มหาศาล
    fn length(&self, declared: u64, min_item_size: usize) -> Result<usize, DecodeError> {
        let declared = usize::try_from(declared).map_err(|_| DecodeError::Truncated)?;
        if declared.saturating_mul(min_item_size) > self.remaining() {
            return Err(DecodeError::Truncated);
        }
        Ok(declared)
    }

    pub fn value(&mut self) -> Result<Value, DecodeError> {
        let head = self.head()?;
        match head.major {
            0 => Ok(Value::Uint(head.arg)),
            1 => Ok(Value::Nint(head.arg)),
            2 => {
                let len = self.length(head.arg, 1)?;
                Ok(Value::Bytes(self.take(len)?.to_vec()))
            }
            3 => {
                let len = self.length(head.arg, 1)?;
                let raw = self.take(len)?;
                let text = core::str::from_utf8(raw).map_err(|_| DecodeError::InvalidUtf8)?;
                Ok(Value::Text(String::from(text)))
            }
            4 => {
                let len = self.length(head.arg, 1)?;
                self.nested(|r| {
                    let mut items = Vec::with_capacity(len);
                    for _ in 0..len {
                        items.push(r.value()?);
                    }
                    Ok(Value::Array(items))
                })
            }
            5 => {
                let len = self.length(head.arg, 2)?;
                self.nested(|r| {
                    let mut pairs = Vec::with_capacity(len);
                    for _ in 0..len {
                        let key = r.value()?;
                        let value = r.value()?;
                        pairs.push((key, value));
                    }
                    Ok(Value::Map(pairs))
                })
            }
            7 => match (head.info, head.arg) {
                (20, _) => Ok(Value::Bool(false)),
                (21, _) => Ok(Value::Bool(true)),
                (22, _) => Ok(Value::Null),
                (25, bits) => Ok(Value::Float(f16_to_f64(bits as u16))),
                (26, bits) => Ok(Value::Float(f64::from(f32::from_bits(bits as u32)))),
                (27, bits) => Ok(Value::Float(f64::from_bits(bits))),
                _ => Err(DecodeError::Unsupported),
            },
            _ => Err(DecodeError::Unsupported), // tag (6) ไม่ใช้ใน SDP
        }
    }

    fn nested(&mut self, f: impl FnOnce(&mut Self) -> Result<Value, DecodeError>) -> Result<Value, DecodeError> {
        if self.depth >= MAX_DEPTH {
            return Err(DecodeError::TooDeep);
        }
        self.depth += 1;
        let result = f(self);
        self.depth -= 1;
        result
    }
}

/// half float (IEEE 754 binary16) → f64 — core ไม่มี f16 ใน Rust stable
fn f16_to_f64(bits: u16) -> f64 {
    let sign = if bits & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exp = i32::from((bits >> 10) & 0x1F);
    let frac = f64::from(bits & 0x3FF);
    let magnitude = match exp {
        0 => frac * pow2(-24), // subnormal
        31 if frac == 0.0 => f64::INFINITY,
        31 => f64::NAN,
        _ => (1.0 + frac / 1024.0) * pow2(exp - 15),
    };
    sign * magnitude
}

/// 2^n แบบไม่พึ่ง std (`f64::powi` อยู่ใน std)
fn pow2(n: i32) -> f64 {
    f64::from_bits(((1023 + n) as u64) << 52)
}

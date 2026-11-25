//! แสดง CBOR Value แบบ JSON ให้คนอ่าน (byte string เป็น h'..' แบบ CBOR diagnostic)

use sdp_core::Value;

pub fn show(v: &Value) -> String {
    match v {
        Value::Uint(n) => n.to_string(),
        Value::Nint(n) => format!("-{}", u128::from(*n) + 1),
        Value::Bytes(b) => format!("h'{}'", hex(b)),
        Value::Text(s) => format!("{s:?}"),
        Value::Array(items) => format!("[{}]", items.iter().map(show).collect::<Vec<_>>().join(", ")),
        Value::Map(pairs) => {
            let inner: Vec<String> = pairs.iter().map(|(k, v)| format!("{}: {}", show(k), show(v))).collect();
            format!("{{{}}}", inner.join(", "))
        }
        Value::Bool(b) => b.to_string(),
        Value::Null => "null".into(),
        Value::Float(f) => f.to_string(),
    }
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

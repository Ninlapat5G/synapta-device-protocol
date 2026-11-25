//! ข้อมูลจากสายเชื่อไม่ได้ (บอร์ดพัง, คนในวง WiFi) — ตัวถอดต้องไม่ panic ไม่จอง RAM ตามที่ข้อความอ้าง และไม่ stack ล้น

use sdp_core::cbor::{DecodeError, Value, MAX_DEPTH};
use sdp_core::crypto::{OpenError, Role, Session};
use sdp_core::frame::SerialReceiver;
use sdp_core::Envelope;

#[test]
fn declared_length_larger_than_input_is_rejected_without_allocating() {
    let huge_bytes = [0x5B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
    assert_eq!(Value::decode(&huge_bytes), Err(DecodeError::Truncated));
    let huge_array = [0x9A, 0xFF, 0xFF, 0xFF, 0xFF, 0x00];
    assert_eq!(Value::decode(&huge_array), Err(DecodeError::Truncated));
}

#[test]
fn nesting_deeper_than_limit_is_rejected() {
    let ok = [vec![0x81; MAX_DEPTH], vec![0x00]].concat();
    assert!(Value::decode(&ok).is_ok());
    let too_deep = [vec![0x81; MAX_DEPTH + 1], vec![0x00]].concat();
    assert_eq!(Value::decode(&too_deep), Err(DecodeError::TooDeep));
}

#[test]
fn half_floats() {
    let cases: [(&[u8], f64); 4] = [
        (&[0xF9, 0x3C, 0x00], 1.0),
        (&[0xF9, 0x00, 0x01], 5.960_464_477_539_063e-8), // subnormal เล็กสุด
        (&[0xF9, 0x7C, 0x00], f64::INFINITY),
        (&[0xF9, 0xC4, 0x00], -4.0),
    ];
    for (bytes, want) in cases {
        assert_eq!(Value::decode(bytes), Ok(Value::Float(want)));
    }
}

#[test]
fn map_equality_ignores_key_order() {
    let a = Value::Map(vec![(Value::Uint(0), Value::Uint(1)), (Value::Uint(1), Value::Uint(2))]);
    let b = Value::Map(vec![(Value::Uint(1), Value::Uint(2)), (Value::Uint(0), Value::Uint(1))]);
    assert_eq!(a, b);
    assert_eq!(a.to_bytes(), b.to_bytes());
}

#[test]
fn plaintext_and_short_frames_are_refused_by_session() {
    let mut s = Session::new(&[7; 16], Role::Device);
    assert_eq!(s.open(&[0x00, 0x83, 0x09, 0x01, 0xA0]), Err(OpenError::NotEncrypted));
    assert_eq!(s.open(&[0x01, 0, 0, 0, 1]), Err(OpenError::TooShort));
    assert_eq!(s.open(&[]), Err(OpenError::TooShort));
}

#[test]
fn sealed_frames_round_trip_between_host_and_device() {
    let key = [9; 16];
    let (mut host, mut device) = (Session::new(&key, Role::Host), Session::new(&key, Role::Device));
    for body in [&b""[..], b"\x83\x09\x01\xa0", &[0xAA; 300]] {
        let inner = host.seal(&[0x01], body).unwrap();
        assert_eq!(device.open(&inner).unwrap(), body);
    }
    let with_addr = host.seal(&[0x03, 0x05], b"rs485").unwrap();
    assert_eq!(device.open(&with_addr).unwrap(), b"rs485");
}

/// ตัวสุ่มแบบกำหนดได้ — เทสต์ซ้ำได้ผลเดิม ไม่ต้องพึ่ง crate สุ่ม
struct Lcg(u64);
impl Lcg {
    fn next(&mut self) -> u8 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as u8
    }
}

#[test]
fn random_input_never_panics() {
    let mut rng = Lcg(0x5D9);
    let mut rx = SerialReceiver::new(128);
    let mut session = Session::new(&[1; 16], Role::Host);
    for _ in 0..20_000 {
        let len = usize::from(rng.next() % 64);
        let bytes: Vec<u8> = (0..len).map(|_| rng.next()).collect();
        let _ = Value::decode(&bytes);
        let _ = Envelope::decode(&bytes);
        let _ = session.open(&bytes);
        for &b in &bytes {
            let _ = rx.push(b);
        }
    }
}

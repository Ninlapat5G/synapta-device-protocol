//! ตรวจ sdp-core กับ spec/vectors/*.json ทุกรายการ — vector ที่ไม่รู้จักชื่อทำให้เทสต์ตก ไม่ถูกข้ามเงียบ ๆ

use sdp_core::cbor::Value;
use sdp_core::crc::{crc16_ccitt_false, crc32};
use sdp_core::crypto::{key_proof, session_key, OpenError, Role, Session};
use sdp_core::descriptor::{caps_hash, Capability, Danger, EventInfo};
use sdp_core::firmware::{BootState, FirmwareInfo, PairingMode, UploadExpectation, UploadOutcome};
use sdp_core::frame::{encode_serial, encode_tcp, SerialEvent, SerialReceiver};
use sdp_core::message::{DescribeR, ErrorMsg, Hello, HelloAck, PairProof};
use sdp_core::{cobs, Envelope, Message};
use serde_json::Value as Json;

fn load(name: &str) -> Json {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../spec/vectors/");
    let text = std::fs::read_to_string(format!("{path}{name}")).expect("อ่านไฟล์ vector ไม่ได้");
    serde_json::from_str(&text).unwrap()
}

fn hex(s: &Json) -> Vec<u8> {
    let s = s.as_str().expect("ค่า hex ต้องเป็น string");
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

fn to_hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn arr<const N: usize>(h: &str) -> [u8; N] {
    hex(&Json::from(h)).try_into().unwrap()
}

// ---------- อุปกรณ์ตัวอย่างเดียวกับ generate.py ----------

fn tmap(pairs: &[(&str, Value)]) -> Value {
    Value::Map(pairs.iter().map(|(k, v)| (Value::text(k), v.clone())).collect())
}

fn ty(t: &str) -> Value {
    tmap(&[("type", Value::text(t))])
}

fn cap_led() -> Capability {
    Capability {
        name: "led.set".into(),
        description: Some("Turn the on-board LED on or off".into()),
        input_schema: Some(tmap(&[
            ("type", Value::text("object")),
            ("properties", tmap(&[("on", ty("boolean"))])),
            ("required", Value::Array(vec![Value::text("on")])),
        ])),
        output_schema: None,
        danger: Danger::Actuator,
    }
}

fn cap_temp() -> Capability {
    Capability {
        name: "temp.read".into(),
        description: Some("Read board temperature in Celsius".into()),
        input_schema: None,
        output_schema: Some(tmap(&[
            ("type", Value::text("object")),
            ("properties", tmap(&[("celsius", ty("number"))])),
        ])),
        danger: Danger::None,
    }
}

fn evt_button() -> EventInfo {
    EventInfo {
        name: "button.pressed".into(),
        description: Some("BOOT button pressed".into()),
        data_schema: Some(tmap(&[
            ("type", Value::text("object")),
            ("properties", tmap(&[("count", tmap(&[("type", Value::text("integer")), ("minimum", Value::Uint(0))]))])),
        ])),
    }
}

const PSK: &str = "000102030405060708090a0b0c0d0e0f";
const HOST_NONCE: &str = "0102030405060708";
const DEV_NONCE: &str = "1112131415161718";
const BOOT_ID: &str = "0badf00d";

fn example_caps_hash() -> u32 {
    caps_hash([(&[cap_led(), cap_temp()][..], &[evt_button()][..])])
}

fn hello_ack(pairing: Option<PairProof>) -> Message {
    Message::HelloAck(HelloAck {
        sdp_min: 1,
        sdp_max: 1,
        version: 1,
        device_id: arr("a1b2c3d4e5f60718"),
        model: "esp32-devkit".into(),
        fw: "led-demo 1.0.0".into(),
        max_frame: 512,
        caps_hash: example_caps_hash(),
        heartbeat_ms: 1000,
        boot_id: arr(BOOT_ID),
        pairing,
    })
}

fn object(pairs: &[(&str, Value)]) -> Option<Value> {
    Some(tmap(pairs))
}

fn expected(name: &str) -> Envelope {
    let (id, msg) = match name {
        "hello" => (
            1,
            Message::Hello(Hello { sdp_min: 1, sdp_max: 1, host: "SynaptaCode".into(), host_nonce: arr(HOST_NONCE) }),
        ),
        "hello_ack" => (1, hello_ack(None)),
        "hello_ack_paired" => {
            let proof = key_proof(&arr(PSK), &arr(HOST_NONCE), &arr(DEV_NONCE));
            (1, hello_ack(Some(PairProof { dev_nonce: arr(DEV_NONCE), key_proof: proof })))
        }
        "error_version_mismatch" => (
            1,
            Message::Error(ErrorMsg {
                code: 1,
                msg: None,
                detail: Some(Value::Map(vec![(Value::Uint(0), Value::Uint(2)), (Value::Uint(1), Value::Uint(3))])),
            }),
        ),
        "describe" => (2, Message::Describe { page: 0 }),
        "describe_r" => (
            2,
            Message::DescribeR(DescribeR {
                caps: vec![cap_led(), cap_temp()],
                events: vec![evt_button()],
                next_page: None,
            }),
        ),
        "describe_r_page0" => {
            (2, Message::DescribeR(DescribeR { caps: vec![cap_led()], events: vec![], next_page: Some(1) }))
        }
        "describe_page1" => (3, Message::Describe { page: 1 }),
        "describe_r_page1" => {
            (3, Message::DescribeR(DescribeR { caps: vec![cap_temp()], events: vec![evt_button()], next_page: None }))
        }
        "call_led_set" => (4, Message::Call { name: "led.set".into(), input: object(&[("on", Value::Bool(true))]) }),
        "result_led_set" => (4, Message::Result { output: object(&[("on", Value::Bool(true))]) }),
        "call_temp_read" => (5, Message::Call { name: "temp.read".into(), input: None }),
        "result_temp_read" => (5, Message::Result { output: object(&[("celsius", Value::Float(23.5))]) }),
        "result_float64" => (12, Message::Result { output: object(&[("ratio", Value::Float(0.1))]) }),
        "result_empty" => (6, Message::Result { output: None }),
        "error_unknown_capability" => {
            (7, Message::Error(ErrorMsg { code: 2, msg: Some("no capability 'led.blink'".into()), detail: None }))
        }
        "error_frame_too_large" => (0, Message::Error(ErrorMsg { code: 6, msg: None, detail: None })),
        "event_button" => {
            (0, Message::Event { name: "button.pressed".into(), data: object(&[("count", Value::Uint(3))]) })
        }
        "ping" => (8, Message::Ping),
        "pong" => (8, Message::Pong { uptime_ms: 123456, boot_id: arr(BOOT_ID) }),
        "pong_uptime_max" => (9, Message::Pong { uptime_ms: 0xFFFF_FFFF, boot_id: arr(BOOT_ID) }),
        "pair" => (10, Message::Pair { psk: arr(PSK) }),
        "pair_ack" => (10, Message::PairAck),
        "unpair" => (11, Message::Unpair),
        "pair_with_code" => (13, Message::PairWithCode { psk: arr(PSK), code: *b"01234567" }),
        "firmware_info" => (14, Message::FirmwareInfo),
        "firmware_info_r" => (14, Message::FirmwareInfoR(FirmwareInfo {
            project_id: "led-demo".into(), build_id: "demo-build-a".into(),
            pairing_mode: PairingMode::Disabled, boot_state: BootState::Running,
        })),
        "id_max" => (65535, Message::Ping),
        other => panic!("vector ใหม่ที่เทสต์ยังไม่รู้จัก: {other}"),
    };
    Envelope::new(id, msg)
}

// ---------- เทสต์ ----------

#[test]
fn upload_outcome_requires_the_same_board_and_a_confirmed_boot() {
    let Message::HelloAck(before) = expected("hello_ack").msg else { panic!("missing hello_ack vector") };
    let Message::FirmwareInfoR(old) = expected("firmware_info_r").msg else { panic!("missing firmware info vector") };
    let upload = UploadExpectation { previous_hello: &before, previous_firmware: &old, project_id: "led-demo", build_id: "demo-build-b" };
    let mut after = before.clone();
    after.boot_id[0] ^= 1;
    let new = FirmwareInfo { build_id: "demo-build-b".into(), ..old.clone() };
    assert_eq!(upload.verify(&after, &new), UploadOutcome::Verified);
    assert_eq!(upload.verify(&before, &new), UploadOutcome::Unknown);
    assert_eq!(upload.verify(&after, &FirmwareInfo { boot_state: BootState::Pending, ..new }), UploadOutcome::Unknown);
    assert_eq!(upload.verify(&after, &old), UploadOutcome::Unknown);
    let rolled_back = FirmwareInfo { boot_state: BootState::RolledBack, ..old.clone() };
    assert_eq!(upload.verify(&after, &rolled_back), UploadOutcome::RolledBack);
    after.device_id[0] ^= 1;
    assert_eq!(upload.verify(&after, &rolled_back), UploadOutcome::Unknown);
}

#[test]
fn messages() {
    let file = load("messages.json");
    assert_eq!(format!("{:08x}", example_caps_hash()), file["caps_hash"].as_str().unwrap());

    for v in file["messages"].as_array().unwrap() {
        let name = v["name"].as_str().unwrap();
        let want = expected(name);
        let body = hex(&v["body"]);
        assert_eq!(to_hex(&want.encode()), to_hex(&body), "เข้ารหัส {name}");
        assert_eq!(Envelope::decode(&body).as_ref(), Ok(&want), "ถอด {name}");

        let inner = [&[0u8][..], &body].concat();
        assert_eq!(encode_serial(&inner), hex(&v["serial"]), "เฟรม serial {name}");
        assert_eq!(encode_tcp(&inner), hex(&v["tcp"]), "เฟรม tcp {name}");

        let mut rx = SerialReceiver::new(sdp_core::HOST_RX_LIMIT);
        let events: Vec<_> = hex(&v["serial"]).into_iter().filter_map(|b| rx.push(b)).collect();
        assert_eq!(events, vec![SerialEvent::Frame(inner)], "รับเฟรม serial {name}");
    }
}

#[test]
fn framing() {
    let file = load("framing.json");
    for v in file["crc16_ccitt_false"].as_array().unwrap() {
        assert_eq!(format!("{:04x}", crc16_ccitt_false(&hex(&v["data"]))), v["crc"].as_str().unwrap());
    }
    for v in file["crc32"].as_array().unwrap() {
        assert_eq!(format!("{:08x}", crc32(&hex(&v["data"]))), v["crc"].as_str().unwrap());
    }
    for v in file["cobs"].as_array().unwrap() {
        let (raw, enc) = (hex(&v["raw"]), hex(&v["encoded"]));
        assert_eq!(to_hex(&cobs::encode(&raw)), to_hex(&enc));
        assert_eq!(cobs::decode(&enc), Some(raw));
    }

    for v in file["serial_streams"].as_array().unwrap() {
        let name = v["name"].as_str().unwrap();
        let mut rx = SerialReceiver::new(v["max_frame"].as_u64().unwrap() as usize);
        let got: Vec<Json> = hex(&v["stream"])
            .into_iter()
            .filter_map(|b| rx.push(b))
            .map(|e| match e {
                SerialEvent::Frame(inner) => serde_json::json!({ "frame": to_hex(&inner) }),
                SerialEvent::Drop { reason, raw } if raw.is_empty() => serde_json::json!({ "drop": reason.as_str() }),
                SerialEvent::Drop { reason, raw } => {
                    serde_json::json!({ "drop": reason.as_str(), "raw": to_hex(&raw) })
                }
            })
            .collect();
        assert_eq!(&Json::from(got), &v["events"], "stream {name}");
        assert_eq!(rx.pending_len() as u64, v["pending_bytes"].as_u64().unwrap(), "pending {name}");
    }
}

#[test]
fn decode_rules() {
    let file = load("decode.json");
    for v in file["cases"].as_array().unwrap() {
        let name = v["name"].as_str().unwrap();
        let result = Envelope::decode(&hex(&v["body"]));
        match v["expect"].as_str().unwrap() {
            "ok" => assert_eq!(result, Ok(expected(v["same_as"].as_str().unwrap())), "case {name}"),
            "error" => {
                let err = result.expect_err(name);
                assert_eq!(u64::from(err.code()), v["error"].as_u64().unwrap(), "code {name}");
                assert_eq!(u64::from(err.reply_id()), v["reply_id"].as_u64().unwrap(), "reply_id {name}");
            }
            other => panic!("expect ไม่รู้จัก: {other}"),
        }
    }
}

#[test]
fn crypto() {
    let file = load("crypto.json");
    let (psk, hn, dn) = (arr(PSK), arr(HOST_NONCE), arr(DEV_NONCE));
    assert_eq!(file["psk"].as_str().unwrap(), PSK);
    assert_eq!(to_hex(&key_proof(&psk, &hn, &dn)), file["key_proof"].as_str().unwrap());
    let key = session_key(&psk, &hn, &dn);
    assert_eq!(to_hex(&key), file["session_key"].as_str().unwrap());

    for v in file["frames"].as_array().unwrap() {
        let role = match v["direction"].as_u64().unwrap() {
            0 => Role::Host,
            _ => Role::Device,
        };
        let mut session = Session::new(&key, role);
        for _ in 1..v["counter"].as_u64().unwrap() {
            session.seal(&[0x01], b"").unwrap();
        }
        let inner = session.seal(&[0x01], &hex(&v["plain_body"])).unwrap();
        assert_eq!(to_hex(&inner), v["inner"].as_str().unwrap(), "frame {}", v["name"]);
        if let Some(serial) = v.get("serial") {
            assert_eq!(encode_serial(&inner), hex(serial));
            assert_eq!(encode_tcp(&inner), hex(&v["tcp"]));
        }
    }

    let mut device = Session::new(&key, Role::Device);
    for v in file["device_receive_sequence"].as_array().unwrap() {
        let name = v["name"].as_str().unwrap();
        match (device.open(&hex(&v["inner"])), v.get("ok"), v["drop"].as_str()) {
            (Ok(body), Some(want), _) => assert_eq!(to_hex(&body), want.as_str().unwrap(), "{name}"),
            (Err(OpenError::Replay), None, Some("replay")) => {}
            (Err(OpenError::AuthFailed), None, Some("auth_failed")) => {}
            (got, _, _) => panic!("{name}: ได้ {got:?} ไม่ตรง vector"),
        }
    }
}

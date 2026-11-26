//! รัน sdp-check กับ sdp-sim ในโปรเซสเดียวกัน — ลำดับเฟส 0 ครบทั้งแบบไม่เข้ารหัสและแบบจับคู่แล้ว

use std::net::TcpListener;
use std::sync::mpsc::{self, Receiver, Sender};
use std::time::Duration;

use sdp_check::link::TcpLink;
use sdp_check::{run, CheckOptions, Connection, Line, PRESS_PROMPT};
use sdp_sim::{serve, Notice, Options};

const PSK: [u8; 16] = [7; 16];

struct Sim {
    addr: String,
    press: Sender<()>,
    notices: Receiver<Notice>,
}

fn start_sim(psk: Option<[u8; 16]>) -> Sim {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    let (press, buttons) = mpsc::channel();
    let (tell, notices) = mpsc::channel();
    let options = Options { device_id: [1, 2, 3, 4, 5, 6, 7, 8], heartbeat_ms: 100, psk };
    std::thread::spawn(move || {
        let _ = serve(listener, options, buttons, move |n| {
            let _ = tell.send(n);
        });
    });
    Sim { addr, press, notices }
}

fn check(sim: &Sim, psk: Option<[u8; 16]>) -> (bool, Vec<String>) {
    let mut conn = Connection::new(Box::new(TcpLink::connect(&sim.addr).unwrap()), psk);
    let options = CheckOptions { event_timeout: Duration::from_secs(5), skip_event: false, skip_link_lost: false };
    let mut lines = Vec::new();
    let ok = run(&mut conn, &options, &mut |line| match line {
        Line::Pass(t) => lines.push(format!("PASS {t}")),
        Line::Fail(t) => lines.push(format!("FAIL {t}")),
        Line::Info(t) => {
            if t.starts_with(PRESS_PROMPT) {
                sim.press.send(()).unwrap();
            }
            lines.push(format!("INFO {t}"));
        }
    });
    (ok, lines)
}

fn notices(sim: &Sim) -> Vec<Notice> {
    std::thread::sleep(Duration::from_millis(100));
    sim.notices.try_iter().collect()
}

#[test]
fn full_sequence_plaintext() {
    let sim = start_sim(None);
    let (ok, lines) = check(&sim, None);
    assert!(ok, "{lines:#?}");
    assert!(lines.iter().any(|l| l.contains("caps_hash a91e1219 matches")), "{lines:#?}");
    assert!(lines.iter().any(|l| l.starts_with("PASS event button.pressed {\"count\": 1}")), "{lines:#?}");
    let seen = notices(&sim);
    assert!(seen.contains(&Notice::Led(true)) && seen.contains(&Notice::Led(false)), "{seen:?}");
    assert!(seen.contains(&Notice::LinkLost), "sim must run on_link_lost when pings stop: {seen:?}");
}

#[test]
fn full_sequence_encrypted() {
    let sim = start_sim(Some(PSK));
    let (ok, lines) = check(&sim, Some(PSK));
    assert!(ok, "{lines:#?}");
    assert!(lines[0].contains("encrypted session"), "{lines:#?}");
    assert!(lines.iter().any(|line| line.starts_with("PASS firmware: project sdp-sim / build sim-")), "{lines:#?}");
    assert!(notices(&sim).contains(&Notice::LinkLost));
}

#[test]
fn wrong_psk_is_detected_by_key_proof() {
    let sim = start_sim(Some(PSK));
    let (ok, lines) = check(&sim, Some([8; 16]));
    assert!(!ok);
    assert!(lines.iter().any(|l| l.contains("key_proof does not match")), "{lines:#?}");
}

#[test]
fn unpaired_device_is_refused_when_psk_is_known() {
    let sim = start_sim(None);
    let (ok, lines) = check(&sim, Some(PSK));
    assert!(!ok);
    assert!(lines.iter().any(|l| l.contains("refusing plaintext")), "{lines:#?}");
}

#[test]
fn paired_device_refuses_plaintext_commands() {
    let sim = start_sim(Some(PSK));
    // host ที่ไม่มีกุญแจ: hello ได้ (ไม่เข้ารหัสเสมอ) แต่คำสั่งถัดไปต้องโดน auth_required
    let mut conn = Connection::new(Box::new(TcpLink::connect(&sim.addr).unwrap()), None);
    conn.hello().unwrap();
    match conn.request(sdp_core::Message::Ping).unwrap() {
        sdp_core::Message::Error(e) => assert_eq!(e.code, sdp_core::message::code::AUTH_REQUIRED),
        other => panic!("expected auth_required, got t={}", other.type_code()),
    }
}

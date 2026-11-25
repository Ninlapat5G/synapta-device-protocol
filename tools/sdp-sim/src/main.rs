//! sdp-sim [--listen 127.0.0.1:7878] [--psk HEX32] [--heartbeat-ms 1000]
//!
//! กด Enter = กดปุ่มของอุปกรณ์ (ส่ง event button.pressed)

use std::io::BufRead;
use std::net::TcpListener;
use std::process::ExitCode;
use std::sync::mpsc;

use sdp_sim::{serve, Notice, Options};

const USAGE: &str = "usage: sdp-sim [--listen ADDR:PORT] [--psk HEX32] [--heartbeat-ms N] [--device-id HEX16]";

fn parse_hex<const N: usize>(text: &str) -> Option<[u8; N]> {
    if text.len() != N * 2 {
        return None;
    }
    let mut out = [0u8; N];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(text.get(i * 2..i * 2 + 2)?, 16).ok()?;
    }
    Some(out)
}

fn main() -> ExitCode {
    let mut listen = String::from("127.0.0.1:7878");
    let mut options = Options { device_id: [0x5D, 0x51, 0x4D, 0, 0, 0, 0, 1], heartbeat_ms: 1000, psk: None };
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = args.next();
        let ok = match (arg.as_str(), value.as_deref()) {
            ("--listen", Some(v)) => {
                listen = v.to_string();
                true
            }
            ("--psk", Some(v)) => parse_hex::<16>(v).map(|k| options.psk = Some(k)).is_some(),
            ("--device-id", Some(v)) => parse_hex::<8>(v).map(|id| options.device_id = id).is_some(),
            ("--heartbeat-ms", Some(v)) => v.parse().map(|ms| options.heartbeat_ms = ms).is_ok(),
            _ => false,
        };
        if !ok {
            eprintln!("{USAGE}");
            return ExitCode::from(2);
        }
    }

    let listener = match TcpListener::bind(&listen) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("cannot listen on {listen}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let paired = if options.psk.is_some() { "paired (encrypted after hello)" } else { "not paired (plaintext)" };
    println!("sdp-sim listening on {listen} - {paired}, heartbeat {} ms", options.heartbeat_ms);
    println!("press Enter to press the device button");

    let (press, buttons) = mpsc::channel();
    std::thread::spawn(move || {
        for _ in std::io::stdin().lock().lines() {
            if press.send(()).is_err() {
                break;
            }
        }
    });

    let result = serve(listener, options, buttons, |notice| match notice {
        Notice::Connected(addr) => println!("host connected from {addr}"),
        Notice::Disconnected => println!("host disconnected - outputs off"),
        Notice::Led(on) => println!("LED {}", if on { "on" } else { "off" }),
        Notice::EventSent(name) => println!("event {name} sent"),
        Notice::LinkLost => println!("link lost (no message for 3 x heartbeat) - outputs off"),
        Notice::Dropped(reason) => println!("frame dropped: {reason}"),
    });
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("sdp-sim stopped: {e}");
            ExitCode::FAILURE
        }
    }
}

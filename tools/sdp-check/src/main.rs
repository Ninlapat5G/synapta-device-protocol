//! sdp-check --serial COM5 [--baud 115200] [--reset rts|dtr|none]
//! sdp-check --tcp 127.0.0.1:7878 [--psk HEX32]
//! ตัวเลือกร่วม: --event-timeout SEC, --skip-event, --skip-link-lost

use std::process::ExitCode;
use std::time::Duration;

use sdp_check::link::{Link, Reset, SerialLink, TcpLink};
use sdp_check::{run, CheckOptions, Connection, Line};

const USAGE: &str = "usage: sdp-check (--serial PORT [--baud N] [--reset rts|dtr|none] | --tcp HOST:PORT [--psk HEX32])
                 [--event-timeout SEC] [--skip-event] [--skip-link-lost]";

struct Args {
    serial: Option<String>,
    baud: u32,
    reset: Reset,
    tcp: Option<String>,
    psk: Option<[u8; 16]>,
    options: CheckOptions,
}

fn parse_psk(text: &str) -> Option<[u8; 16]> {
    if text.len() != 32 {
        return None;
    }
    let mut out = [0u8; 16];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(text.get(i * 2..i * 2 + 2)?, 16).ok()?;
    }
    Some(out)
}

fn parse_args() -> Option<Args> {
    let mut args = Args {
        serial: None,
        baud: 115_200,
        reset: Reset::Rts,
        tcp: None,
        psk: None,
        options: CheckOptions { event_timeout: Duration::from_secs(30), skip_event: false, skip_link_lost: false },
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--skip-event" => args.options.skip_event = true,
            "--skip-link-lost" => args.options.skip_link_lost = true,
            "--serial" => args.serial = Some(it.next()?),
            "--tcp" => args.tcp = Some(it.next()?),
            "--baud" => args.baud = it.next()?.parse().ok()?,
            "--psk" => args.psk = Some(parse_psk(&it.next()?)?),
            "--event-timeout" => args.options.event_timeout = Duration::from_secs(it.next()?.parse().ok()?),
            "--reset" => {
                args.reset = match it.next()?.as_str() {
                    "rts" => Reset::Rts,
                    "dtr" => Reset::Dtr,
                    "none" => Reset::None,
                    _ => return None,
                }
            }
            _ => return None,
        }
    }
    // PSK ใช้กับลิงก์ไร้สายเท่านั้น — USB ไม่เข้ารหัสตามสเปก
    let valid = args.serial.is_some() != args.tcp.is_some() && !(args.serial.is_some() && args.psk.is_some());
    valid.then_some(args)
}

fn main() -> ExitCode {
    let Some(args) = parse_args() else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    let link: Box<dyn Link> = match (&args.serial, &args.tcp) {
        (Some(port), _) => match SerialLink::open(port, args.baud, args.reset) {
            Ok(link) => {
                println!("… opened {port} at {} baud, waiting 2 s for the board to boot", args.baud);
                Box::new(link)
            }
            Err(e) => {
                eprintln!("cannot open {port}: {e}");
                return ExitCode::FAILURE;
            }
        },
        (_, Some(addr)) => match TcpLink::connect(addr) {
            Ok(link) => Box::new(link),
            Err(e) => {
                eprintln!("cannot connect to {addr}: {e}");
                return ExitCode::FAILURE;
            }
        },
        _ => unreachable!("parse_args checks exactly one link"),
    };

    let mut conn = Connection::new(link, args.psk);
    if args.serial.is_some() {
        // บอร์ดเพิ่งรีเซ็ต: รอบูตเสร็จ ข้อความบูตเก็บเป็น log
        if let Err(e) = conn.pump(Duration::from_secs(2)) {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    }
    let ok = run(&mut conn, &args.options, &mut |line| match line {
        Line::Pass(t) => println!("✓ {t}"),
        Line::Fail(t) => println!("✗ {t}"),
        Line::Info(t) => println!("… {t}"),
    });
    println!("{}", if ok { "PASS" } else { "FAIL" });
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

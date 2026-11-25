//! รับ host ทาง TCP ทีละราย: เฟรม `len(2) | flags | body`, เข้ารหัสเมื่อจับคู่แล้ว, จับเวลา heartbeat
//!
//! ทำตัวเหมือนอุปกรณ์ WiFi ตามสเปก — hello/hello_ack ไม่เข้ารหัสเสมอ, ข้อความอื่นต้องเข้ารหัสถ้ามี PSK

use std::io::{self, ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use sdp_core::crypto::{self, Role, Session};
use sdp_core::frame::{encode_tcp, FLAG_ENCRYPTED};
use sdp_core::message::{code, ErrorMsg, PairProof};
use sdp_core::{Envelope, Message};

use crate::device::{Device, MAX_FRAME};

pub struct Options {
    pub device_id: [u8; 8],
    pub heartbeat_ms: u32,
    /// มี = จับคู่ผ่าน USB มาแล้ว (จำลอง) → ทุกข้อความหลัง hello ต้องเข้ารหัส
    pub psk: Option<[u8; 16]>,
}

/// สิ่งที่เกิดขึ้นในอุปกรณ์ — main.rs พิมพ์ให้คนดู, เทสต์ใช้ตรวจ
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Notice {
    Connected(SocketAddr),
    Disconnected,
    Led(bool),
    EventSent(String),
    LinkLost,
    Dropped(&'static str),
}

const FLAGS_RESERVED: u8 = 0xFC;

pub fn serve(
    listener: TcpListener,
    options: Options,
    buttons: Receiver<()>,
    mut notify: impl FnMut(Notice),
) -> io::Result<()> {
    let mut boot_id = [0u8; 4];
    getrandom::getrandom(&mut boot_id).map_err(io::Error::other)?;
    let mut device = Device::new(options.device_id, boot_id, options.heartbeat_ms);
    for stream in listener.incoming() {
        let stream = stream?;
        notify(Notice::Connected(stream.peer_addr()?));
        let result = Connection::new(stream, &options, &mut device, &buttons).run(&mut notify);
        device.link_lost(); // TCP ปิด = host หายแน่นอน ไม่ต้องรอ heartbeat
        notify(Notice::Disconnected);
        if let Err(e) = result {
            if e.kind() != ErrorKind::ConnectionReset && e.kind() != ErrorKind::ConnectionAborted {
                return Err(e);
            }
        }
    }
    Ok(())
}

struct Connection<'a> {
    stream: TcpStream,
    options: &'a Options,
    device: &'a mut Device,
    buttons: &'a Receiver<()>,
    session: Option<Session>,
    linked: bool,
    last_heard: Instant,
}

impl<'a> Connection<'a> {
    fn new(stream: TcpStream, options: &'a Options, device: &'a mut Device, buttons: &'a Receiver<()>) -> Self {
        Connection { stream, options, device, buttons, session: None, linked: false, last_heard: Instant::now() }
    }

    fn run(&mut self, notify: &mut impl FnMut(Notice)) -> io::Result<()> {
        self.stream.set_read_timeout(Some(Duration::from_millis(20)))?;
        self.stream.set_nodelay(true)?;
        let mut pending: Vec<u8> = Vec::new();
        let mut chunk = [0u8; 1024];
        loop {
            match self.stream.read(&mut chunk) {
                Ok(0) => return Ok(()),
                Ok(n) => pending.extend_from_slice(&chunk[..n]),
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                Err(e) => return Err(e),
            }
            while pending.len() >= 2 {
                let len = usize::from(u16::from_be_bytes([pending[0], pending[1]]));
                if pending.len() < 2 + len {
                    break;
                }
                let inner: Vec<u8> = pending.drain(..2 + len).skip(2).collect();
                self.on_frame(&inner, notify)?;
            }
            if self.buttons.try_recv().is_ok() {
                let event = self.device.press_button();
                if self.linked {
                    let name = match &event {
                        Message::Event { name, .. } => name.clone(),
                        _ => unreachable!(),
                    };
                    self.send(Envelope::new(0, event), self.session.is_some())?;
                    notify(Notice::EventSent(name));
                }
            }
            let limit = Duration::from_millis(3 * u64::from(self.options.heartbeat_ms));
            if self.linked && self.last_heard.elapsed() > limit {
                self.linked = false;
                self.device.link_lost();
                notify(Notice::LinkLost);
            }
        }
    }

    fn on_frame(&mut self, inner: &[u8], notify: &mut impl FnMut(Notice)) -> io::Result<()> {
        if inner.is_empty() || inner[0] & FLAGS_RESERVED != 0 {
            notify(Notice::Dropped("reserved_flag"));
            return Ok(());
        }
        if inner.len() > MAX_FRAME {
            return self.send(Envelope::new(0, error(code::FRAME_TOO_LARGE)), false);
        }
        let encrypted = inner[0] & FLAG_ENCRYPTED != 0;
        let body = if encrypted {
            let Some(session) = self.session.as_mut() else {
                return self.send(Envelope::new(0, error(code::AUTH_REQUIRED)), false);
            };
            match session.open(inner) {
                Ok(body) => body,
                Err(_) => {
                    notify(Notice::Dropped("decrypt_failed")); // ปลอม/เล่นซ้ำ — เงียบไว้ ไม่บอกผู้โจมตี
                    return Ok(());
                }
            }
        } else {
            inner[1..].to_vec()
        };
        self.last_heard = Instant::now();
        self.linked = true;

        let env = match Envelope::decode(&body) {
            Ok(env) => env,
            Err(e) => return self.send(Envelope::new(e.reply_id(), error(e.code())), encrypted),
        };
        if let Message::Hello(hello) = &env.msg {
            let pairing = self.options.psk.map(|psk| self.open_session(&psk, &hello.host_nonce));
            let reply = self.device.hello(hello, pairing);
            return self.send(Envelope::new(env.id, reply), false);
        }
        if self.options.psk.is_some() && !encrypted {
            return self.send(Envelope::new(env.id, error(code::AUTH_REQUIRED)), false);
        }
        let was_on = self.device.led_on();
        let reply = self.device.handle(env.msg, true);
        if self.device.led_on() != was_on {
            notify(Notice::Led(self.device.led_on()));
        }
        self.send(Envelope::new(env.id, reply), encrypted)
    }

    /// session ใหม่ทุก hello — dev_nonce สุ่มใหม่ ได้ session key ใหม่
    fn open_session(&mut self, psk: &[u8; 16], host_nonce: &[u8; 8]) -> PairProof {
        let mut dev_nonce = [0u8; 8];
        getrandom::getrandom(&mut dev_nonce).expect("ระบบสุ่มของ OS ใช้ไม่ได้");
        let key = crypto::session_key(psk, host_nonce, &dev_nonce);
        self.session = Some(Session::new(&key, Role::Device));
        PairProof { dev_nonce, key_proof: crypto::key_proof(psk, host_nonce, &dev_nonce) }
    }

    fn send(&mut self, env: Envelope, encrypt: bool) -> io::Result<()> {
        let body = env.encode();
        let inner = match (encrypt, self.session.as_mut()) {
            (true, Some(session)) => {
                session.seal(&[FLAG_ENCRYPTED], &body).map_err(|_| io::Error::other("counter หมด"))?
            }
            _ => [&[0u8][..], &body].concat(),
        };
        self.stream.write_all(&encode_tcp(&inner))
    }
}

fn error(code: u16) -> Message {
    Message::Error(ErrorMsg { code, msg: None, detail: None })
}

//! คุย SDP บน Link หนึ่งเส้น: จับคู่คำขอกับคำตอบด้วย id, เก็บ event/log ที่มาระหว่างรอ, เข้ารหัสเมื่อมี PSK

use std::collections::VecDeque;
use std::fmt;
use std::time::{Duration, Instant};

use sdp_core::crypto::{self, Role, Session};
use sdp_core::frame::FLAG_ENCRYPTED;
use sdp_core::message::{Hello, HelloAck};
use sdp_core::{Envelope, Message, Value};

use crate::link::{Incoming, Link};

#[derive(Debug)]
pub struct CheckError(pub String);

impl fmt::Display for CheckError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl From<std::io::Error> for CheckError {
    fn from(e: std::io::Error) -> Self {
        CheckError(format!("link error: {e}"))
    }
}

fn fail<T>(msg: impl Into<String>) -> Result<T, CheckError> {
    Err(CheckError(msg.into()))
}

pub struct Event {
    pub name: String,
    pub data: Option<Value>,
}

pub struct Connection {
    link: Box<dyn Link>,
    psk: Option<[u8; 16]>,
    session: Option<Session>,
    next_id: u16,
    pub events: VecDeque<Event>,
    pub logs: Vec<String>,
    pub reply_timeout: Duration,
}

impl Connection {
    pub fn new(link: Box<dyn Link>, psk: Option<[u8; 16]>) -> Self {
        Connection {
            link,
            psk,
            session: None,
            next_id: 1,
            events: VecDeque::new(),
            logs: Vec::new(),
            reply_timeout: Duration::from_secs(2),
        }
    }

    pub fn is_wireless(&self) -> bool {
        self.link.is_wireless()
    }

    /// hello → hello_ack และเปิด session เข้ารหัสถ้ามี PSK (wire-format.md §เปิด session)
    pub fn hello(&mut self) -> Result<HelloAck, CheckError> {
        let mut host_nonce = [0u8; 8];
        getrandom::getrandom(&mut host_nonce).map_err(|e| CheckError(format!("no OS randomness: {e}")))?;
        self.session = None;
        let hello = Message::Hello(Hello { sdp_min: 1, sdp_max: 1, host: "sdp-check".into(), host_nonce });
        let ack = match self.request(hello)? {
            Message::HelloAck(ack) => ack,
            Message::Error(e) => return fail(format!("hello refused: error {} {}", e.code, e.msg.unwrap_or_default())),
            other => return fail(format!("expected hello_ack, got t={}", other.type_code())),
        };
        match (&self.psk, &ack.pairing) {
            (Some(psk), Some(p)) => {
                let expected = crypto::key_proof(psk, &host_nonce, &p.dev_nonce);
                if !crypto::proof_matches(&expected, &p.key_proof) {
                    return fail("key_proof does not match the PSK - wrong key or impostor device");
                }
                let key = crypto::session_key(psk, &host_nonce, &p.dev_nonce);
                self.session = Some(Session::new(&key, Role::Host));
            }
            // กันการถอยไปไม่เข้ารหัส: รู้ PSK แล้วแต่อุปกรณ์บอกว่ายังไม่จับคู่
            (Some(_), None) => return fail("device says it is not paired, but a PSK was given - refusing plaintext"),
            (None, _) => {}
        }
        Ok(ack)
    }

    pub fn request(&mut self, msg: Message) -> Result<Message, CheckError> {
        let id = self.next_id;
        self.next_id = if self.next_id == u16::MAX { 1 } else { self.next_id + 1 };
        let body = Envelope::new(id, msg).encode();
        let inner = match self.session.as_mut() {
            Some(s) => s.seal(&[FLAG_ENCRYPTED], &body).map_err(|_| CheckError("counter exhausted".into()))?,
            None => [&[0u8][..], &body].concat(),
        };
        self.link.send(&inner)?;

        let deadline = Instant::now() + self.reply_timeout;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return fail(format!("no reply to request id {id} within {} ms", self.reply_timeout.as_millis()));
            }
            if let Some(env) = self.receive(left)? {
                // id 0 = error ของเฟรมที่อุปกรณ์อ่าน id ไม่ได้ — ถือเป็นคำตอบของคำขอที่ค้าง (ส่งทีละหนึ่ง)
                if env.id == id || (env.id == 0 && matches!(env.msg, Message::Error(_))) {
                    return Ok(env.msg);
                }
            }
        }
    }

    /// รับ event / log ไปเรื่อย ๆ จนครบเวลา
    pub fn pump(&mut self, duration: Duration) -> Result<(), CheckError> {
        let deadline = Instant::now() + duration;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Ok(());
            }
            self.receive(left)?;
        }
    }

    /// รับหนึ่งอย่าง: event เก็บเข้าคิว, log เก็บไว้, ข้อความอื่นคืนให้ผู้เรียก
    fn receive(&mut self, timeout: Duration) -> Result<Option<Envelope>, CheckError> {
        let inner = match self.link.recv(timeout)? {
            None => return Ok(None),
            Some(Incoming::Log(text)) => {
                if !text.trim().is_empty() {
                    self.logs.push(text);
                }
                return Ok(None);
            }
            Some(Incoming::Frame(inner)) => inner,
        };
        let body = if inner[0] & FLAG_ENCRYPTED != 0 {
            let Some(session) = self.session.as_mut() else {
                return fail("device sent an encrypted frame before a session was opened");
            };
            session.open(&inner).map_err(|e| CheckError(format!("cannot decrypt device frame: {e:?}")))?
        } else {
            inner[1..].to_vec()
        };
        let env = Envelope::decode(&body).map_err(|e| CheckError(format!("device sent a malformed body: {e:?}")))?;
        if self.session.is_some() && inner[0] & FLAG_ENCRYPTED == 0 && !matches!(env.msg, Message::HelloAck(_)) {
            return fail("device sent plaintext inside an encrypted session");
        }
        if let Message::Event { name, data } = env.msg {
            self.events.push_back(Event { name, data });
            return Ok(None);
        }
        Ok(Some(env))
    }
}

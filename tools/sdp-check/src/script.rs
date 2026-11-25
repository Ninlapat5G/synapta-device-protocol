//! ลำดับตรวจของเฟส 0 (roadmap.md): hello → describe → call → event → ping หาย → on_link_lost

use std::time::{Duration, Instant};

use sdp_core::descriptor::{caps_hash, Capability, EventInfo};
use sdp_core::message::code;
use sdp_core::{Message, Value};

use crate::conn::{CheckError, Connection};
use crate::show::{hex, show};

pub struct CheckOptions {
    pub event_timeout: Duration,
    pub skip_event: bool,
    pub skip_link_lost: bool,
}

pub enum Line {
    Pass(String),
    Fail(String),
    /// บอกว่ากำลังรออะไร — ถ้าเป็นการขอให้กดปุ่ม ข้อความขึ้นต้นด้วย PRESS_PROMPT
    Info(String),
}

pub const PRESS_PROMPT: &str = "press the board's button";

struct Run<'a> {
    conn: &'a mut Connection,
    out: &'a mut dyn FnMut(Line),
    failed: bool,
}

impl Run<'_> {
    fn pass(&mut self, text: String) {
        (self.out)(Line::Pass(text));
    }
    fn fail(&mut self, text: String) {
        self.failed = true;
        (self.out)(Line::Fail(text));
    }
    fn info(&mut self, text: String) {
        (self.out)(Line::Info(text));
    }
}

/// คืน true ถ้าผ่านทุกข้อ — ข้อที่พังตั้งแต่ hello/describe จะหยุดเลย เพราะข้อถัดไปพึ่งผลของมัน
pub fn run(conn: &mut Connection, options: &CheckOptions, out: &mut dyn FnMut(Line)) -> bool {
    let mut run = Run { conn, out, failed: false };
    if let Err(e) = steps(&mut run, options) {
        run.fail(e.0);
    }
    for log in std::mem::take(&mut run.conn.logs) {
        run.info(format!("device log: {}", log.trim_end()));
    }
    !run.failed
}

fn steps(run: &mut Run, options: &CheckOptions) -> Result<(), CheckError> {
    let ack = run.conn.hello()?;
    let heartbeat = Duration::from_millis(u64::from(ack.heartbeat_ms.max(100)));
    run.pass(format!(
        "hello: {} / {}, device_id {}, SDP v{}, max_frame {}, heartbeat {} ms{}",
        ack.model,
        ack.fw,
        hex(&ack.device_id),
        ack.version,
        ack.max_frame,
        ack.heartbeat_ms,
        if ack.pairing.is_some() { ", encrypted session" } else { "" }
    ));

    let (caps, events) = describe(run)?;
    let hash = caps_hash([(&caps[..], &events[..])]);
    let names: Vec<&str> = caps.iter().map(|c| c.name.as_str()).chain(events.iter().map(|e| e.name.as_str())).collect();
    let summary = format!("describe: {} capabilities, {} events ({})", caps.len(), events.len(), names.join(", "));
    if hash == ack.caps_hash {
        run.pass(format!("{summary}, caps_hash {:08x} matches", hash));
    } else {
        // ถ้าอุปกรณ์ส่ง key ที่ตัวนี้ไม่รู้จัก hash ที่คิดใหม่จะต่างได้ — แต่ firmware ของเราไม่ส่ง key แบบนั้น
        run.fail(format!("{summary}, but caps_hash {:08x} != hello_ack {:08x}", hash, ack.caps_hash));
    }

    call_known(run, &caps)?;
    match run.conn.request(Message::Call { name: "sdp-check.no-such-capability".into(), input: None })? {
        Message::Error(e) if e.code == code::UNKNOWN_CAPABILITY => {
            run.pass(format!("unknown capability -> error 2 ({})", e.msg.unwrap_or_default()))
        }
        other => run.fail(format!("unknown capability should give error 2, got {}", describe_msg(&other))),
    }

    match run.conn.request(Message::Ping)? {
        Message::Pong { uptime_ms, boot_id } if boot_id == ack.boot_id => {
            run.pass(format!("ping -> pong, uptime {uptime_ms} ms, boot_id {}", hex(&boot_id)))
        }
        other => run.fail(format!("ping should give pong with the same boot_id, got {}", describe_msg(&other))),
    }

    if !events.is_empty() && !options.skip_event {
        wait_for_event(run, options.event_timeout, heartbeat)?;
    }
    if !options.skip_link_lost {
        link_lost(run, heartbeat, &ack.boot_id)?;
    }
    Ok(())
}

fn describe(run: &mut Run) -> Result<(Vec<Capability>, Vec<EventInfo>), CheckError> {
    let (mut caps, mut events, mut page) = (Vec::new(), Vec::new(), 0);
    loop {
        match run.conn.request(Message::Describe { page })? {
            Message::DescribeR(r) => {
                caps.extend(r.caps);
                events.extend(r.events);
                match r.next_page {
                    Some(next) if next > page => page = next,
                    Some(next) => return Err(CheckError(format!("describe page {page} points back to page {next}"))),
                    None => return Ok((caps, events)),
                }
            }
            other => return Err(CheckError(format!("describe page {page} failed: {}", describe_msg(&other)))),
        }
    }
}

/// สั่งงาน capability ของ LedDemo ถ้ามี — อุปกรณ์อื่นแค่แสดงรายชื่อ
fn call_known(run: &mut Run, caps: &[Capability]) -> Result<(), CheckError> {
    let has = |name: &str| caps.iter().any(|c| c.name == name);
    if has("led.set") {
        for on in [true, false] {
            let input = Value::Map(vec![(Value::text("on"), Value::Bool(on))]);
            match run.conn.request(Message::Call { name: "led.set".into(), input: Some(input) })? {
                Message::Result { output } => run.pass(format!(
                    "call led.set {{\"on\": {on}}} -> {}",
                    output.as_ref().map(show).unwrap_or_default()
                )),
                other => run.fail(format!("call led.set failed: {}", describe_msg(&other))),
            }
        }
    }
    if has("temp.read") {
        match run.conn.request(Message::Call { name: "temp.read".into(), input: None })? {
            Message::Result { output } => {
                run.pass(format!("call temp.read -> {}", output.as_ref().map(show).unwrap_or_default()))
            }
            other => run.fail(format!("call temp.read failed: {}", describe_msg(&other))),
        }
    }
    Ok(())
}

/// รอ event ระหว่างนั้นส่ง ping ตามจังหวะ heartbeat ให้อุปกรณ์รู้ว่ายังต่ออยู่
fn wait_for_event(run: &mut Run, timeout: Duration, heartbeat: Duration) -> Result<(), CheckError> {
    run.info(format!("{PRESS_PROMPT} within {} s (pinging every {} ms)", timeout.as_secs(), heartbeat.as_millis()));
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Some(event) = run.conn.events.pop_front() {
            let data = event.data.as_ref().map(show).unwrap_or_default();
            run.pass(format!("event {} {data}", event.name));
            return Ok(());
        }
        run.conn.request(Message::Ping)?;
        run.conn.pump(heartbeat.min(deadline.saturating_duration_since(Instant::now())))?;
    }
    run.fail(format!("no event within {} s", timeout.as_secs()));
    Ok(())
}

/// หยุด ping เกิน 3 × heartbeat — firmware ต้องเรียก onLinkLost แล้วยังตอบ hello ใหม่ได้โดยไม่รีเซ็ต
fn link_lost(run: &mut Run, heartbeat: Duration, boot_id: &[u8; 4]) -> Result<(), CheckError> {
    let wait = heartbeat * 3 + Duration::from_millis(1000);
    run.info(format!("stopping pings for {} ms to trigger on_link_lost", wait.as_millis()));
    run.conn.pump(wait)?;
    if run.conn.is_wireless() {
        run.info("on_link_lost is not visible over TCP - check the device console".into());
    } else if let Some(line) = run.conn.logs.iter().find(|l| l.contains("link lost")) {
        let line = line.trim_end().to_string();
        run.pass(format!("device reported link loss: {line:?}"));
    } else {
        run.fail("device did not report link loss (expected a 'SDP: link lost' log line)".into());
    }
    let ack = run.conn.hello()?;
    if &ack.boot_id == boot_id {
        run.pass("device answers hello again after link loss (no reset)".into());
    } else {
        run.fail("boot_id changed - the board reset instead of handling link loss".into());
    }
    Ok(())
}

fn describe_msg(m: &Message) -> String {
    match m {
        Message::Error(e) => format!("error {} {}", e.code, e.msg.clone().unwrap_or_default()),
        other => format!("message t={}", other.type_code()),
    }
}

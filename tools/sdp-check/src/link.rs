//! ช่องทางรับส่ง inner (`flags | body`) กับอุปกรณ์: USB serial (COBS + CRC) หรือ TCP (ความยาว 2 ไบต์)

use std::collections::VecDeque;
use std::io::{self, ErrorKind, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::{Duration, Instant};

use sdp_core::frame::{encode_serial, encode_tcp, SerialEvent, SerialReceiver};
use sdp_core::HOST_RX_LIMIT;

pub enum Incoming {
    Frame(Vec<u8>),
    /// ข้อความนอกเฟรม (Serial.print ของ firmware, boot log ของ ESP32) หรือเฟรมเสีย
    Log(String),
}

pub trait Link {
    fn send(&mut self, inner: &[u8]) -> io::Result<()>;
    /// รอได้ไม่เกิน `timeout` — ไม่มีอะไรมา = None
    fn recv(&mut self, timeout: Duration) -> io::Result<Option<Incoming>>;
    fn is_wireless(&self) -> bool;
}

/// วิธีรีเซ็ตบอร์ดตอนเปิดพอร์ต — ESP32 ใช้ RTS (ต่อขา EN), Uno ใช้ DTR (ต่อขา RESET ผ่านตัวเก็บประจุ)
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Reset {
    Rts,
    Dtr,
    None,
}

pub struct SerialLink {
    port: Box<dyn serialport::SerialPort>,
    receiver: SerialReceiver,
    queue: VecDeque<Incoming>,
}

impl SerialLink {
    pub fn open(name: &str, baud: u32, reset: Reset) -> io::Result<SerialLink> {
        let mut port =
            serialport::new(name, baud).timeout(Duration::from_millis(20)).open().map_err(io::Error::other)?;
        // ปล่อย DTR ไว้เสมอบน ESP32: DTR ต่อขา IO0 ซึ่ง LedDemo ใช้เป็นปุ่ม — กดค้างจะกลายเป็น event ปลอม
        port.write_data_terminal_ready(false).map_err(io::Error::other)?;
        port.write_request_to_send(false).map_err(io::Error::other)?;
        let pulse = |port: &mut Box<dyn serialport::SerialPort>, dtr: bool| -> serialport::Result<()> {
            if dtr {
                port.write_data_terminal_ready(true)?;
            } else {
                port.write_request_to_send(true)?;
            }
            std::thread::sleep(Duration::from_millis(100));
            port.write_data_terminal_ready(false)?;
            port.write_request_to_send(false)
        };
        match reset {
            Reset::Rts => pulse(&mut port, false).map_err(io::Error::other)?,
            Reset::Dtr => pulse(&mut port, true).map_err(io::Error::other)?,
            Reset::None => {}
        }
        Ok(SerialLink { port, receiver: SerialReceiver::new(HOST_RX_LIMIT), queue: VecDeque::new() })
    }
}

impl Link for SerialLink {
    fn send(&mut self, inner: &[u8]) -> io::Result<()> {
        self.port.write_all(&encode_serial(inner))?;
        self.port.flush()
    }

    fn recv(&mut self, timeout: Duration) -> io::Result<Option<Incoming>> {
        let deadline = Instant::now() + timeout;
        let mut buf = [0u8; 256];
        loop {
            if let Some(item) = self.queue.pop_front() {
                return Ok(Some(item));
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
            let n = match self.port.read(&mut buf) {
                Ok(n) => n,
                Err(e) if e.kind() == ErrorKind::TimedOut => 0,
                Err(e) => return Err(e),
            };
            for &byte in &buf[..n] {
                match self.receiver.push(byte) {
                    Some(SerialEvent::Frame(inner)) => self.queue.push_back(Incoming::Frame(inner)),
                    Some(SerialEvent::Drop { raw, .. }) => {
                        self.queue.push_back(Incoming::Log(String::from_utf8_lossy(&raw).into_owned()));
                    }
                    None => {}
                }
            }
        }
    }

    fn is_wireless(&self) -> bool {
        false
    }
}

pub struct TcpLink {
    stream: TcpStream,
    pending: Vec<u8>,
}

impl TcpLink {
    pub fn connect(addr: &str) -> io::Result<TcpLink> {
        let target = addr.to_socket_addrs()?.next().ok_or_else(|| io::Error::other("address not found"))?;
        let stream = TcpStream::connect_timeout(&target, Duration::from_secs(3))?;
        stream.set_nodelay(true)?;
        Ok(TcpLink { stream, pending: Vec::new() })
    }
}

impl Link for TcpLink {
    fn send(&mut self, inner: &[u8]) -> io::Result<()> {
        self.stream.write_all(&encode_tcp(inner))
    }

    fn recv(&mut self, timeout: Duration) -> io::Result<Option<Incoming>> {
        let deadline = Instant::now() + timeout;
        let mut buf = [0u8; 1024];
        loop {
            if self.pending.len() >= 2 {
                let len = usize::from(u16::from_be_bytes([self.pending[0], self.pending[1]]));
                if self.pending.len() >= 2 + len {
                    return Ok(Some(Incoming::Frame(self.pending.drain(..2 + len).skip(2).collect())));
                }
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Ok(None);
            }
            self.stream.set_read_timeout(Some(left.max(Duration::from_millis(1))))?;
            match self.stream.read(&mut buf) {
                Ok(0) => return Err(io::Error::new(ErrorKind::UnexpectedEof, "device closed the connection")),
                Ok(n) => self.pending.extend_from_slice(&buf[..n]),
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => return Ok(None),
                Err(e) => return Err(e),
            }
        }
    }

    fn is_wireless(&self) -> bool {
        true
    }
}

//! เข้ารหัสลิงก์ไร้สาย (decisions #12, wire-format.md §การเข้ารหัส)
//!
//! PSK จากการจับคู่ผ่าน USB → session key ใหม่ทุกครั้งจาก nonce สองฝั่ง → AES-128-CCM + counter กันเล่นซ้ำ

use alloc::vec::Vec;

use ccm::aead::generic_array::GenericArray;
use ccm::aead::{AeadInPlace, KeyInit};
use ccm::consts::{U13, U8};
use hmac::{Hmac, Mac};
use sha2::Sha256;

use crate::frame::{FLAG_ADDR, FLAG_ENCRYPTED};

type AesCcm = ccm::Ccm<aes::Aes128, U8, U13>;

const COUNTER_LEN: usize = 4;
const TAG_LEN: usize = 8;

fn hmac_sha256(key: &[u8], label: &[u8], host_nonce: &[u8; 8], dev_nonce: &[u8; 8]) -> [u8; 32] {
    let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC รับกุญแจได้ทุกความยาว");
    mac.update(label);
    mac.update(host_nonce);
    mac.update(dev_nonce);
    mac.finalize().into_bytes().into()
}

/// อุปกรณ์ส่งค่านี้ใน `hello_ack` ให้ host รู้ว่ามี PSK จริง
pub fn key_proof(psk: &[u8; 16], host_nonce: &[u8; 8], dev_nonce: &[u8; 8]) -> [u8; 8] {
    let full = hmac_sha256(psk, b"proof", host_nonce, dev_nonce);
    full[..8].try_into().unwrap()
}

pub fn session_key(psk: &[u8; 16], host_nonce: &[u8; 8], dev_nonce: &[u8; 8]) -> [u8; 16] {
    let full = hmac_sha256(psk, b"key", host_nonce, dev_nonce);
    full[..16].try_into().unwrap()
}

/// เทียบ key_proof แบบใช้เวลาเท่ากันทุกกรณี — กันการเดาทีละ byte จากเวลาที่ตอบ
pub fn proof_matches(expected: &[u8; 8], received: &[u8; 8]) -> bool {
    expected.iter().zip(received).fold(0u8, |acc, (a, b)| acc | (a ^ b)) == 0
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Host,
    Device,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpenError {
    /// เฟรมสั้นกว่า header + counter + tag
    TooShort,
    /// `flags` bit 0 ไม่ได้ตั้ง — ทางไร้สายกับอุปกรณ์ที่จับคู่แล้ว = ตอบ `auth_required`
    NotEncrypted,
    Replay,
    AuthFailed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CounterExhausted;

/// สถานะการเข้ารหัสของหนึ่ง session ฝั่งใดฝั่งหนึ่ง
pub struct Session {
    cipher: AesCcm,
    send_dir: u8,
    recv_dir: u8,
    last_sent: u32,
    last_received: u32,
}

impl Session {
    pub fn new(session_key: &[u8; 16], role: Role) -> Self {
        let (send_dir, recv_dir) = match role {
            Role::Host => (0, 1),
            Role::Device => (1, 0),
        };
        Session {
            cipher: AesCcm::new(GenericArray::from_slice(session_key)),
            send_dir,
            recv_dir,
            last_sent: 0,
            last_received: 0,
        }
    }

    /// `header` = `flags | [addr]` (ต้องตั้ง bit 0) → คืน inner = `header | counter | ciphertext | tag`
    pub fn seal(&mut self, header: &[u8], body: &[u8]) -> Result<Vec<u8>, CounterExhausted> {
        debug_assert!(header[0] & FLAG_ENCRYPTED != 0, "header ของเฟรมเข้ารหัสต้องตั้ง flags bit 0");
        let counter = self.last_sent.checked_add(1).ok_or(CounterExhausted)?;
        self.last_sent = counter;

        let mut out = Vec::with_capacity(header.len() + COUNTER_LEN + body.len() + TAG_LEN);
        out.extend_from_slice(header);
        out.extend_from_slice(&counter.to_be_bytes());
        let start = out.len();
        out.extend_from_slice(body);
        let tag = self
            .cipher
            .encrypt_in_place_detached(&nonce(self.send_dir, counter), header, &mut out[start..])
            .expect("CCM เข้ารหัสได้เสมอเมื่อ body ไม่เกิน 64 KB");
        out.extend_from_slice(&tag);
        Ok(out)
    }

    /// แกะ inner ที่เข้ารหัส คืน body
    pub fn open(&mut self, inner: &[u8]) -> Result<Vec<u8>, OpenError> {
        let flags = *inner.first().ok_or(OpenError::TooShort)?;
        if flags & FLAG_ENCRYPTED == 0 {
            return Err(OpenError::NotEncrypted);
        }
        let header_len = if flags & FLAG_ADDR != 0 { 2 } else { 1 };
        if inner.len() < header_len + COUNTER_LEN + TAG_LEN {
            return Err(OpenError::TooShort);
        }
        let (header, rest) = inner.split_at(header_len);
        let (counter, rest) = rest.split_at(COUNTER_LEN);
        let (ciphertext, tag) = rest.split_at(rest.len() - TAG_LEN);
        let counter = u32::from_be_bytes(counter.try_into().unwrap());

        if counter <= self.last_received {
            return Err(OpenError::Replay);
        }
        let mut body = ciphertext.to_vec();
        self.cipher
            .decrypt_in_place_detached(&nonce(self.recv_dir, counter), header, &mut body, GenericArray::from_slice(tag))
            .map_err(|_| OpenError::AuthFailed)?;
        // ขยับหลังถอดผ่านเท่านั้น — ไม่งั้นเฟรมปลอม counter สูง ๆ ทำให้เฟรมจริงที่ตามมาถูกทิ้งเป็น replay ทั้งหมด
        self.last_received = counter;
        Ok(body)
    }
}

fn nonce(direction: u8, counter: u32) -> GenericArray<u8, U13> {
    let mut n = [0u8; 13];
    n[0] = direction;
    n[1..5].copy_from_slice(&counter.to_be_bytes());
    GenericArray::from(n)
}

//! COBS: ทำให้ข้อมูลไม่มี byte 0x00 เพื่อใช้ 0x00 เป็นตัวคั่นเฟรมบน serial
//!
//! ตัวถอดแบบทีละ byte อยู่ใน `frame::SerialReceiver` เพราะต้องรู้ขีดความยาวของผู้รับ

use alloc::vec::Vec;

/// COBS มาตรฐาน — บล็อกเต็ม 254 ไบต์ที่จบพอดีท้ายข้อมูล ไม่เติมรหัส 0x01 ต่อท้าย
/// (ตัวเข้ารหัสบางตัวเติม ถอดได้ผลเหมือนกันแต่ byte ไม่ตรง vector)
pub fn encode(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + data.len() / 254 + 2);
    let mut code_at = 0;
    out.push(0); // ที่ว่างไว้ใส่รหัสของบล็อกแรก
    let mut ended_on_full_block = false;

    for &byte in data {
        ended_on_full_block = false;
        if byte == 0 {
            out[code_at] = (out.len() - code_at) as u8;
            code_at = out.len();
            out.push(0);
        } else {
            out.push(byte);
            if out.len() - code_at == 255 {
                out[code_at] = 0xFF;
                code_at = out.len();
                out.push(0);
                ended_on_full_block = true;
            }
        }
    }

    if ended_on_full_block {
        out.pop(); // ไม่มีข้อมูลตามหลังบล็อกเต็ม ไม่ต้องมีรหัสบล็อกว่าง
    } else {
        out[code_at] = (out.len() - code_at) as u8;
    }
    out
}

/// ถอดท่อนที่ไม่มี 0x00 ทั้งท่อน คืน `None` ถ้ารหัสชี้เลยท้ายท่อน
pub fn decode(chunk: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(chunk.len());
    let mut i = 0;
    while i < chunk.len() {
        let code = chunk[i] as usize;
        if code == 0 || i + code > chunk.len() {
            return None;
        }
        let block = &chunk[i + 1..i + code];
        if block.contains(&0) {
            return None;
        }
        out.extend_from_slice(block);
        i += code;
        if code != 0xFF && i < chunk.len() {
            out.push(0);
        }
    }
    Some(out)
}

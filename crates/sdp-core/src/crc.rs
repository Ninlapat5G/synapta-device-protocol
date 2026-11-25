//! CRC ที่สเปกใช้: CRC-16 ของเฟรม serial และ CRC-32 ของ `caps_hash`
//!
//! คำนวณทีละบิตแทนตาราง — เฟรมสั้น และไม่เปลือง flash ถ้าวันหนึ่งใช้บน MCU

/// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, ไม่กลับบิต, ไม่ XOR ท้าย
pub fn crc16_ccitt_false(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &byte in data {
        crc ^= u16::from(byte) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

/// CRC-32 (IEEE 802.3) ตัวเดียวกับ zlib
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &byte in data {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    !crc
}

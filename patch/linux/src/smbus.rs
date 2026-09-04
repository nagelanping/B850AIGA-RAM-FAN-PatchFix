//! 南桥 SMBus HST word read，读取候选 SPD 温度传感器。
//!
//! 链路（LOG.md §5，固件 SkSmartFanProtocol 0x1614/0x183c 同款）：
//! 基址 0xb00；候选 SPD 7-bit 地址 0x53/0x52/0x51/0x50；命令 0x31；word read 2 字节。

use crate::port::Port;
use std::io;
use std::time::{Duration, Instant};

pub const SMBUS_BASE: u16 = 0xb00;
pub const CMD_DIMM_TEMP: u8 = 0x31;
/// 候选 SPD 温度传感器 7-bit 地址（槽位）。固件用 0xa6/0xa4/... 写格式地址；
/// 这里统一用 7-bit，写 HST 时转换。
pub const SPD_ADDR7: [u8; 4] = [0x53, 0x52, 0x51, 0x50];

// HST 寄存器相对基址偏移（LOG.md §5 / exp2_smbus_probe.py 实机验证；i2c-piix4 标准布局）
const HST_STS: u16 = 0; // 0xb00 状态：bit0 BUSY；其余为错误位
const HST_CNT: u16 = 2; // 0xb02 控制
const HST_CMD: u16 = 3; // 0xb03 命令
const XMIT_SLVA: u16 = 4; // 0xb04 从地址
const HST_DATA0: u16 = 5; // 0xb05 数据低
const HST_DATA1: u16 = 6; // 0xb06 数据高

const STS_BUSY: u8 = 0x01;
/// SMBus word read + start（协议 0x05 + start 位）。
const CNT_WORD_READ_START: u8 = 0x4c;

/// raw word（16-bit 小端，来自 HST_DATA0/1）→ 整数摄氏度。
/// 固件换算：((raw << 3) >> 5) * 25 / 100。
pub fn raw_to_celsius(raw: u16) -> u32 {
    let scaled = ((raw as u32) << 3) >> 5;
    scaled * 25 / 100
}

/// DIMM 合理温度范围（℃）。越界视为无效样本；负温（raw 高位为符号）换算后越界，同样拒绝。
pub fn celsius_valid(c: u32) -> bool {
    (MIN_DIMM_TEMP..=MAX_DIMM_TEMP).contains(&c)
}

const MIN_DIMM_TEMP: u32 = 0;
const MAX_DIMM_TEMP: u32 = 120;

/// 对单个 7-bit 从地址执行一次 HST word read，返回摄氏度温度。
pub fn read_dimm_temp(port: &Port, addr7: u8) -> io::Result<u32> {
    let base = SMBUS_BASE;
    port.write_u8(base + HST_STS, 0xff)?; // W1C 清状态
    port.write_u8(base + XMIT_SLVA, (addr7 << 1) | 1)?;
    port.write_u8(base + HST_CMD, CMD_DIMM_TEMP)?;
    port.write_u8(base + HST_CNT, CNT_WORD_READ_START)?;

    let deadline = Instant::now() + Duration::from_millis(100);
    let sts = loop {
        let s = port.read_u8(base + HST_STS)?;
        if s & STS_BUSY == 0 {
            break s;
        }
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                format!("SMBus addr 0x{:02x}: BUSY timeout, sts=0x{:02x}", addr7, s),
            ));
        }
        std::thread::sleep(Duration::from_micros(200));
    };
    // v1 采取保守策略：BUSY 之外任何状态位置位都算失败，便于实机观察完整错误位。
    if sts & !STS_BUSY != 0 {
        return Err(io::Error::other(format!(
            "SMBus addr 0x{:02x}: sts=0x{:02x}",
            addr7, sts
        )));
    }

    let d0 = port.read_u8(base + HST_DATA0)?;
    let d1 = port.read_u8(base + HST_DATA1)?;
    let raw = u16::from_le_bytes([d0, d1]);
    let c = raw_to_celsius(raw);
    if !celsius_valid(c) {
        return Err(io::Error::other(format!(
            "SMBus addr 0x{:02x}: raw=0x{:04x} -> {}°C out of range",
            addr7, raw, c
        )));
    }
    Ok(c)
}

/// 轮询全部候选地址。返回与 SPD_ADDR7 一一对应的结果，由调用方汇总。
pub fn read_all_dimm_temps(port: &Port) -> Vec<(u8, io::Result<u32>)> {
    SPD_ADDR7
        .iter()
        .map(|&a| (a, read_dimm_temp(port, a)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_sample() {
        // exp2 实机数据：d0=96 d1=2 raw=608 -> 38°C
        assert_eq!(raw_to_celsius(608), 38);
    }

    #[test]
    fn boundaries() {
        assert_eq!(raw_to_celsius(0), 0);
        assert_eq!(raw_to_celsius(30 * 16), 30);
        assert_eq!(raw_to_celsius(120 * 16), 120);
        // 负温度（u16 高位符号样式）换算后越界 → 拒绝
        assert!(!celsius_valid(raw_to_celsius(0xFFF0)));
        assert!(!celsius_valid(raw_to_celsius(u16::MAX)));
        assert!(celsius_valid(38));
        assert!(!celsius_valid(121));
    }

    #[test]
    fn hst_offsets_match_verified_sequence() {
        // 锚死 LOG.md §5 / WORKFLOW §3.2 / exp2_smbus_probe.py 三方一致的绝对地址
        assert_eq!(
            (
                SMBUS_BASE + HST_STS,
                SMBUS_BASE + HST_CNT,
                SMBUS_BASE + HST_CMD,
                SMBUS_BASE + XMIT_SLVA,
                SMBUS_BASE + HST_DATA0,
                SMBUS_BASE + HST_DATA1,
            ),
            (0xb00, 0xb02, 0xb03, 0xb04, 0xb05, 0xb06)
        );
    }
}

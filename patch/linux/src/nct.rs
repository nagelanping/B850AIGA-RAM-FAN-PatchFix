//! NCT6796D SIO 自定义端口（0x295/0x296）页选择与 Virtual_TEMP 值写入。
//! 只碰页 0x0c 的 reg 0x36；不写曲线、源选择、模式（WORKFLOW.md §3.3 安全边界）。

use crate::port::Port;
use std::io;

pub const SIO_IDX: u16 = 0x295;
pub const SIO_DAT: u16 = 0x296;

/// 内存风扇 Virtual_TEMP 值寄存器所在页与偏移（LOG.md §6.6 实验 1）。
pub const VIRTEMP_PAGE: u8 = 0x0c;
pub const VIRTEMP_REG: u8 = 0x36;

/// 页选择序列（保留高 4 位，仿 SkSmartFanCtrlPei）：
/// outb(0x4e, IDX); v = inb(DAT); outb((v & 0xf0) | page, DAT)
pub fn select_page(port: &Port, page: u8) -> io::Result<()> {
    port.write_u8(SIO_IDX, 0x4e)?;
    let v = port.read_u8(SIO_DAT)?;
    port.write_u8(SIO_DAT, (v & 0xf0) | (page & 0x0f))?;
    Ok(())
}

/// 写入 Virtual_TEMP（°C×1，例如 0x1e=30°C），随后读回校验。
pub fn write_virtemp(port: &Port, celsius: u8) -> io::Result<()> {
    select_page(port, VIRTEMP_PAGE)?;
    port.write_u8(SIO_IDX, VIRTEMP_REG)?;
    port.write_u8(SIO_DAT, celsius)?;

    select_page(port, VIRTEMP_PAGE)?;
    port.write_u8(SIO_IDX, VIRTEMP_REG)?;
    let readback = port.read_u8(SIO_DAT)?;
    if readback != celsius {
        return Err(io::Error::other(format!(
            "Virtual_TEMP write mismatch: wrote {}, read back {}",
            celsius, readback
        )));
    }
    Ok(())
}

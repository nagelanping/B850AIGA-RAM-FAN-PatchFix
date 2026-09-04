//! /dev/port 单字节端口读写（唯一硬件后端）。
//! 需要 root 或 CAP_SYS_RAWIO。

use std::fs::{File, OpenOptions};
use std::io;
use std::os::unix::fs::FileExt;

pub struct Port {
    f: File,
}

impl Port {
    pub fn open() -> io::Result<Self> {
        Ok(Self {
            f: OpenOptions::new()
                .read(true)
                .write(true)
                .open("/dev/port")?,
        })
    }

    pub fn read_u8(&self, port: u16) -> io::Result<u8> {
        let mut b = [0u8; 1];
        let n = self.f.read_at(&mut b, port as u64)?;
        if n != 1 {
            return Err(io::Error::other(format!(
                "/dev/port read_at(0x{:x}) returned {} bytes",
                port, n
            )));
        }
        Ok(b[0])
    }

    pub fn write_u8(&self, port: u16, value: u8) -> io::Result<()> {
        let n = self.f.write_at(&[value], port as u64)?;
        if n != 1 {
            return Err(io::Error::other(format!(
                "/dev/port write_at(0x{:x}) returned {} bytes",
                port, n
            )));
        }
        Ok(())
    }
}

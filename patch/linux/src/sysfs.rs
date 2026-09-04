//! sysfs 后端：从内核 `spd5118` hwmon 读取 DIMM 温度（毫摄氏度）。
//!
//! 实机验证（2026-09-04）：两根 DIMM 对应 `spd5118` hwmon 各一个（hwmon4/hwmon6），
//! 内核经 `i2c_piix4`（FCH SMBus 0xb00）按需采样。读 sysfs 避免用户态 raw SMBus
//! 与内核竞争（WORKFLOW.md 阶段 0 第 2 条的优先路径）。

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

const SENSOR_NAME: &str = "spd5118";

/// hwmon 目录的 name 是否为本传感器（防止 hwmon 号被其它驱动复用后误读）。
pub fn is_sensor(dir: &Path) -> bool {
    fs::read_to_string(dir.join("name"))
        .map(|s| s.trim() == SENSOR_NAME)
        .unwrap_or(false)
}

/// 扫描 /sys/class/hwmon，返回所有 spd5118 hwmon 目录。
pub fn find_sensor_hwmuns() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = fs::read_dir("/sys/class/hwmon") {
        for e in rd.flatten() {
            let dir = e.path();
            if is_sensor(&dir) {
                out.push(dir);
            }
        }
    }
    out
}

/// 读取一个 hwmon 目录下全部 temp*_input，返回 (文件路径, 摄氏度) 列表。
pub fn read_hwmon_temps(dir: &Path) -> Vec<(PathBuf, io::Result<u32>)> {
    let mut out = Vec::new();
    let Ok(rd) = fs::read_dir(dir) else {
        out.push((
            dir.to_path_buf(),
            Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("hwmon dir gone: {}", dir.display()),
            )),
        ));
        return out;
    };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with("temp") && name.ends_with("_input") {
            let path = e.path();
            let v = read_temp_input(&path);
            out.push((path, v));
        }
    }
    out
}

/// temp*_input（毫摄氏度，可为负）→ 整数摄氏度（四舍五入），范围外拒绝。
pub fn millideg_to_celsius(milli: i64) -> io::Result<u32> {
    if !(0..=120_000).contains(&milli) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{milli} m°C out of range 0..120000"),
        ));
    }
    Ok(((milli + 500) / 1000) as u32)
}

fn read_temp_input(path: &Path) -> io::Result<u32> {
    let s = fs::read_to_string(path)?;
    let milli: i64 = s.trim().parse().map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{}: {e}", path.display()),
        )
    })?;
    millideg_to_celsius(milli)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn millideg_rounding() {
        assert_eq!(millideg_to_celsius(41250).unwrap(), 41);
        assert_eq!(millideg_to_celsius(43250).unwrap(), 43);
        assert_eq!(millideg_to_celsius(0).unwrap(), 0);
        assert_eq!(millideg_to_celsius(120_000).unwrap(), 120);
        // 实机 raw 对照：704 -> 44.0°C；sysfs 同数量级
        assert_eq!(millideg_to_celsius(44_000).unwrap(), 44);
    }

    #[test]
    fn out_of_range_rejected() {
        assert!(millideg_to_celsius(-1).is_err());
        assert!(millideg_to_celsius(-50).is_err()); // 未实现传感器的常见值
        assert!(millideg_to_celsius(120_001).is_err());
        assert!(millideg_to_celsius(i64::MIN).is_err());
    }
}

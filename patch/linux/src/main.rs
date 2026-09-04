//! ram-fan-virtual-temp：把 DIMM 温度持续喂给 NCT6796D Virtual_TEMP（SIO 页 0x0c reg 0x36），
//! 使 FAN5=MEM_FAN 按 BIOS 曲线运行。用法：
//!   ram-fan-virtual-temp           常驻，每 0.5s 一轮（systemd 服务）
//!   ram-fan-virtual-temp --once    单次“读取→汇总→写入”，阶段 B 实机验证用
//!
//! 温度读取走内核 spd5118 hwmon（sysfs），避免与内核争抢 SMBus 控制器；
//! 写入走 /dev/port 的 NCT SIO 端口（与 nct6775 驱动的并发在实机验证）。

mod nct;
mod port;
mod sysfs;

use std::path::PathBuf;
use std::time::Duration;

const CYCLE: Duration = Duration::from_millis(500);

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let once = args.iter().any(|a| a == "--once");
    if let Some(other) = args.iter().find(|a| a.as_str() != "--once") {
        eprintln!("unknown argument: {other}");
        eprintln!("usage: ram-fan-virtual-temp [--once]");
        std::process::exit(2);
    }

    let port = match port::Port::open() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("open /dev/port failed (需要 root/CAP_SYS_RAWIO): {e}");
            std::process::exit(1);
        }
    };

    if once {
        let mut known = Vec::new();
        let ok = match run_cycle(&port, &mut known, true) {
            Ok(()) => true,
            Err(e) => {
                eprintln!("{e}");
                false
            }
        };
        std::process::exit(if ok { 0 } else { 1 });
    }

    let mut known: Vec<PathBuf> = Vec::new();
    let mut consec_fail: u32 = 0;
    loop {
        match run_cycle(&port, &mut known, false) {
            Ok(()) => {
                if consec_fail != 0 {
                    eprintln!("INFO: cycle recovered after {consec_fail} failures");
                }
                consec_fail = 0;
            }
            Err(e) => {
                consec_fail += 1;
                if consec_fail == 1 || consec_fail.is_multiple_of(10) {
                    eprintln!("WARN: {consec_fail} consecutive failed cycles: {e}");
                }
            }
        }
        std::thread::sleep(CYCLE);
    }
}

/// 一轮：读全部 spd5118 传感器 → 取最高 → 写 Virtual_TEMP。
/// 返回本轮是否成功写入。verbose=true（--once）时逐传感器打印。
fn run_cycle(port: &port::Port, known: &mut Vec<PathBuf>, verbose: bool) -> Result<(), String> {
    let hwmuns = sysfs::find_sensor_hwmuns()
        .map_err(|e| format!("scan /sys/class/hwmon failed, skip write: {e}"))?;

    // hwmon 目录消失或 name 已非 spd5118（模块卸载/换号复用）→ 从 known 移除，不算不完整。
    let mut retained = Vec::with_capacity(known.len());
    for dir in known.iter() {
        match sysfs::is_sensor(dir) {
            Ok(true) => retained.push(dir.clone()),
            Ok(false) => {}
            Err(e) => {
                return Err(format!(
                    "verify known sensor {} failed, skip write: {e}",
                    dir.display()
                ));
            }
        }
    }
    *known = retained;
    for h in &hwmuns {
        if !known.contains(h) {
            known.push(h.clone());
            if verbose {
                eprintln!("found sensor: {}", h.display());
            }
        }
    }

    let mut samples: Vec<u32> = Vec::new();
    let mut incomplete = false;
    let mut first_err: Option<String> = None;
    for dir in known.iter() {
        for (path, res) in sysfs::read_hwmon_temps(dir) {
            match res {
                Ok(c) => samples.push(c),
                Err(e) => {
                    incomplete = true;
                    if verbose {
                        eprintln!("{}: {e}", path.display());
                    } else if first_err.is_none() {
                        first_err = Some(format!("{}: {e}", path.display()));
                    }
                }
            }
        }
    }

    // 汇总完整性：任一已发现传感器读取失败 → 本轮不完整，不写入（保持上一次值）。
    if samples.is_empty() || incomplete {
        if verbose {
            eprintln!(
                "no complete sample set ({} sensors), skip write",
                known.len()
            );
        } else if let Some(e) = &first_err {
            return Err(format!("cycle incomplete, skip write; first error: {e}"));
        } else {
            return Err("cycle incomplete, skip write; no temperature samples".into());
        }
        return Err("cycle incomplete, skip write".into());
    }

    let tmax = *samples.iter().max().unwrap();
    // millideg_to_celsius 保证 tmax<=120，u8 转换不越界
    let val = tmax as u8;
    match nct::write_virtemp(port, val) {
        Ok(()) => {
            if verbose {
                eprintln!("wrote Virtual_TEMP = {val}°C (samples={samples:?})");
            }
            Ok(())
        }
        Err(e) => Err(format!("ERROR: write Virtual_TEMP failed: {e}")),
    }
}

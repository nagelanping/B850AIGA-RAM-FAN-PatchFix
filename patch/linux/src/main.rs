//! ram-fan-virtual-temp：把 DIMM 温度持续喂给 NCT6796D Virtual_TEMP（页 0x0c reg 0x36），
//! 使 FAN5=MEM_FAN 按 BIOS 曲线运行。用法：
//!   ram-fan-virtual-temp           常驻，每 2s 一轮（systemd 服务）
//!   ram-fan-virtual-temp --once    单次“读取→汇总→写入”，阶段 B 实机验证用

mod nct;
mod port;
mod smbus;

use std::time::Duration;

const CYCLE: Duration = Duration::from_secs(2);

fn main() {
    let once = std::env::args().skip(1).any(|a| a == "--once");
    if let Some(other) = std::env::args().skip(1).find(|a| a != "--once") {
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
        let ok = run_cycle(&port, &mut known, true);
        std::process::exit(if ok { 0 } else { 1 });
    }

    let mut known: Vec<u8> = Vec::new();
    let mut consec_fail: u32 = 0;
    loop {
        if run_cycle(&port, &mut known, false) {
            consec_fail = 0;
        } else {
            consec_fail += 1;
            // 连续失败降频日志，避免刷屏
            if consec_fail == 1 || consec_fail.is_multiple_of(10) {
                eprintln!("WARN: {consec_fail} consecutive failed cycles");
            }
        }
        // 周期含本轮硬件耗时，保持约 2s 间隔
        std::thread::sleep(CYCLE);
    }
}

/// 一轮：读全部候选 DIMM → 汇总最高值 → 写 Virtual_TEMP。
/// 返回本轮是否成功写入。详细结果 verbose=true（--once）时逐地址打印。
fn run_cycle(port: &port::Port, known: &mut Vec<u8>, verbose: bool) -> bool {
    let results = smbus::read_all_dimm_temps(port);

    let mut samples: Vec<u32> = Vec::new();
    let mut first_err: Option<String> = None;
    for (addr, res) in &results {
        match res {
            Ok(c) => {
                samples.push(*c);
                if !known.contains(addr) {
                    known.push(*addr);
                    if verbose {
                        eprintln!("found DIMM sensor at 7-bit addr 0x{addr:02x}");
                    }
                }
            }
            Err(e) => {
                if verbose {
                    eprintln!("addr 0x{addr:02x}: {e}");
                } else if first_err.is_none() {
                    first_err = Some(format!("addr 0x{addr:02x}: {e}"));
                }
            }
        }
    }

    // 汇总完整性（WORKFLOW.md §3.2）：曾经发现过的 DIMM 本轮缺失 → 本轮不完整，
    // 不写入（保持上一次值），避免用偏低样本降速。空槽（从未成功过）不影响完整性。
    let missing = known
        .iter()
        .any(|a| !results.iter().any(|(addr, r)| *addr == *a && r.is_ok()));
    if samples.is_empty() || missing {
        if verbose {
            eprintln!("no complete sample set (known={known:?}), skip write");
        } else if let Some(e) = &first_err {
            eprintln!("cycle incomplete, skip write; first error: {e}");
        }
        return false;
    }

    let tmax = *samples.iter().max().unwrap();
    // celsius_valid 保证 tmax<=120，u8 转换不越界
    let val = tmax as u8;
    match nct::write_virtemp(port, val) {
        Ok(()) => {
            if verbose {
                eprintln!("wrote Virtual_TEMP = {val}°C (samples={samples:?})");
            }
            true
        }
        Err(e) => {
            eprintln!("ERROR: write Virtual_TEMP failed: {e}");
            false
        }
    }
}

# Windows / Linux 内存风扇修复工作流

## 1. 唯一目标

修复 MAXSUN MS-iCraft B850 AIGA 在重启后 `FAN5=MEM_FAN` 曲线失效的问题：持续读取 DIMM 温度并写入 NCT6796D `Virtual_TEMP`。不修改 BIOS、曲线、温度源或固件。

```text
DIMM SPD → FCH SMBus → DIMM 温度 → NCT Virtual_TEMP → BIOS Smart Fan → FAN5
```

Linux 版本已实机验证并交付，是当前可用的修复。Windows 版本按受控实验推进，测试签名版已完成实机验证与发布打包。

## 2. 硬件事实

- NCT 芯片：chip id `0xd802`，NCT6796D-S / NCT6799D 兼容系列。
- NCT SIO：index `0x295`、data `0x296`；标准 SIO `0x2e/0x2f`。
- `FAN5=MEM_FAN`：page `0x09`、reg `0x00` = source `0x0a`（`Virtual_TEMP`）。
- 写回目标：page `0x0c`、reg `0x36`；编码为整数摄氏度。
- FCH SMBus：实机诊断基址 `0xb00`，控制器 `PCI\VEN_1022&DEV_790B`。
- SPD 地址按 `0x53, 0x52, 0x51, 0x50` 轮询，命令 `0x31`，word read。目标机已装 `0x53` + `0x51`，空槽 `0x50` + `0x52`。
- 温度换算：`scaled=(raw << 3) >> 5`，`celsius=(scaled * 25) / 100`，有效范围 `0..120°C`。
- SMBus `HST_STS=0x02` 可表示成功；`0x04` 不能简单等同于失败类型，空槽与已安装 DIMM 的错误处理必须区分。
- NCT 页选择必须保留高 4 位：读取旧值后写 `(old & 0xf0) | page`。
- 实测：写 30°C → `pwm5=76`、约 1031 rpm；写 40°C → `pwm5=101`、约 1326 rpm。

## 3. 当前进展和结论（2026-09-06）

### 3.1 已完成

- Linux 服务：读取 `spd5118` hwmon、最高有效 DIMM 温度选择、NCT 写回/读回校验、systemd 常驻、实机验证；成品以 `B850AIGA-RAM-FAN-PatchFix_Linux.tar.gz` 形式另行分发。
- Windows 受控模型全部实机验证通过（2026-09-05）：身份门禁（§5.2 第 1 步）、受控 SMBus 读取（第 2 步）、单次写回 FEED_ONCE（第 3 步）、常驻 0.5s 喂值服务。
- 方案 B（2026-09-06）：压测暴露的假失败已根治——去掉映射状态机，每轮全读 4 槽，有效温度 `1..120°C`（0°C 屏蔽），单槽瞬时垃圾/超时同轮重试一次；身份门禁 60s 缓存心跳。压测 45–46°C 稳定 2.5min+ 写回一致，零 WARN/FATAL。
- 最终子代理审查：无 S1，可发布受控测试版。
- 分发准备（2026-09-06）：`install.ps1`/`uninstall.ps1`/`INSTALL.md`（PS 5.1 兼容，经子代理审查修复）、`build-release.ps1`、`RAMFanTestSign.cer`；发布目录组装于 `release/windows/`（含根 README/LICENSE），发布者手动压 7z 为 `B850AIGA-RAM-FAN-PatchFix_Windows_TestSign.7z`。

### 3.2 已证、不重做

已证明：

- 通用 `PNP0C02` upper-filter 在运行期和开机栈构建均不挂载；驱动不会收到 `EvtDevicePrepareHardware`。
- `pnputil`、SetupAPI `DIF_INSTALLDEVICE`、`UpdateDriverForPlugAndPlayDevicesW` 均拒绝以第三方 function driver 替换目标节点。
- 因此“绑定 `PNP0C02` 并取得 translated resources”在本平台不能作为 Windows 实现前提。
- 不再继续修改 INF、upper-filter、PNP0C02 function-driver 替换或用 PCI 资源授权 NCT 端口。

`LOG.md` 中早于 2026-09-05 的 PNP0C02 绑定方案仅是历史计划，不得恢复为当前实施计划或授权依据。

### 3.3 授权状态

- 批准本目标机受控非 PnP 访问模型实验例外；四件事（身份/访问/实验批准/发布批准）记录于 `AGENTS.md`“授权边界”，逐级批准至常驻喂值阶段。
- 发布批准：测试签名版为最终交付形态，不计划 Microsoft Attestation/WHQL/EV 官方认证。

## 4. 修复优先级与路线选择

1. **Linux 交付**：已验证的修复，不等待 Windows。
2. **Windows 受控可行性试验**：批准的非 PnP KMDF + 固定端口访问模型，已完成全部受控试验并打包测试签名版。
3. **Windows 公测版**：测试签名版一键安装 + 开机自启；无官方认证计划。
4. **SMM/BIOS 方案**：仅在 Windows 受控试验失败且机主另行批准后评估；本工作流不刷写 BIOS。

受控非 PnP 模型仅以批准的目标机实验例外推进；公测版不再主张正式交付路径。

## 5. Windows 受控模型与试验记录

### 5.1 最小边界

候选实现为**非 PnP KMDF 控制设备 + 驱动内固定目标端口**：

- 只支持已确认的主板/芯片身份：通过系统 PnP 枚举确认 PCI `DEV_790B` + NCT chip id `0xd802` 的拒绝式校验；任何一项不匹配即不工作。
- 端口白名单只允许 `0xb00` SMBus、`0x295/0x296` NCT SIO，以及仅用于 NCT 身份探针的标准 SIO `0x2e/0x2f`；不提供任意端口或任意寄存器 IOCTL。
- 只允许写 NCT page `0x0c` / reg `0x36`；SMBus 事务、超时清理、页恢复和写后读回在驱动内完成并加锁。
- ACPI 资源声明是诊断/平台绑定证据，不等于独占；NCT/SMBus 可能被固件、ACPI/WMI 或监控软件并发访问。
- 仅允许测试机、管理员/SYSTEM、明确签名的驱动和可逆安装；不关闭签名策略作为发布方案，不引入 WinRing0/InpOut32。

### 5.2 试验记录（均已实机验证，2026-09-05/06）

1. **只读身份门禁**：`QUERY_HW` 结果 `SMBusBase=0x0b00 ChipId=d802 ControllerFound=1 ChipIdValid=1 HwMatched=1`。PCI `DEV_790B` 用 `Enum\PCI` 注册表前缀枚举（`HalGetBusDataByOffset` 在此 x64 平台不可用，已证实）；NCT chip id 用标准 SIO `0x2e/0x2f` 探针。
2. **受控 SMBus 读取**：`--dimm` 六轮一致——已装 `0x53`/`0x51` OK（hst=0x02，~30–42°C），空槽 `0x52`/`0x50` BUS_ERR（hst=0x06）。修正“0x53/0x52 已装”假设为 `0x53`+`0x51`。
3. **单次写回**：`--once` 6 轮 `status=0 written=35 readback=35`，NCT page `0x0c`/reg `0x36` 写回与读回校验一致，页保存/恢复。
4. **常驻喂值**：0.5s FEED_ONCE 循环 + SCM（DEMAND_START）；RUNNING 下每轮 `FEED ok: max/written/readback` 随 DIMM 温度波动，服务停止保留值、重启平滑恢复。
5. **方案 B + 重试 + 门禁缓存**：压测 45–46°C 稳定 2.5min+ 写回一致、零 WARN/FATAL。取舍记录：门禁负结果缓存 60s（拒绝侧安全）、FATAL 瞬时误报窗口、日志无轮转（正式发布前加）。

### 5.3 失败试验与回滚规则

- 覆盖空槽、已装 DIMM 失败、全部失败、SMBus BUSY 超时、NCT 读回不一致、外部并发；失败时不写 0°C、不写猜测值，保留旧值并记录热安全风险。
- 回滚：运行 `uninstall.ps1`（停删 `RAMFan`/`RAMFanPnP` 服务、删驱动文件与 `ProgramData\RAMFan` 副本）。回滚验收确认 `RAMFanPnP`/控制设备/服务/驱动文件无残留，重启后原设备栈恢复。NCT 最后一笔有效 `Virtual_TEMP` 不主动清除，须记录该行为。
- 内核驱动一旦加载 `sc stop` 返回 1052 无法热卸载，换驱动必须重启。

## 6. 发布与质量门槛

### 6.1 公测版分发（当前形态，2026-09-06 定案）

- 面向高级用户的公测版。安装脚本一键处理环境：自动开启 testsigning、关闭内存完整性（HVCI）、导入测试证书；Secure Boot 需在 BIOS/UEFI 手动关闭（脚本检测到 ON 时提示）。需要重启时脚本提示，重启后再次运行同一命令完成安装。
- 驱动 `RAMFanPnP` 与喂值服务 `RAMFan` 均为 AUTO_START（开机自启）；`RAMFan` 服务依赖 `RAMFanPnP`，保证开机加载顺序。
- 卸载脚本 `uninstall.ps1` 支持 `-RestoreSecurity`：自动还原 testsigning（关闭）与内存完整性（重新启用），需重启生效；Secure Boot 需自行回 BIOS 开启；测试证书删除有风险（其他测试驱动可能共用），仅打印命令。
- 分发脚本兼容 Windows PowerShell 5.1（提权用当前宿主重启）。
- 单一版本源在 `patch/windows/`；`build-release.ps1` 组装到 `release/windows/`（不入库），压缩由发布者手动完成（7z）。
- 桌面“测试模式”水印是 testsigning 开启的 Windows 标识，属预期现象；随 testsigning 关闭（即卸载还原）而消失。补丁不提供隐藏水印工具。

### 6.2 公测版质量门槛

公测版无官方签名认证计划，交付前仍须满足：

- x64 Release 构建、静态检查、纯逻辑自检、安装/卸载/回滚验证通过。
- 签名验证：`.sys` 页哈希签名与 `ramfan-service.exe` 签名均通过 `signtool verify`。
- 安装脚本一键配置路径（自动 testsigning/HVCI/证书 + 重启续装）与 `-RestoreSecurity` 卸载还原路径完整实测。
- 由独立子代理审查寄存器、SMBus 状态、温度换算、IOCTL 边界、并发、失败策略、签名和回滚后，才允许提交/打包。

## 7. Linux 交付路线（维护边界）

Windows 决策期间不改造 Linux 链路。发布/维护只做必要修复：

- 读取 `spd5118` hwmon，读取不完整时跳过写入并保留上次完整值。
- 写 NCT page `0x0c` / reg `0x36`，写后读回校验。
- 周期默认 0.5 秒，温度有效范围 `0..120°C`。
- 修改后运行 `cargo fmt`、`cargo check`、`cargo test`、`cargo clippy`。

## 8. 文档、审查与记录规则

- 每个决策先写入根目录 `LOG.md`：选择、依据、风险、是否批准。
- Windows `patch/` 的任何非文档代码修改，先查现有调用链；完成后必须独立子代理审查，再提交。
- 所有实机试验记录 OS、BIOS、内存、基线、命令、结果、签名状态、回滚状态和未决风险。
- 不刷 BIOS、不修改 BIOS 变量、不修改 `page 0x09` 曲线/模式/温度源。
- 不同时运行多个会写 `0x295/0x296` 或访问 SMBus 的工具。
- 文档用中性写作规范：一个概念一个术语；先给结论；命令/路径/默认值保持准确，缺失事实不臆造。

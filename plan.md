# BBK 9288S QEMU 模拟器计划

> 开源整理说明：早期逆向阶段使用过 `debug-touch-ui-inject` 和
> `debug-touch-ui-gate-mask` 验证固件 UI 分发路径。真实串行触摸 ADC
> 硬件路径完成后，这两个 guest 内存诊断入口已从发布源码中删除。下文相关
> 内容仅作为历史实验记录。

更新时间：2026-07-23

## 目标

用 QEMU 实现一个步步高 9288S 的可观察、可调试模拟器。

当前阶段不追求一次性完整还原整机，而是让真实 NAND 固件在稳定的设备骨架上运行，并把 CPU、MMIO、中断、触摸、LCD、存储等行为做成可追踪、可复现实验。每次推进都应留下日志证据，避免靠猜测硬凑。

## 项目和固件

- 项目目录：`E:\eebbk9288s-qemu`
- 运行固件：`E:\eebbk9288s-runtime\nand-user.raw`
- 启动方式：板级加载器解析 NAND OOB/FTL 和 FAT16，直接读取其中的
  `系统\kernel.bin`；正式启动不需要外置内核文件
- 固件格式：`KNL 0100200608021118`
- KNL header：`0x40` 字节
- payload 加载地址：`0x02000000`
- 当前 reset/入口 PC：`0x0200468a`

## 硬件依据

当前按 Epson S1C33L05 / C33 STD 32-bit RISC 建模。

主要参考：

- Epson S1C33L05 Technical Manual: https://global.epson.com/products_and_drivers/semicon/pdf/id000276.pdf
- Epson S1C33000 Core CPU Manual: https://global.epson.com/products_and_drivers/semicon/pdf/id000569.pdf

与 9288S 的匹配点：

- C33 STD 32-bit RISC，固定 16-bit 指令。
- 片内 RAM 16 KiB。
- 片内 VRAM 40 KiB。
- LCDC 支持 `2 bpp / 4-level gray scale`，匹配 9288S 四灰阶屏幕。
- 当前按竖屏 `160x240`、`2bpp` 建模，framebuffer 为 `9600` 字节，可放入片内 VRAM。
- 10-bit ADC 适合建模电阻触摸屏。
- SDRAM、NAND/SmartMedia、USB function、LCDC 都符合 9288S 这类学习机/电子词典设备。

关键中断向量依据：

- port input 0..3：vector `12..15`
- key input 0/1：vector `16/17`
- 16-bit timer0 comparison B：vector `24`
- 16-bit timer3 comparison B：vector `42`
- 8-bit timer0..3：vector `53`
- A/D converter：vector `64`
- clock timer：vector `65`
- port input 4..7：vector `68..71`

## 当前实现状态

### QEMU target 和 machine

- 已新增 `s1c33-softmmu` target。
- 已新增 `bbk9288s` machine。
- 已新增 `c33l05` CPU 类型。
- SDRAM 映射在 `0x02000000`，默认 8 MiB。
- IRAM 映射在 `0x00000000`，大小 16 KiB。
- 片内 VRAM 映射在 `0x003c0000..0x003c9fff`，大小 40 KiB。
- LCDC 寄存器窗口映射在 `0x00380000`。
- direct KNL load 时 SP 初始化为固件入口实际设置的 `0x3f80`；IRAM 顶部
  `0x3ffc..0x3fff` 保留 RTC 有效标记 `01 02 03 04`。
- KNL loader 已识别 `KNL ` header、加载 payload、扫描 vector 并设置 PC。
- Windows/MSYS2 symlink 限制已加 hardlink/copy fallback。

### CPU

已实现真实固件早期路径需要的大部分 C33 指令：

- `EXT`、`NOP`、`SLP`、`HALT`
- `RET`、`RET.d`、`RETI`
- 相对 `call/jp`、寄存器绝对 `call/jp`
- 条件跳转和常见 delayed forms
- `pushn/popn`
- SP 相对 byte/half/word load/store
- 特殊寄存器和通用寄存器传送
- byte/half/word load/store
- 常见 ALU/logic/compare
- shift/rotate：`srl/sll/sra/sla/rr/rl`
- `scan0/scan1`
- 带进位加减：`adc`、`sbc`，并更新 `N/Z/V/C`。
- multiplier/divider 常用路径
- bit 操作
- `BRK`
- PSR bit set/clear

已实现调试能力：

- GDB register XML。
- unimplemented opcode 打印 PC、opcode、寄存器状态、最近 TB。
- HALT/SLP 打印 PC、next PC、CPU 状态、寄存器、最近 TB。
- CPU 属性：`exit-on-halt`、`trace-psr`、`trace-calls`、`trace-exec`、`trace-exec-start`、`trace-exec-end`、`trace-mem`、`trace-mem-start`、`trace-mem-end`、`trace-mem-pc-start`、`trace-mem-pc-end`。
- `trace-exec` 默认关闭；开启后在 TB 入口打印 `pc/sp/psr`。
- 若设置 `trace-exec-start/end`，则只记录 TB PC 位于该闭区间内的执行。
- `trace-mem` 默认保持旧行为：开启后记录全部 memory helper 访问。
- 若设置 `trace-mem-start/end`，则只记录访问区间与该闭区间重叠的读写。
- 若设置 `trace-mem-pc-start/end`，则只记录执行 PC 位于该闭区间内的读写。

已修正：

- `HALT` 和 `SLP` 的 CPU 状态已经区分。
- 只有 `SLP` 会设置 `env.in_sleep=true`。
- 新增 `s1c33_cpu_resume_from_sleep()`，用于设备模型从真实 SLP 状态恢复 CPU。
- 修正 `HALT` / `SLP` helper 退出方式：translator 已把 `env->pc` 写成下一条指令，helper 现在用 `cpu_loop_exit()` 保留这个架构 PC，避免 IRQ/RETI 返回到 `HALT` 本身。

### 中断和低功耗

已实现：

- TTBR CPU 状态和 `0x48134..0x48137` byte mirror。
- maskable IRQ 入口/返回：检查 `PSR.IE`、`PSR.IL`，通过 `TTBR + vector * 4` dispatch，`RETI` 恢复。
- NMI vector `7`。
- 已验证 16-bit timer3-B、port input 4/5、ADC、USB/SPI/LCDC group 的基本 ITC pending 计算。
- Timer3-B 只有在 `T16_CTRL3.PRUN=1` 时才运行；pen release 后固件清 `PRUN` 会停止采样中断，不再形成错误的永久 vector `42` 循环。
- key input 0/1 vector `16/17` dispatch。
- 16-bit timer0-B vector `24` dispatch metadata。
- clock timer / CTM vector `65` dispatch。
- `0x40151..0x4015b` CTM 寄存器已命名并做最小模型：
  - `0x40151` `TCRUN/TCRST`，`TCRST` 只在 stop 状态、且本次写入不同时启动时清计数器。
  - `0x40152` `TCINT`，保留选择位，`TCIF/TCAF` 按 write-one-to-clear 处理。
  - `0x40154..0x40158` 秒/分/时/day 计数；秒寄存器写入按只读忽略。
  - `0x4026b` `PCTM` priority，`0x40277/0x40287 bit1` 接 `ECTM/FCTM`。
- CTM 当前按约 32Hz 更新计数器，但严格按 `TCISE[2:0]` 选择位决定是否产生 `TCIF/FCTM`；固件启动后常见 `TCISE=111` 表示不产生周期 CTM interrupt。
- reset 时从 QEMU RTC 初始化秒、分、时和 16-bit day counter。固件的
  day epoch 已由静态代码和 SDK `dayfrom1900` 字段交叉确认：实际有效
  日期范围为 1901..2049，计数起点是 1901-01-01。

低功耗相关：

- 命名 S1C33L05 clock generator 寄存器：
  - `0x40180` `CLG_POWER`
  - `0x40181` `CLG_PRESCALER`
  - `0x40190` `CLG_CLOCK_OPT`
  - `0x4019e` `CLG_PROTECT`
- 依据 S1C33L05 manual 中 `8T1ON` 对 SLEEP 后 OSC3 稳定等待的描述，新增 8-bit timer1 underflow 释放 SLP 的模型。
- 当前实现只在 CPU 真的处于 `SLP` 且 `CLG_CLOCK_OPT.8T1ON` 允许时恢复 CPU，避免把普通 `HALT` 误当成 SLP。

### MMIO 和外设

已实现 `0x00040000`、64 KiB S1C33L05 I/O stub：

- byte-addressable backing store。
- register metadata：name、group、mask、reset value。
- interrupt factor flag 支持 write-one-to-clear。
- 首次访问日志和可选 `trace-io`。

已实现 `0x01000000`、64 KiB 板级 I/O stub：

- `0x01000082..85` 的早期状态/命令握手。
- 让固件越过旧 `0x02084d32` HALT 路径。

已实现最小 HSDMA：

- 当前固件使用 channel 1：`0x48230..0x4823f`。
- `0x4029a bit 1` software trigger。
- 支持当前路径需要的 dual-address successive transfer。
- 完成后清 `HS1_EN`，让 `0x02005378` 轮询退出。

已实现最小 LCD：

- 片内 VRAM 按 `160x240`、`2bpp`、四灰阶展开。
- 已按 S1C33L05 LCDC 的 2bpp byte layout 修正 bit 顺序：一个 byte 内 4 个像素按 bit7..bit0 的 2-bit pair 展开。
- direct KNL load 下种 `HSIZE=0x0013`、`VSIZE=0x00ef`。
- 支持 `debug-lcd-dump-ms` / `debug-lcd-dump-path` 生成 PGM。
- 最新验证中校准页中文和十字光标已清晰可读，旧“画面乱”问题确认是 LCD bit order 错误。
- 当前本机构建没有 GTK/SDL/VNC，交互窗口验证需要重配 display backend。

### GPIO、按键、触摸、ADC

已实现最小 GPIO port data/control：

- P0/P1/P2/P3 data read 按 IOC 区分输入/输出。
- 输出位返回 guest latch。
- 输入位默认高电平。
- P0 key low 当前只在观测到的 P1.3 scan strobe low 时对固件可见。

已实现 QEMU keyboard hook：

- 临时映射：Enter/Space -> P0.0，Esc/Backspace -> P0.1，方向键 -> P0.3..P0.6，M/F/Tab -> P0.7。
- key down 会触发已验证的 port input 4 wake。
- host QKeyCode 去重，并按 P0 bit 计数，避免 repeat 或共享 bit 的按键提前释放。
- `trace-key-scan` 和 `debug-key-p0-low-mask` 用于反推真实键矩阵。

已实现最小 ADC 和触摸：

- hook `0x40240..0x4025c` A/D converter 寄存器。
- 软件触发会同步生成 10-bit sample，填 `ADD` 和 `ADxBUF`，置 `ADF`/`ADF_FLAGS`。
- AD0 当前建模为电池/电源监测通道，返回 `0x03c0` 这类电量正常值。
- 候选触摸通道在 touch-up 时返回开路高值 `0x03ff`。
- QEMU absolute pointer 映射到竖屏 `160x240` 坐标，再转换成 10-bit ADC。
- 片内 AD0 保持电池/电源监测用途；触摸坐标实际来自板级串行触摸 ADC，而不是片内 AD1/AD2。
- 已在 `0x00300000` 建模板级串行触摸 ADC，固件使用的数据寄存器为 `0x00300020`。
- 串行时钟是 `0x00300020 bit7`，MISO 是 bit4，MOSI 来自片内 `P1D bit3`。
- 固件命令 `0x93/0x90` 读取 Y，`0xd3` 读取 X；模型按 12-bit 数据阶段输出由 host pointer 生成的样本。
- pen down 通过 `touch-fpt=4..7` 选择 FPT 线，默认 P06/FPT6。该路径投递 port6/vector `70`，随后由 Timer3-B/vector `42` 完成周期坐标采样。
- touch down 会触发 FPT/port interrupt，也会把对应 P0 输入位拉低。
- 默认 `touch-fpt=6` 时，pen down 会让 P06/P0.6 在 P0D read 中呈现低电平。
- `0x402c1` 已命名为 `K5D` input data register，动态只读返回。
- `0x402c4` 已命名为 `K6D` input data register，动态只读返回。
- `trace-key-scan` 日志已增加 `touch-low=0x..`、`k5-low/cfk5`、`k6-low/cfk6`。
- 新增调试属性 `touch-k5-low-mask`，默认 `0`。它只用于实验性把 touch down 同步拉低指定 K5 input bit，不作为默认触摸模型。
- 新增调试属性 `touch-p0-low`，默认 `on`。关闭后 touch 仍走 port/FPT wake，但不再在 P0D/keyscan GPIO 读数中暴露低电平，用于区分 port wake 和 key matrix 行为。
- 新增诊断属性 `debug-touch-ui-inject`，默认 `off`。开启后把 QEMU touch 样本写入已观察到的 guest pen cache 和 `0x02380002/04/06/08` gate，用于验证 UI/校准层，不作为默认硬件模型。
- 新增诊断属性 `debug-touch-ui-gate-mask`，默认 `0x0f`，只在 `debug-touch-ui-inject=on` 时生效：
  - bit0 写 `0x02380002`
  - bit1 写 `0x02380004`
  - bit2 写 `0x02380006`
  - bit3 写 `0x02380008`
  该属性用于复现单 gate 实验，不改变默认触摸硬件模型。

key input interrupt：

- 已实现 S1C33L05 manual 语义下的 `SPPK/SCPK/SMPK` 最小模型。
- `SPPK` source select：
  - FPK0：`K5[4:0]`、`K6[4:0]`、`P0[4:0]`、`P2[4:0]`
  - FPK1：`K6[3:0]`、`K6[7:4]`、`P0[7:4]`、`P2[7:4]`
- `SCPK` 为 comparison condition，`SMPK` 为 mask。
- 当前按 manual 建模为从匹配态变成非匹配态时置 `FK0/FK1`。
- 已把 `FIR0_KEY_PORT0_3` bit4/bit5 分别接到 vector `16/17`。
- QMP touch 验证显示当前固件配置的是 `FK0 = K5[3:0] 从 0xf 变非 0xf`，而触摸低电平在 P0.4/K6.4，所以当前 touch 不会走 vector `16/17`。
- `touch-k5-low-mask=0x01` 负向实验显示：K5.0 拉低会触发 `key-input0` vector `16`，固件进入 `0x020046f0` 后在 `0x0200470a` BRK。因此不能把 K5 低电平作为默认触摸路径。

timer0 / 16-bit timer 可观察性：

- 已命名 `0x40147` 为 16-bit timer0 clock control。
- 已命名 `0x48180..0x48186` 为 timer0 compare/counter/control。
- 已命名 `0x40266` 为 `P16TM0_1`，并补 `EIR2/FIR2_16TM0_1`。
- 已实现 timer0 compare A/B dispatch metadata：comparison A 对应 vector `25`，comparison B 对应 vector `24`。
- 已实现 timer0 counter 的受控 host-time tick，用于观测 compare A/B 命中；compare B 命中后 counter 回到 0。
- `PRESET` 按手册作为 write-only 处理，写 1 清 counter，读 `0x48186` 时不再读回该位。
- 默认不自动生成 timer0-A/B factor；只记录 compare 命中日志。实验表明强行触发 timer0-B/vector `24` 会进入固件默认 BRK handler。
- 新增诊断属性 `debug-timer16-factors`，默认 `off`。只有打开后才允许 timer0 compare 命中置 `FIR2`，用于后续验证真实 IRQ/IDMA 路由。

## 已验证结果

### 串行触摸 ADC 和三点校准闭环

- 最新日志：`build\bbk9288s-test\run-qmp-touch-serialadc-calibrate3-exact-fpt6-20260723-013830.log`。
- 状态快照：`build\bbk9288s-test\run-qmp-touch-serialadc-calibrate3-exact-fpt6-20260723-013830.txt`。
- LCD：`build\bbk9288s-test\lcd-serialadc-calibrate3-exact-zoom3.png`。
- 固件内置三点目标由 `0x0209930c..0x02099316` 确认，顺序为 `(16,24)`、`(144,216)`、`(80,120)`。
- 三次点击均走真实默认硬件模型：FPT6/vector `70` 唤醒，Timer3-B/vector `42` 采样，`0x00300020` 串行 ADC 返回 X/Y；未启用 `debug-touch-ui-inject`。
- 三组原始坐标写入 `0x02292328..0x02292333`，`gfStep_X/gfStep_Y` 在 `0x02292318` 起始区域写入非零浮点值，证明固件校准函数通过验证。
- 校准结束后 LCD 正常进入主界面，可读出“听说、背诵、阅读、写作、翻译、词典、课程、语法、娱乐、工具”等入口。
- 校准后点击主界面“词典”，同一默认触摸路径可准确打开应用；修正 FAT
  中文短名后不再弹出“HZK_LIB.BIN 字库文件不存在”，而是进入词典正常
  启动提示“可以用USB线给机器供电!”。
- 宿主系统包中的 `D:\Downloads\步步高9288s系统文件\系统\数据\HZK_LIB.BIN` 大小为 `3,081,592` 字节；旧报错最终确认是 FAT 中文短名编码不兼容，不是下载缺失。当前 GBK 8.3 修补后 guest 已能通过该检查。
- 全流程没有 `BRK` 或 unimplemented opcode；此前第三点触发的 `0xb825` 已按 Epson C33 手册实现为 `adc %r5,%r2`，同组 `sbc` 也已补齐。
- 旧的 FPT4/event1/pen-cache 调查保留为历史排除证据，但其“真实触摸坐标入口尚未找到”结论已被本节取代。

### NAND、FTL 和完整系统资源闭环

- 启动反汇编和动态命令流确认 NAND 数据口为 `0x04000000`，P2.4/P2.5 分别驱动 CLE/ALE，P2.6 为 RY/BY，P51 bit1 为低有效 CE。
- 模拟器返回 Samsung `EC DA 10 95`，固件按 256 MiB large-page NAND 初始化：2 KiB data + 64-byte OOB、64 pages/block、2048 blocks。
- 板级 NAND 已支持 ID、status、page read、page program、block erase，以及 CPU/HSDMA 对同一数据口的访问；`nand-image` 可加载和退出时保存 276,824,064-byte raw+OOB 镜像。
- 空 NAND 由真实固件完成格式化后得到 MBR 和 FAT16：partition LBA `32`、32 sectors/cluster、224 reserved sectors、2 份 256-sector FAT、512 root entries。
- FTL 每个物理块承载 256 个 512-byte logical sectors；OOB 每 16-byte slot 重复块标签，正确编码为 `(LBN << 1) | parity(LBN)`。错误地省略 parity 时固件会清除约一半映射；修正后 1076 个映射重启保持不变且无 erase/program repair。
- 新增 `scripts\bbk9288s_nand_image.py`，支持 `info`、`extract`、`pack`、`install`：提取平面 FAT16、用 VFAT LFN 写入宿主 `系统` 树，把可放入 8.3 的中文名改写为固件可识别的 GBK 短名并同步 LFN checksum，再按物理 NAND/OOB 格式回包。
- 已生成 `build\bbk9288s-test\nand-system.raw` 和 `nand-system.fat.img`，写入 285 files / 137,874,541 bytes；raw image 使用 1076 logical blocks。
- `系统\数据\HZK_LIB.BIN` 在 FAT 中大小为 `3,081,592`，与宿主文件 SHA-256 均为 `db8b0697ab93c8defab6f54e407c3df9778a42aacc56a8dc2b9d30a64aef5fa2`。
- 挂载完整 NAND 后主菜单已加载“互动简介、听力测试、国际音标”等位图资源，不再是缺资源的纯文字界面，证明固件文件读取链已生效。
- 重启稳定性日志：`build\bbk9288s-test\nand-parity-reopen-fpt6-20260723-022302.log`；阶段截图：`build\bbk9288s-test\nand-ui-stages-fpt6-20260723-023117-contact.png`。
- “请重新设置时间!”根因已确认并修复：旧初始 SP `0x4000` 的第一次
  `pushn` 覆盖 `0x3ffc` RTC cookie；改为 `0x3f80` 并从 host RTC 初始化
  CTM 后提示消失。当前剩余 UI 缺口是词典自己的 USB 供电提示不能由
  旧临时触摸/键盘映射可靠关闭，不再是 RTC、NAND 或字库缺失。最终
  串行触摸 ADC 和六键硬件映射已经补齐，可可靠关闭该提示。
- 最终端到端日志：`build\bbk9288s-test\final-dictionary-localtime-fpt6-20260723-031423.log`；截图：`build\bbk9288s-test\final-dictionary-localtime-post.png`。状态栏为本地时间 `03:14`，无时间提示、`BRK` 或 unimplemented opcode，词典已进入正常 USB 供电提示。

### 海盗船端到端可玩闭环

- 修正串行触摸 ADC 的 Y 轴极性后，校准点击、主菜单“娱乐”和海盗船图标均能稳定命中。
- 实现 16-bit Timer2-B 系统节拍：固件设置 `PSCDT0=0`，从 48 MHz
  OSC3/PLL 时钟取源；`P16TS2=7` 选择 `/4096`，compare B=`292`，周期约
  24.9 ms。模型置 `FIR3 bit2`、复位计数器并投递 vector `38`。固件 ISR
  清因子并 post `event1/0x100`，应用由该硬件节拍接收 `MSG_TIMER=0x0144`。
- 修正共享 `FIR3` 的中断仲裁：Timer3-B 触摸优先级 4 高于 Timer2-B 系统节拍优先级 3，避免高频系统节拍吞掉触摸采样。
- 海盗船已从白屏推进到标题页和游戏棋盘；标题页可用触摸或确定键进入。全过程未使用 `debug-touch-ui-inject`、guest 内存写入或系统 API hook。
- 应用窗口回调动态记录确认六条实体输入线：`K5.3/K5.2/K5.1/K5.0` 分别产生上/下/左/右，`P0.5` 为确定，`P0.4` 为退出。主机映射为方向键、Enter/Space、Esc/Backspace。
- 最终回归依次执行“上、左、确定、下、右、确定、退出”：光标移动、行动提交、关卡/得分变化和退出确认框均正常。
- 最终截图：`build\bbk9288s-test\pirate-playable-fpt6-20260723-085131-key1-up.png`、`pirate-playable-fpt6-20260723-085131-key4-down.png`、`pirate-playable-fpt6-20260723-085131-key7-esc.png`。
- 本地构建已启用 GTK/SDL；`run-bbk9288s.cmd` 复用运行目录中的
  `nand-user.raw` 保存 guest 数据，并打开可交互窗口。
- 板级启动加载器现已扫描 NAND OOB FTL 映射、解析 FAT16 并递归查找
  `kernel.bin`。无 `-kernel` 参数的实测已加载 2,523,332 字节 KNL 文件，
  正确得到 `load=0x02000000`、`pc=0x0200468a`。

### Web 前端和局域网可玩闭环

- QEMU 使用内置 VNC 服务在 `127.0.0.1:5900` 提供显示，并在
  `0.0.0.0:6081` 提供 WebSocket；网页只通过这条标准显示/输入通道连接
  硬件模型。
- `web` 使用 noVNC 1.7、Vite 5 和 Lucide，提供 160x240 四灰阶像素显示、
  触摸层、方向键、确定、退出、自动重连和全屏。布局在 `1280x720` 与
  `390x844` 视口验证无横向或纵向溢出、控件遮挡。
- 浏览器瞬时触摸和按键都会保持 180 ms 后再发 RFB release，使原固件
  去抖及慢速按键矩阵扫描有足够采样时间；事件仍进入 QEMU 的 FPT6、
  串行触摸 ADC、K5/P0 GPIO 模型，没有 guest 内存注入、系统 API hook
  或应用级快捷通道。
- Web 端实测完成三点校准，关闭开机提示，依次点击“娱乐”和“海盗船”，
  从标题页进入棋盘。网页“上/左”使棋盘角色移动，“确定”提交行动后得分
  从 0 变为 60，证明手机触摸控件可以实际游玩。
- `run-bbk9288s-web.cmd` 构建静态前端，启动 `0.0.0.0:8000` HTTP 服务和
  QEMU WebSocket，并打印本机及局域网 URL。默认端口无认证和 TLS，只应
  暴露在可信局域网。

### 外部硬解码音频与 Web 播放闭环

- S1C33L05 手册没有片内音频/DAC 模块；固件的真实音频路径位于板级
  `0x003a0000` 窗口。`+0x02` 是命令/码流写口，`+0x0a` 的 `0x04/0x10`
  是读/写就绪，`0x00300020` 的低有效片选区分控制命令与压缩数据。
- 补齐 C33 `swap` / `swaph` 指令及反汇编后，固件 MP3 帧头解析不再在
  `0x0208b374` 停机。
- QEMU 新增板级音频解码器总线模型和 `audio-stream` 机器属性。模型只在
  固件选中压缩数据口时记录实际写入字节，不读取 NAND 文件、不调用 guest
  函数，也没有系统或应用 hook。
- 解码器输入端建模为 2 KiB FIFO，仅剩余空间足够容纳固件下一次 32 字节
  传输时拉高 DREQ；FIFO 按检测到的 MPEG 码率消耗，避免固件无限灌流并
  抢占应用执行。硬件输出达到 32 MiB 后轮转，限制运行目录增长。
- Web 服务从 QEMU 的硬件输出偏移持续等待新码流；浏览器按用户手势建立
  MediaSource，顺序追加 MP3、裁剪旧缓冲并在文件轮转后从零续流，提供
  扬声器静音按钮和音量滑杆，不会重复旧片段。
- 端到端回归依次完成三点校准、“国际音标 -> 元音音标 -> 发音”。QEMU
  运行中输出 `16,315` 字节，识别为 `44.1 kHz` 单声道、约 `1.01925 s`、
  `128 kbps` MP3；浏览器实测 `readyState=4`、播放进度递增、静音和
  `35%/80%` 音量切换有效。
- “娱乐 -> 雷霆战机 -> 挑战模式”回归中，游戏棋盘约每 0.2 秒刷新光标；
  浏览器开启声音后快速点击方向键会稳定移动一格且 QEMU 保持空闲等待。
  40 ms 按键脉冲会漏过该游戏的矩阵扫描，因此 Web 端统一使用 180 ms。
- 当前已验证 MP3 路径；固件声明的 WMA/WAV/MID/BLM 仍需分别确认它们
  是否复用同一硬解码总线和浏览器支持情况。

### NAND Web 文件管理闭环

- 新增 `scripts\bbk9288s_web_server.py`，统一托管静态 Web、NAND API 和
  QEMU 生命周期；QMP 只监听 `127.0.0.1:6082`，用于正常停止/重启机器。
- Web 新增“模拟器 / NAND 文件”页签。文件页支持容量统计、面包屑目录、
  中文目录浏览、多文件上传、下载、新建目录、重命名和递归删除，桌面与
  `390x844` 手机视口均完成布局验证。
- 文件修改使用显式维护事务：QMP `quit` 先让板级 NAND 模型保存
  `nand-user.raw`，再提取 FAT16；操作只写暂存 flat image。“应用并重启”
  会补 GBK 8.3 短名校验、按真实 FTL tag/parity 和 OOB 布局回包后启动
  QEMU；“放弃”删除暂存修改后直接重启。
- `pyfatfs` 的 GBK 多字节 8.3 填充会破坏扩展名，因此管理器用 IBM437
  生成 ASCII SFN + VFAT LFN，并对现有纯 GBK SFN 做显示路径映射；最终
  回包继续调用 `patch_gbk_short_names`，兼顾网页中文名和固件字节路径。
- 独立 NAND 副本回归已完成创建、上传、下载 SHA-256 一致、中文重命名、
  中文嵌套目录、递归删除和容量统计。真实 `nand-user.raw` 又完成一次
  “停机提取 -> 暂存修改 -> 回包 -> QEMU 重启 -> noVNC 重连”，随后重新
  校准并进入海盗船棋盘，方向键仍可移动角色。

构建和静态检查：

- `ninja -C E:\eebbk9288s-qemu\build qemu-system-s1c33.exe` 通过。
- `git diff --check -- hw/s1c33/bbk9288s.c target/s1c33/helper.c` 通过。
- 构建日志里仍有一个既有 `HAVE_CMPXCHG128` warning，不是本轮代码引入。
- 2026-07-23 复核：
  - `python -m py_compile scripts\bbk9288s_qmp_touch_trace.py scripts\bbk9288s_nand_image.py scripts\bbk9288s_disas_annotate.py scripts\bbk9288s_find_u32.py scripts\bbk9288s_static_refs.py` 通过。
  - `git diff --check -- hw\s1c33\bbk9288s.c plan.md README.bbk9288s.md` 通过。
  - `ninja -C build qemu-system-s1c33.exe` 通过。

默认固件 smoke：

- 日志：`build\bbk9288s-test\run-default-timer16-metadata.log`
- 退出：host timeout `124`，符合 `exit-on-halt=off` smoke 的预期。
- 无 `BRK`。
- 无 unimplemented opcode。
- 无 unknown pending source。
- 无 vector `24`。

CTM smoke：

- 日志：`build\bbk9288s-test\run-default-ctm-smoke.log`
- 退出：host timeout `124`，符合 `exit-on-halt=off` smoke 的预期。
- `BRK` / unimplemented / unknown pending：0
- vector `65` 日志行：244
- `bbk9288s-ctm` 日志行：247
- 说明：该日志来自旧的“无条件 32Hz CTM factor”模型，现已判定过激。当前 CTM 仍计数，但只有 `TCISE` 选择到对应频率时才置 `TCIF/FCTM`。

QMP touch + CTM/vector65 旧验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-ctm-vector65.log`
- CPU 参数：`trace-exec=on,trace-exec-start=0x02004998,trace-exec-end=0x02004bb8,trace-mem=on,trace-mem-pc-start=0x02004998,trace-mem-pc-end=0x02004bb8`
- QEMU 正常退出。
- `BRK` / unimplemented / unknown pending：0
- `bbk9288s-ctm` 日志行：173
- vector `65` 日志行：162
- `0x02004998..0x02004bb8` 范围 `s1c33-exec`：1283 行。
- 该范围 `s1c33-mem`：1368 行。
- HMP 反汇编确认 vector65 handler 在 `0x020049e8` 读 `0x40152`，不是 `0x40112`。
- CTM tick 置 `TCIF/FCTM` 后，固件读到 `TCINT=0xee`，进入 `0x02004a14`。
- `0x02004a14` 队列路径开始写 gate：
  - `0x02004ace -> 0x02380002 = 1`
  - `0x02004afc -> 0x02380004 = 1`
  - `0x02004b2a -> 0x02380006 = 1`
- 废弃结论：事件分发器随后调用 `0x020680b8` / `0x020678c8` 并触发的 AD0 采样，曾被误判为 touch-down ADC。
- 修正结论：`0x020678c8` 实际连续采样 AD0，用作电池/电源状态检查；把 AD0 绑定到触摸 X 会在左上角点击时得到低值并触发“电池电量过低，系统即将关闭!”弹窗。
- 该日志中 CTM factor 来自旧模型对 `TCISE=111` 的错误处理，不能再作为真实触摸坐标入口证据。

LCD/CTM/AD0 修正后验证：

- 当前复验日志：`build\bbk9288s-test\run-lcd-dump-recheck-now.log`
- 当前复验图像：`build\bbk9288s-test\lcd-dump-recheck-now-zoom3.png`
- 最新复验日志：`build\bbk9288s-test\run-lcd-dump-current.log`
- 最新复验图像：`build\bbk9288s-test\lcd-dump-current-zoom3.png`
- 日志：`build\bbk9288s-test\run-lcd-dump-ctmselect-calib-5s.log`
- 图像：`build\bbk9288s-test\lcd-dump-ctmselect-calib-5s-zoom3.png`
- 日志：`build\bbk9288s-test\run-lcd-dump-ctmselect-click1-5s.log`
- 图像：`build\bbk9288s-test\lcd-dump-ctmselect-click1-5s-zoom3.png`
- QEMU 正常退出。
- `BRK` / unimplemented / unknown pending：0
- `bbk9288s-ctm: factor set`：0，符合固件 `TCISE=111` 不产生周期 CTM 中断的语义。
- `bbk9288s-adc` complete：6，均为启动/电池检查 AD0 样本，`add=0x3c0`。
- 该旧 FPT4 日志中，touch down/up 只能触发 port wake 和 vector `68`/`53` debounce，未看到触摸坐标采样；后续已由 FPT6 + `0x00300020` 串行 ADC 路径取代。
- 校准页画面清晰显示“请点击 / 十字光标中心”和左上十字，无低电量或自动关机弹窗。
- 2026-07-23 复验的 headless PGM 仍为清晰校准页，日志灰阶统计为 `gray-counts=551,0,0,37849`；若窗口画面仍乱，优先检查是否在运行旧 `qemu-system-s1c33.exe` 或不同 display backend，而不是继续改 LCD 2bpp bit order。

QMP touch/keyinput/timer16 metadata 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-keyinput-timer16meta.log`
- QEMU 正常退出。
- `BRK` / unimplemented：0
- unknown pending：0
- vector `68`：1
- vector `53`：71
- vector `16`：0
- vector `17`：0
- vector `24`：0
- `FK0/FK1 factor set`：0
- touch-down ADC complete：0
- ADC complete 总数：6，仍是启动阶段 touch-up/open 样本。
- `0x40147` 已按 timer0 register metadata 命名，不再按 unknown reg 记录。
- `0x40262` key priority register 已按 metadata 命名。

QMP touch 聚焦 trace-mem 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-tracemem-filter.log`
- CPU 参数：`trace-mem=on,trace-mem-start=0x022f8500,trace-mem-end=0x023800c0`
- QEMU 正常退出。
- 日志大小约 `1.3 MiB`，较完整 memory trace 明显降低。
- `s1c33-mem` 行数：`7267`
- `BRK` / unimplemented：0
- unknown pending：0
- touch pen down/up：各 1
- vector `68`：1
- vector `53`：76
- vector `16/17/24`：0
- ADC complete 总数：6，touch-down ADC complete：0
- watch 结果：
  - `0x022f8569`：77 次访问，0 次写入。
  - `0x022f856a`：77 次访问，0 次写入。
  - `0x022f8554`：78 次访问，1 次写入，写入 PC `0x0206208e`，值 `0x00`。
  - `0x02380002`：79 次访问，1 次写入，启动清 0。
  - `0x02380004`：155 次访问，77 次写入，全部清 0。
  - `0x02380008`：155 次访问，77 次写入，全部清 0。
  - `0x023800b0`：84 次访问，6 次写入，其中 `0x02008054 -> 1`、`0x02008194 -> 2`、`0x020080a4 -> 3`。

QMP touch 表区聚焦 trace-mem 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-tracemem-table-022109b0.log`
- CPU 参数：`trace-mem=on,trace-mem-start=0x022109b0,trace-mem-end=0x022109d0`
- QEMU 正常退出。
- `s1c33-mem` 行数：4
- 写入：0
- `BRK` / unimplemented / unknown pending：0
- 读到的表/配置值：
  - `0x0206206c` 读 `0x022109b8 = 0x78`
  - `0x02062194` 读 `0x022109c8 = 0x64`
  - `0x020621d6` 读 `0x022109c9 = 0x64`
  - `0x020622fe` 读 `0x022109cc = 0x48`
- 已直接核对 KNL payload：这些值分别位于文件偏移 `0x2109f8`、`0x210a08`、`0x210a09`、`0x210a0c`，是固件静态数据。

QMP touch 函数级 trace-mem 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-tracemem-pc-02061f40.log`
- CPU 参数：`trace-mem=on,trace-mem-pc-start=0x02061f40,trace-mem-pc-end=0x020623d2`
- QEMU 正常退出。
- `s1c33-mem` 行数：26
- `BRK` / unimplemented / unknown pending：0
- `0x02061f40` 函数级切片确认：
  - `0x022f8554`：`0x02061f50` 读 0，`0x0206208e` 写 0。
  - `0x022f8558`：`0x02061f7c` 读 0，`0x020621aa` / `0x02062294` 写 0。
  - `0x022f855c`：`0x02061f8a` 读 0，`0x020621ee` / `0x020622b6` 写 0。
  - `0x022f8560`：`0x02061fde` 读 0，`0x020622d8` 写 0。
  - `0x022f8564`：`0x02061f98` 读 0，`0x020621b0` 写 0。
  - `0x022f8565`：`0x02061fa6` 读 0，`0x020621f4` 写 0。
  - `0x022f8568`：`0x02061fb4` 读 0，`0x02062320` 写 0。
  - `0x022f8569`：`0x02061fc2` 读 0，当前切片无写入。
  - `0x022f856a`：`0x02061fd0` 读 0，当前切片无写入。

静态 writer/caller 扫描：

- `0x022f8569` 的实际 store 指令在 `0x0206236e`，由 helper `0x02062340` 执行。
- `0x022f856a` 的实际 store 指令在 `0x020623bc`，由 helper `0x0206238e` 执行。
- 两个 helper 都先执行 `ld.ub r4,r6; cmp r4,0; jreq`，所以输入 `r6 == 0` 时直接返回，不写目标变量。
- 直接 caller：
  - `0x020614cc -> 0x02062340`
  - `0x0206153a -> 0x0206238e`
  - `0x02061a28 -> 0x02062340`
  - `0x02061a38 -> 0x0206238e`
  - `0x02061fc8 -> 0x02062340`
  - `0x02061fd6 -> 0x0206238e`
- 当前 QMP touch 路径只执行 `0x02061fc8/1fd6` 这组归一化调用，输入来自 `0x022f8569/6a` 本身且为 0。
- `0x020614cc/153a/1a28/1a38` 这两组更像真实 setter，会先从 `0x0238f148` 取 byte 后传入 helper；当前 QMP touch 路径没有执行这些 caller。

QMP touch 中间状态 trace-mem 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-tracemem-0238f140.log`
- CPU 参数：`trace-mem=on,trace-mem-start=0x0238f140,trace-mem-end=0x0238f160`
- QEMU 正常退出。
- `s1c33-mem` 行数：9
- `BRK` / unimplemented / unknown pending：0
- `0x0238f140..0x0238f160` 只有 `0x0200478c` 启动清 0。
- `0x0238f148` 当前触摸路径没有运行时读写，说明 setter 分支的中间输入也没有被当前触摸流程置位。

QMP touch 前台 setter 范围 trace 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-tracemem-pc-02060000.log`
- CPU 参数：`trace-mem=on,trace-mem-pc-start=0x02060000,trace-mem-pc-end=0x02061a70`
- 单次 touch down/up：QEMU 正常退出，`s1c33-mem` 行数 0。
- 日志：`build\bbk9288s-test\run-qmp-touch2-tracemem-pc-02060000.log`
- 双次 touch down/up：QEMU 正常退出，`s1c33-mem` 行数 0，touch down/up 各 2，vector `68` 为 2，vector `53` 为 136。
- 结论：当前触摸次数不是主要问题；`0x02060000..0x02061a70` 这片前台 setter/转换代码没有被当前触摸流程调度。

QMP touch trace-exec 验证：

- 日志：`build\bbk9288s-test\run-qmp-touch2-traceexec-02060000.log`
- CPU 参数：`trace-exec=on,trace-exec-start=0x02060000,trace-exec-end=0x02061a70`
- 双次 touch down/up：QEMU 正常退出，`s1c33-exec` 行数 0。
- 日志：`build\bbk9288s-test\run-qmp-touch-traceexec-02008000.log`
- CPU 参数：`trace-exec=on,trace-exec-start=0x02008000,trace-exec-end=0x02008260`
- 单次 touch down/up：QEMU 正常退出，`s1c33-exec` 行数 2129。
- 正向验证显示 `trace-exec` 工作正常；`0x02060000..0x02061a70` 的空结果说明该范围确实未执行。

QMP touch 长按无日志验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-longhold-nolog.txt`
- 图像：`build\bbk9288s-test\lcd-dump-touch-longhold-nolog-zoom3.png`
- QMP 在左上校准十字附近按下约 7.5 秒，再释放，QEMU 正常退出。
- 此轮未启用 `-d guest_errors`，避免 8TM underflow 日志拖慢 timer。
- 长按后 PGM dump 与当前校准页 `lcd-dump-current.pgm` 的 SHA256 完全一致。
- 结论：不是日志拖慢或按住时间不够导致 UI 未推进；校准画面确实没有响应当前 touch 模型。

QMP touch 长按 HMP 内存读验证：

- 日志：`build\bbk9288s-test\run-qmp-touch-longhold-xp.txt`
- 用 QMP `human-monitor-command` / HMP `xp` 在长按期间直接读取 RAM/队列状态，避免开启 guest trace。
- 长按期间 `0x022681e0..` 关键状态：
  - `0x022681ec = 0x2f`
  - `0x022681f4 = 1`
  - `0x022681f5 = 0x2f`
  - `0x022681f6 = 1`
  - `0x022681f8 = 0`
  - `0x022681fa = 0x2f`
- 长按期间 `0x023800b0 = 2`，说明 debounce 已确认稳定按下。
- 长按期间 `0x02380002/04/06/08` 仍为 0，`0x022f8569/6a/54` 仍为 0。
- 释放后一秒这些 gate 和坐标缓存仍未变成非零。
- 结论：当前模型已能进入稳定按下状态，但还没有触发前台事件 gate 或触摸坐标采样。

QMP touch K5.0 负向实验：

- 日志：`build\bbk9288s-test\run-qmp-touch-k5mask01-state.log`
- 参数：`-M bbk9288s,trace-key-scan=on,touch-k5-low-mask=0x01,...`
- pen down 时日志显示：
  - `k5_low=0x01`
  - `FK0 factor set by touch-pen source=K5[4:0] old=0x1f new=0x1e compare=0x0f mask=0x0f`
  - `deliver source=key-input0 vector=16`
  - `IRQ vector=16 handler=0x020046f0`
- 固件随后在 `0x0200470a` 执行 BRK，QEMU 退出。
- 结论：当前固件对 key-input0/vector16 没有可用处理路径，K5.0 不匹配触摸校准入口。

QMP touch 关闭 P0D 低电平实验：

- 日志：`build\bbk9288s-test\run-qmp-touch-p0low-off.log`
- 图像：`build\bbk9288s-test\lcd-dump-touch-p0low-off-zoom3.png`
- 参数：`-M bbk9288s,trace-key-scan=on,touch-p0-low=off,...`
- QEMU 正常退出。
- `BRK` / unimplemented / unknown pending：0
- pen down/up 各 1。
- port4 / vector `68` 仍触发。
- 8TM / vector `53` 只触发 1 次。
- vector53 读取：
  - `K5D = 0x1f`
  - `P0D = 0xfb`
  - `touch-low=0x00`
- `0x02008218` 写 `0x023800b0 = 0`。
- ADC complete 仍只有启动阶段 6 次 AD0 电池样本。
- PGM dump 与当前校准页 SHA256 完全一致。
- 结论：关闭 P0D touch-low 不会打开坐标路径；P0D low 是当前 debounce 路径识别触摸/按键状态所需输入，但它本身仍不足以触发触摸坐标采样。

QMP touch IRAM 省电循环 trace：

- 日志：`build\bbk9288s-test\run-qmp-touch-iram-tracemem-longhold.log`
- CPU 参数：`trace-mem=on,trace-mem-pc-start=0x00001c50,trace-mem-pc-end=0x00001d60`
- 为越过带日志时的 debounce timing，QMP 按住约 40 秒，再释放。
- `BRK` / unimplemented / unknown pending：0
- vector `68`：1
- vector `53`：102
- 8TM1 sleep resume：约 2999 次。
- `0x00001c18` 是 IRAM 轮询 `P0.4` 释放的等待点。
- 释放后进入 `0x00001cb4` `SLP` 循环；8TM1 能恢复 `SLP`，不是 8TM 唤醒缺失。
- 该循环每轮关键访存：
  - `0x00001c56 -> 0x00003fc8 = 0`
  - `0x00001cba` 读 `0x00003fc8 = 0`
  - `0x00001d52` 读 `0x00003fc8 = 0`
  - `0x00001ca4` 读/清 `FIR5_8TM0_3 = 0x02`
  - `0x00001ca6/1ca8` 重新启动 `T8_CTRL1`
- 结论：稳定按下后固件最终会进正常 IRAM 省电循环，8TM1 OSC3 稳定等待能工作；问题仍是前台事件 gate/坐标状态没有被置非零，而不是 CPU 卡死或 8TM1 不恢复。

前台 gate/static writer 扫描：

- `0x0238f148` 静态访问点集中在 `0x0206025a..0x02061a4c`，其中大量 writer 位于 `0x020602ac`、`0x02060374`、`0x02060466`、`0x02060562`、`0x020605fe`、`0x020606c6`、`0x0206077c`、`0x0206083c`、`0x020608b2`、`0x02060f42`、`0x02061044`、`0x0206109a`、`0x020610fa`、`0x0206118c`、`0x02061224`、`0x020612a2`、`0x02061322`、`0x02061470`、`0x020614de`、`0x02061568`、`0x02061844`、`0x02061902`、`0x02061916`、`0x02061994`。
- `0x02380002` 静态 writer 包括 `0x020047a2`、`0x02004aca`、`0x02004b6e`、`0x02007b98`、`0x02007bb2`、`0x02007bd8`、`0x020088c2`、`0x020681fe..` 附近、`0x02088fce`、`0x02089bf4`。
- `0x02380004` 静态 writer 包括 `0x020047b4`、`0x02004af8`、`0x02004b80`、`0x02007c12`、`0x0200815a`、`0x020081ee`、`0x02008892`、`0x02009bcc`、`0x02009c7c`、`0x02067a50`、`0x020682a8`。
- `0x02380008` 静态 writer 包括 `0x020047d8`、`0x02004b5e`、`0x02004ba4`、`0x02007c60`、`0x02008166`、`0x020081fa`、`0x0200891e`、`0x02009bd8`、`0x02009c88`、`0x02067a7c`。
- 当前动态 touch 日志只命中初始化清零和 vector53 debounce 清零；这些更早的前台 setter/gate 写入路径没有被触发。

触摸路径已确认：

- QMP pointer 事件进入 QEMU。
- touch pen-down 能触发 port input 4 / vector `68`。
- P0D 能读到 P04 low。
- K5D 能读到全高 `0x1f`。
- 8-bit timer debounce / vector `53` 在跑，并能生成 stable value `0x2f`。
- HALT IRQ 返回 PC 已正确保存为 `0x0000006c`。
- `0x0200839c` 已被调用；后续 IRAM 例程会在 `0x00001c18` 等 P0.4 释放。
- pen up 后 vector `53` release handler 会把 `0x023800b0` 置为 `0x03`。
- release 事件分发路径清 `0x023800b0`、设置 `0x023800b2=1`、写 `0x02380768=1`，再检查 `0x02380004/02/08` 都为 0 后回到 HALT。
- FPT4/FPT5 能进入当前 debounce/event 路径。
- FPT6/FPT7 当前不会进入同一路径。

## 历史排除记录（当前卡点已解决）

本轮已修正两个会误导后续排查的问题：

> LCD 2bpp bit order 修正后，校准页已经清晰可读；旧“画面乱”不是字体或分辨率问题。

> CTM 必须遵守 `TCISE[2:0]`。固件当前写入的 `TCISE=111` 是 no interrupt，旧模型无条件产生 vector `65` 会错误触发自动关机/队列路径。

以下是串行触摸 ADC 路径确认前的历史卡点，现已解决：

> 当时 touch down 已能触发 port wake 和 vector `53` debounce，但尚未找到坐标入口。最终确认真实路径是 P06/FPT6、Timer3-B/vector `42` 和板级串行触摸 ADC `0x00300020`；AD0 仍是电池/电源检查。

当前 debounce/event 分支理解：

- `0x02008000..` 是 vector `53` debounce handler。
- `0x0200802a..0x02008042` 在 `0x022681f4 == 0` 时读取 `K5D & 0x0f` 和 P0 high nibble，把第一次扫描值写到 `0x022681f5`。
- `0x02008054` 写 `0x023800b0 = 1`。
- `0x0200805a` 写 `0x022681f4 = 1`。
- `0x02008172..0x0200819c` 在稳定扫描值匹配第一次扫描值时写：
  - `0x023800b0 = 2`
  - `0x022681f6 = 1`
  - `0x022681f8 = 0`
  - `0x022681ec = stable value`
- `0x0200807a..0x02008094` 重复扫描路径把 repeat scan value 写到 `0x022681fa`。
- `0x02008096` 的 `ld.ub %r4, %r4` 是寄存器 byte zero-extend，不是查表。
- `0x0200809a` 实际比较的是 `0x3f`。
- `0x023800b0 = 3` 是 release/all-high 分支。
- pen down stable value `0x2f` 会走到 `0x020080e2`。
- `0x020080e2` 读取 `0x022681fa` 并测试 bit4；`0x2f & 0x10 == 0`，所以不会走 release 分支，而是继续 debounce/event-copy 路径。
- `0x020080f2..0x02008102` 会递增 `0x022681f8` 并与约 `0x01e3` 的长按阈值比较；无日志长按后的 HMP 读数显示阈值路径走完后状态停在 `0x023800b0=2`，但没有打开前台 gate。
- `touch-p0-low=off` 时，vector53 读到 P0D 全高输入并走 `0x02008218` 清 `0x023800b0`，不会进入稳定按下路径。
- `0x02008138/44/50` 和 `0x020081ce/da/e6` 读取：
  - `0x022f8569`
  - `0x022f856a`
  - `0x022f8554`
- 然后写入：
  - `0x02380003`
  - `0x02380005`
  - `0x02380007`
- 随后清：
  - `0x02380004`
  - `0x02380006`
  - `0x02380008`

事件分发路径理解：

- `0x0200881e..0x0200894a` 读取 `0x023800b0`。
- 如果 `0x023800b0 == 3`，它会清 `0x023800b0`、切换 `0x023800b2`、写 `0x02380768 = 1`。
- 之后分发器检查：
  - `0x022681fb`
  - `0x02380004`
  - `0x02380002`
  - `0x02380008`
- 旧日志中这些 gate/queue 变量都是 0，所以前台进入 IRAM low-power/HALT 路径，没有启动触摸坐标采样。
- 旧 CTM 日志中 `0x02004a14` 把 `0x02380002/04/06` 写 1，是由错误的无条件 CTM 周期中断制造出来的路径；当前模型不再把它当作真实触摸 gate。
- `0x020678c8` 触发的 AD0 采样已改判为电池/电源检查；不能再用它证明 touch-down ADC 已打通。

长按 latch / timer4/5 验证：

- 日志：
  - `build\bbk9288s-test\run-qmp-touch-longhold-681fb-20260722-214442.log`
  - `build\bbk9288s-test\run-qmp-touch-timer45-meta-20260722-221619.log`
  - `build\bbk9288s-test\run-qmp-touch-timer45-meta-20260722-221619.txt`
- `0x02008000` vector53 去抖路径确认会在稳定按下累计到 99 个 8TM tick 后写 `0x022681fb = 1`：
  - `0x020080f2` 读 `0x022681f8 = 0x0063`
  - `0x020080fa` 写 `0x022681f8 = 0x0064`
  - `0x0200810a` 写 `0x022681fb = 1`
  - `0x02008860` 读到 1，`0x0200886c` 清 0
- 99 tick 来自 S1C33 `ext` 扩展语义；旧反汇编表面显示的 `cmp ... 0xffffffe3` 不能按未扩展的负数理解。
- `0x022681fb` 被消费后，固件进入 `0x0200867c`，随后执行 `0x0208ab52` 一组 timer4 配置：
  - `0x481a2 = T16_CMPB4`
  - `0x481a6 = T16_CTRL4`
  - `0x4014b = T16_CLK4`
  - `0x481ae = T16_CTRL5`
- 已按 Epson S1C33L05 register map 给 timer0..5 补齐 metadata；`0x481a2/0x481a6/0x4014b/0x481ae` 不再是 unknown。
- `run-qmp-touch-timer45-meta-20260722-221619.log` 中：
  - `EIR4_16TM4_5` 保持 0。
  - `FIR4_16TM4_5` 没有被读写。
  - 没有 BRK 或 unimplemented opcode。
  - timer4 被启动后很快在 `0x0208a926/0x0208aa48` 清掉；当前证据不支持无条件合成 timer4/5 IRQ。
- 长按事件后固件进入低地址 RAM 例程，保存/关闭 ITC、timer、GPIO、CTM 并切 `TTBR = 0x00002400`；这更像 power/idle 保存路径，而不是触摸坐标采样路径。

短按 release / FPT sweep / ADC 快照：

- 新增可复用脚本：`scripts/bbk9288s_qmp_touch_trace.py`。
  - 默认点击校准十字附近 `(14,23)`。
  - QMP absolute 坐标范围按 QEMU `INPUT_EVENT_ABS_MAX = 0x7fff` 计算；旧临时脚本误用 `0x7fffffff` 会把候选 ADC 坐标压到右下角，但不影响“固件未读 AD1/AD2”的结论。
  - 默认输出 `.log`、`.txt` 快照并汇总 vector、ADC completion、`0x02380002/04/08/b0` gate 写入。
- 日志：
  - `build\bbk9288s-test\run-qmp-touch-short-gates-20260722-222646.log`
  - `build\bbk9288s-test\run-qmp-touch-short-gates-20260722-222646.txt`
  - `build\bbk9288s-test\run-qmp-touch-fpt-sweep-20260722-222911.json`
  - `build\bbk9288s-test\run-qmp-touch-script-check-fpt4-20260722-223155.log`
  - `build\bbk9288s-test\run-qmp-touch-script-check-fpt4-20260722-223155.txt`
  - `build\bbk9288s-test\run-qmp-touch-adc-snapshot-fpt4-20260722-223244.log`
  - `build\bbk9288s-test\run-qmp-touch-adc-snapshot-fpt4-20260722-223244.txt`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x1-fpt4-20260722-223610.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x2-fpt4-20260722-223612.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x4-fpt4-20260722-223614.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x8-fpt4-20260722-223616.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x10-fpt4-20260722-223618.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x2-script-early-fpt4-20260722-223728.log`
  - `build\bbk9288s-test\run-qmp-touch-k5mask0x2-script-early-fpt4-20260722-223728.txt`
- FPT4 短按 release 路径已确认：
  - `port4` vector `68` 先触发。
  - 8-bit timer debounce vector `53` 周期触发。
  - touch down 时 `P0D = 0xeb`，`touch-low = 0x10`，说明 P0.4 低电平对固件可见。
  - `0x023800b0` 按下后 `0 -> 1 -> 2`，松开后 `3 -> 0`。
  - `0x02380002`、`0x02380004`、`0x02380008` 没有非零写入；前台 dispatcher 读到的 gate 仍为 0。
- FPT sweep：
  - FPT4：`port4` vector `68`，`b0=1/2/3`，gate 仍为 0。
  - FPT5：`port5` vector `69`，`b0=1/2/3`，gate 仍为 0。
  - FPT6：`port6` vector `70` 后进入大量 `16tm3-b`，没有 `b0=1/2/3`。
  - FPT7：没有有效 touch/down 去抖路径。
  - 当时结论：默认 FPT4 最合理。该结论已被后续 FPT6/vector70 + Timer3-B/vector42 + 串行 ADC 的完整校准实测推翻。
- 修正坐标范围后的 FPT4 日志显示候选值 `adc-x=84 adc-y=94`，但 ADC completion 仍只有 `ch=0..0`、`touch=up` 的启动/电源采样。
- ADC 快照：
  - `0x00040240`: `ADD=0x03c0`、`ADTRG=0x00`、`ADCH=0x00`、`ADCTL=0x10`、`ADSMP=0x03`、`ADF_FLAGS=0x01`。
  - touch 后固件没有把 `ADCH` 切到 AD1/AD2，也没有配置外部触发模式。
- K5 sweep：
  - `touch-k5-low-mask=0x01/0x02/0x04/0x08` 都触发 `key-input0` vector `16`，随后 BRK；这些 K5 bit 不能作为默认触摸附加线。
  - `touch-k5-low-mask=0x10` 不 BRK，但仍只产生 FPT4 + vector53 debounce，`0x02380002/04/08` 没有非零写入，ADC 仍只有 AD0。
  - trace 脚本已改成 QEMU 早退时也保留 state 文件并汇总 log；早退本身作为异常路径证据处理。

已查到但还没解释清楚：

- `0x02061f40` 是一段前台/事件状态同步函数，由 `0x0203883a` 调用。
- `0x02061f40` 会读 `0x022f8554/58/5c/60/64/65/68/69/6a`，并调用 `0x02062064`、`0x0206218e`、`0x020621ce`、`0x02062290`、`0x020622b2`、`0x020622d4`、`0x020622f6`、`0x02062340`、`0x0206238e` 这类小 helper 做归一化/复制。
- `0x02062064` 被 `0x02061f56` 调用，当前参数下只把 `0x022f8554` 写回 0；它还读了静态表/配置字节 `0x022109b8 = 0x78`。
- `0x022109b8/c8/c9/cc` 附近表值是 KNL 静态 payload，不是运行时初始化缺失。
- `0x022f8569/6a` 的 writer 存在，但当前只执行到零输入归一化路径；真正 setter 依赖 `0x0238f148` 的非零中间值。
- `0x0238f148` 当前只被启动清零，QMP touch down/up 没有让前台执行 setter 分支。
- `trace-exec` 已证明，即使 down/up 两次，`0x02060000..0x02061a70` 前台 setter/转换代码也没有执行。
- `0x022f8569` / `0x022f856a` / `0x022f8554` 在当前 touch 日志中均未出现非零写入。
- 旧真实 touch 日志中 `0x02380002` / `0x02380004` / `0x02380008` 没有非零写入；旧 CTM 实验中看到的 `0x02380002` 和 `0x02380004` 置 1 来自错误 CTM factor，不再作为当前依据。
- K5.0 人为拉低会立即进入 vector16/BRK，已排除为默认触摸缺失项。
- K5.1..K5.3 人为拉低同样进入 vector16/BRK；K5.4 不 BRK但不打开坐标 gate。
- 关闭 P0D touch-low 不会让固件进入坐标采样，已排除“P0D 低电平干扰了触摸路径”的假设。
- helper `0x02008428` 在 `r6 == 0` 时会清 `P2D` bit3；该路径出现在 release 处理附近。

本轮继续复核：

- `scripts/bbk9288s_qmp_touch_trace.py` 增加 `--no-touch`、`--no-default-snapshots`、`--watch-addr`，便于只取 HMP 反汇编/快照，并对任意内存地址做访问和非零写入汇总。
- 干净反汇编日志：
  - `build\bbk9288s-test\run-qmp-disas-adc-driver-fpt4-20260722-224930.txt`
  - `build\bbk9288s-test\run-qmp-disas-adc-driver2-fpt4-20260722-224945.txt`
  - `build\bbk9288s-test\run-qmp-disas-pen-setters-fpt4-20260722-225020.txt`
  - `build\bbk9288s-test\run-qmp-disas-front-dispatch-fpt4-20260722-225047.txt`
  - `build\bbk9288s-test\run-qmp-disas-debounce-tail-fpt4-20260722-225103.txt`
  - `build\bbk9288s-test\run-qmp-disas-pen-notify-fpt4-20260722-225122.txt`
- `0x02067770..0x02067814` 是同步 ADC 采样 helper：配置/等待 ADC、读 `ADD`、清标志；当前执行时仍只采 `AD0`。
- `0x02067834` / `0x020678c8` 是连续 AD0 采样与阈值判断路径，继续按电池/电源检查处理，不作为触摸坐标入口。
- `0x020622f6`、`0x02062340`、`0x0206238e` 这些 setter 会把输入写入 `0x022f8568/69/6a`；当输入为 0 时直接返回或清零，非真实坐标。
- setter 的通知路径会围绕 `0x022f8554` 调软件队列/通知 helper，但当前 touch trace 没有给 setter 提供非零输入。
- 新动态验证日志：`build\bbk9288s-test\run-qmp-touch-watch-summary-fpt4-20260722-225150.log`。
  - `port4` vector `68` 和 `8tm0-3` vector `53` 仍正常触发。
  - ADC completion 仍只有 6 次 `ch=0..0`、`add=0x03c0` 的启动/电源样本。
  - `0x022f8554`、`0x022f8569`、`0x022f856a` 有读访问但无非零写入。
- `0x022f8560` 唯一非零写入来自 `0x0203885a`，更像 PEN 同步/初始化活动标志，不是触摸坐标。
- `0x02061f40` 同步函数和 `0x02062340/8e` setter 被执行过，但输入仍为 0，说明当前模型已到达 PEN 状态同步层，却没有触发产生坐标的上游条件。
- `build\bbk9288s-test\run-qmp-touch-traceio-board-short-fpt4-20260722-225445.log` 用 `trace-io=on` 复核后，触摸阶段未出现新的板级 I/O 访问；主要仍是 `K5D/P0D` 防抖读数。
- `build\bbk9288s-test\run-qmp-disas-p2d-helper-fpt4-20260722-225509.txt` 显示 release 附近的 `0x02008428` helper 只切换 `P2D` bit3，未直接触发 ADC 或 PEN setter 非零输入。

本轮新增静态注释和 UI gate 诊断：

- 新增 `scripts/bbk9288s_disas_annotate.py`，可按地址区间反汇编 KNL，并注释 `EXT + [%r15]` 形式的绝对 RAM/MMIO 访问、call/jump 目标和已知 guest 变量。
- 新增 `scripts/bbk9288s_find_u32.py`，用于在 KNL payload 中查找小端 32-bit 函数指针。对 `0x0206000c`、`0x0205f73a`、`0x0205fb4a`、`0x0203863e`、`0x0203883a` 的原始指针搜索未命中；这些回调地址目前看起来是在代码中通过 `EXT` immediate 构造后写入对象结构，不是静态 raw pointer table。
- `0x0200881e..0x02008946` 前台 dispatcher 会消费：
  - `0x02380004` -> 清 gate 并调用 `0x020680b8(r6=4)`。
  - `0x02380002` -> 清 gate 并调用 `0x020678c8`，这仍是 AD0 电源检查路径。
  - `0x02380008` -> 清 gate、切 `0x023800b2` 并调用 `0x02008428`。
- `0x02007b40..0x02007c72` 是另一组 gate dispatcher：
  - `0x02380002` 非零会先调 `0x020678c8` 做 AD0 电源检查，再按 `0x02292360` 结果调 `0x020680b8` 或清 gate。
  - `0x02380004` 非零会清 gate 并调用 `0x020680b8(r6=4)`。
  - `0x02380006` 与 `0x02380009` 同时非零才会调用 `0x02067a02`；当前默认触摸和完整 gate 诊断都没有自然写入 `0x02380009`。
  - `0x02380008` 非零会清 gate 并调用 `0x020679f8`。
- `0x020681f2..0x020682b8` 在 `0x020680b8` 内部重复检查 `0x00003fc9`、`0x02380002/04/06`。其中 `0x02380004` 分支会清 gate 后进入 UI 分发；这与诊断注入时校准十字移动的动态路径一致。
- `0x020080f0..0x02008220` / `0x02009baa..` 会把 `0x022f8569/6a/54` 复制到 `0x02380003/05/07`，并清 `0x02380004/06/08`。
- 只注入 `0x022f8569/6a/54` 与 `0x02380004/08` 时，固件会消费 `0x02380004` 并进入 `0x020680b8`，但 LCD 仍停在左上校准点：
  - 日志：`build\bbk9288s-test\run-qmp-touch-uiinject-fpt4-20260722-232610.log`
  - 图像：`build\bbk9288s-test\lcd-dump-uiinject-zoom3.png`
- 注入完整 `0x02380002/04/06/08` gate 后，校准十字从左上移动到屏幕下半部，说明 UI/校准层可以接收这组 guest pen cache/gate：
  - 日志：`build\bbk9288s-test\run-qmp-touch-uiinject-fullgates-fpt4-20260722-232908.log`
  - 状态：`build\bbk9288s-test\run-qmp-touch-uiinject-fullgates-fpt4-20260722-232908.txt`
  - 图像：`build\bbk9288s-test\lcd-dump-uiinject-fullgates-zoom3.png`
- 完整 gate 注入会额外触发 6 次 touch-down 状态下的 AD0 采样，仍返回 `add=0x03c0`；这进一步确认 `0x02380002` 是电源/AD0检查入口，不是触摸坐标 ADC。
- 默认关闭 `debug-touch-ui-inject` 后重跑，行为保持不变：port4/vector68 和 8tm/vector53 去抖正常，`0x02380002/04/06/08`、`0x022f8554/69/6a` 没有自然非零写入。
  - 日志：`build\bbk9288s-test\run-qmp-touch-default-after-fullgates-fpt4-20260722-233028.log`
- 窄 trace 复核 `0x02007b40..0x020682d8`：
  - 默认路径日志：`build\bbk9288s-test\run-qmp-touch-adcgate-default-fpt4-20260722-234655.log`
  - 诊断 gate 日志：`build\bbk9288s-test\run-qmp-touch-adcgate-uiinject-fpt4-20260722-234655.log`
  - 默认路径仍只有 port4/vector68 和 8tm/vector53；`0x02380002/04/06/08/09` 没有自然非零写入，ADC completion 仍只有启动/电源 AD0 样本。
  - 诊断 gate 下 `0x0200888a` 读到 `0x02380004 = 1`，随后 `0x020088ac -> 0x020680b8(r6=4)`；`0x02067a02` 未触发，`0x02380009` 仍为 0。
- 低 RAM gate 复核：
  - 完整 gate 诊断日志：`build\bbk9288s-test\run-qmp-uiinject-fullgates-lowram-fpt4-20260722-234025.log`
  - 只有 `0x020088a0 -> 0x00003fdc = 0x55` 这类状态写入；`0x00003fc9` 没有置位，说明 UI 推进不是靠 `3fc9` 主循环分支。
- 默认长按复核：
  - 日志：`build\bbk9288s-test\run-qmp-touch-longhold-debounce-fpt4-20260722-234157.log`
  - 长按会让 `0x022681fb` 置 1 后被 `0x0200886c` 清掉，但仍不写 `0x02380002/04/06/08` 或非零 pen cache；该路径是 debounce/key longpress 通知，不是坐标入口。
- `0x02008000..0x02008248` debounce writer 复核：
  - `0x02008134..0x0200816a` 和 `0x020081ca..0x020081fe` 都会复制 `0x022f8569/6a/54` 到 `0x02380003/05/07`。
  - 第一段明确把 `0x02380004/06/08` 清 0；第二段写的是 `r7`，但在当前路径 `0x0200819e` 已把 `r7=0`，所以它也是清 gate，不是自然生产者。
  - 因此当前默认 debounce 路径能确认“按下/松开/长按”，但不能生成触摸坐标 gate。
- `debug-touch-ui-inject=on` 下的 PEN/校准逻辑 trace：
  - 日志：`build\bbk9288s-test\run-qmp-uiinject-penlogic-fpt4-20260722-235036.log`
  - 与默认路径一样，只执行 `0x0203883a -> 0x02061f40` 同步函数；没有进入 `0x0206000c`、`0x0205f73a`、`0x020614cc/153a/1a28/1a38` 这些前台 setter/回调。
  - 快照显示诊断注入确实保留了 `0x022f8569=7`、`0x022f856a=4`，但 `0x02061f40` 仍按自己的同步输入把同组缓存当作 0 处理。这说明校准十字移动来自 gate/UI 状态机，不是前台 PEN setter 被自然调度。
- `debug-touch-ui-gate-mask=0x02` 复核：
  - 日志：`build\bbk9288s-test\run-qmp-uiinject-gatemask02-fpt4-20260722-235348.log`
  - 只打开 `0x02380004` gate 时，`0x0200888a` 读到 `0x02380004=1`，随后 `0x02008896` 清 gate 并由 `0x020088ac -> 0x020680b8(r6=4)` 分发。
  - 同一次 trace 中 `0x02380002` 和 `0x02380008` 读取仍为 0，`0x02380006` 没有进入该 dispatcher 分支；这进一步说明校准 UI 推进的最小诊断入口是 `02380004 + pen cache`，不是完整四个 gate 都必需。
- PEN 属性表复核：
  - `0x02000954..0x020009a8` 是一组属性 getter/setter 表，包含 `0x02061f40` 全量同步、`0x020622f6/2338` helper、`0x02062340/2386` x、`0x0206238e/23d4` y、`0x020622d4/22ee` status 等函数。
  - 固件中没有直接 call 或 32-bit 常量指向 `0x0206000c`；它更像高层事件分发入口，当前默认触摸没有把它调度起来。
  - 默认 `0x02061f40` 会逐项调用这些 setter/getter，但所有输入仍为 0，所以 `0x022f8569/6a/54` 保持 0。
- K5 sweep 复核：
  - 新日志：`build\bbk9288s-test\run-qmp-touch-k5mask01/02/04/08/10-fpt4-20260723-000218..000219.log`
  - `touch-k5-low-mask=0x01/02/04/08` 都触发 `key-input0` vector `16`，随后在 `0x0200470a` BRK；这些是错误按键路径，不是触摸坐标入口。
  - `touch-k5-low-mask=0x10` 不 BRK，但只是 `K5[4:0] old=0x1f -> new=0x0f` 的线变更，仍只走 port4/vector68 + vector53 debounce；`0x022f8569/6a/54` 和 `0x02380004/06/08` 没有非零写入。
- FPT6/FPT7 sweep：
  - 日志：`build\bbk9288s-test\run-qmp-touch-fpt6-fpt6-20260723-000437.log`、`build\bbk9288s-test\run-qmp-touch-fpt7-fpt7-20260723-000437.log`
  - `touch-fpt=6` 会触发 port6/vector `70`，随后进入 16tm3-b/vector `42` 周期路径；没有 BRK，但 `0x022f8569/6a/54`、`0x02380004/06/08` 仍没有非零写入。
  - `touch-fpt=7` 只看到 FPT7 factor 置位，未交付 vector，仍无 PEN/gate 非零写入。
  - 因此当前触摸缺口不像是简单选错 FPT4/FPT5/FPT6/FPT7 中断线。
- 事件入口复核：
  - 默认触摸聚焦 trace：`build\bbk9288s-test\run-qmp-event-gate-default-fpt4-20260723-001130.log`。
  - `debug-touch-ui-inject=on` 聚焦 trace：`build\bbk9288s-test\run-qmp-event-gate-uiinject-fpt4-20260723-001149.log`。
  - 两者都没有进入 `0x0205f73a`、`0x02061b6c` 或 `0x0206000c` 一类高层事件对象入口；默认路径也没有 `0x02038024` UI dispatch call。
  - 静态反汇编确认 `0x0205f73a` 处理 `0x0120` 分支时会检查 `r8 >> 16 == 7`，随后才调用 `0x02061b6c` 注册/创建高层触摸对象。
  - 本地 `app_env_9288s` 开发包中 `guiWindow.h` 定义 `MSG_COMMAND = 0x0120`，`guiIal.h` 定义 `IAL_MOUSEEVENT = 1`、`IAL_MOUSE_LEFTBUTTON = 4`，`guiExt.h` 定义 `TP_STATUS_PENDOWN = 1`、`PENMOVE = 2`、`PENUP = 3`。
  - 因此 `0x0205f73a` 的 `0x0120` 更像 GUI command/control 层入口，不是原始硬件触摸中断；默认模型不能直接伪造该消息来推进 UI。
- CTM 手册语义复核：
  - S1C33S01 技术手册中的 CTM 表显示 `TCISE[2:0] = 111` 为 no interrupt，`110/101/100/011/010/001/000` 分别对应 1 day、1 hour、1 minute、1 Hz、2 Hz、8 Hz、32 Hz。
  - 当前固件最终写入常见 `TCINT=0xec`，即 `TCISE=111`，所以默认不再生成周期 vector `65`。
  - `0x02004998` 确实是 CTM/vector65 handler，也会写 `0x02380002/04/06` gate，但在当前 `TCISE=111` 下不能作为真实触摸入口。
- CTM 小修后 smoke：
  - 日志：`build\bbk9288s-test\run-qmp-ctmfix-smoke-fpt4-20260723-002025.log`。
  - 结果仍是 port4/vector `68` + 8tm0-3/vector `53`；`BRK=[]`、`pen-calls=[]`。
  - ADC completion 仍只有 6 次 `ch=0..0`、`touch=up` 的启动/电源样本。
  - 日志中没有 `bbk9288s-ctm: factor set`、`vector=65`、`0x02004998`、`0x02038024`、`BRK` 或 unimplemented opcode。
- 分阶段快照复核：
  - `scripts/bbk9288s_qmp_touch_trace.py` 新增 `--timeline-snapshots` 和 `--snapshot-settle-ms`，可在 `pre/down/hold/up/post` 阶段抓 HMP 快照，不必打开重 trace。
  - 默认触摸 timeline：`build\bbk9288s-test\run-qmp-timeline-default-fpt4-20260723-002248.txt`。
  - `0x023800b0` 随触摸从 `0 -> 2 -> 0`，`0x023800b2` 在 release 后为 `1`，说明去抖确认按下/松开。
  - `0x022f8554/69/6a`、`0x02380004/06/08`、`0x0238f148` 从 `pre` 到 `post` 全程为 0，排除了“短暂非零被日志漏掉”的假设。
  - ADC 寄存器保持启动/电源 AD0 状态，没有切到 AD1/AD2。
- 16-bit timer0 复核：
  - timer0 快照：`build\bbk9288s-test\run-qmp-timeline-timer0-fpt4-20260723-002723.txt`。
  - IDMA 快照：`build\bbk9288s-test\run-qmp-timeline-timer0-idma-fpt4-20260723-002828.txt`。
  - 官方 S1C33L05 手册显示 `0x48180/82/84/86` 分别是 timer0 compare A、compare B、counter、control；`0x40272/82 bit3` 为 timer0 compare A enable/factor，`bit2` 为 timer0 compare B enable/factor。
  - 旧快照中固件把 timer0 compare A/B 设为 `0x007d/0x00fa`，`0x48186=0x07`，`EIR2 bit2` 为 1，`FIR2` 为 0；`0x48186=0x07` 暴露出旧模型错误读回了 write-only `PRESET` 位。
  - `0x40290/94` IDMA request/enable 对应位为 0，所以若真实 timer0-B factor 产生，会走 CPU IRQ；但 vector `24` 当前是 `0x020046f0` 默认 BRK handler。不能简单把 timer0-B 做成周期 IRQ。
  - `0x02008366` 位于 trap table offset `0x78`，即 vector `30`，不是 timer0-B/vector `24`；这是另一条 16-bit timer handler，后续需要单独映射。
- timer0 counter/compare 模型复核：
  - timeline 快照：`build\bbk9288s-test\run-qmp-timer16-counter-fpt4-20260723-003748.txt`。
  - 日志：`build\bbk9288s-test\run-qmp-timer16-counter-fpt4-20260723-003748.log`。
  - `0x48186` 读回 `0x05`，确认 `PRESET` 位不再读回。
  - `0x48184` counter 在 `pre/down/hold/up/post` 快照中持续变化，并在 compare B 后回绕。
  - compare A/B 日志显示命中 `0x007d/0x00fa`，但默认 `debug-timer16-factors=off`，所以 `FIR2` 全程保持 `0`。
  - QEMU 正常退出，未出现 `BRK`、unimplemented opcode，也未交付 vector `24/25`。

QMP touch 事件对象/调度 trace：

- 入口/setter trace：
  - `build\bbk9288s-test\run-qmp-pen-setter-entry-fpt4-20260723-004220.log`
  - 默认 touch 会进入 `0x0203883a -> 0x02061f40`，但 `0x02061f40` 从 `0x022f8554..0x022f856a` 读到的全是 0，再用 zero 参数调用 `0x020622f6/2340/238e/22d4` 这组 setter。
  - `0x02062340` / `0x0206238e` 在 `r6 == 0` 时直接返回，所以 x/y cache 不会写入。
- gate/low-RAM/timeline trace：
  - `build\bbk9288s-test\run-qmp-gate09-timeline-fpt4-20260723-004252.log`
  - `build\bbk9288s-test\run-qmp-lowram-event-window-fpt4-20260723-004347.log`
  - `0x02380009` 在 pre 阶段已经为 1，不是当前缺失触发条件。
  - `0x00003fc0..0x00003fff` 事件窗口在 touch down/hold/up/post 期间稳定；`0x3ffb/3fd8/3fdc` 仍为 0，`0x3fdd=0xc0`。
- vector53 debounce：
  - `build\bbk9288s-test\disas-vector53-debounce.txt`
  - 扫描值为 `(K5D & 0x0f) | (P0D & 0x30)`；当前默认 touch down 下 `K5D=0x1f`、`P0D=0xeb`，合成 `0x2f`，所以固件确实检测到按下。
  - vector53 会把已有 pen cache 复制到 `0x02380003/05/07` 并清 gate；它不是 pen cache 的生产者。
- AD0/power path：
  - `build\bbk9288s-test\disas-adc-power-02067500-02067a80.txt`
  - `0x02067770/0x02067834/0x020678c8` 是 AD0 电池/电源采样链路，不是默认 touch 坐标采样入口。
- scheduler/event object trace：
  - `build\bbk9288s-test\run-qmp-event-object-fpt4-fpt4-20260723-004845.log`
  - `build\bbk9288s-test\run-qmp-watchcall-event-fpt4-20260723-005515.log`
  - `build\bbk9288s-test\run-qmp-waitcallers-fpt4-fpt4-20260723-005923.log`
  - `0x02092c36` 在启动时把任务回调 `0x02088b2e` 注册到 `0x0231a430` 对象。
  - 按下时 vector53 调用 `0x02094b30 -> 0x0209252a(r6=1,r7=0x10)`，事件 1 对应对象 `0x02319f30`，只确认 `0x02319f38 |= 0x10`。
  - `--watch-call` 复验显示 `0x02088b2e` 只在启动阶段被恢复一次；`0x02092644` 只登记了 `r7=7,r8=0x80` 的等待；按下时 `0x0209252a` 两次 post `event1/0x10`，但没有观察到等待该事件的任务。
  - 静态等待点中存在 event1 等待：`0x02007a96(r7=1,r8=0x1111)`、`0x02007db0(r7=1,r8=0x0111)`、`0x0206801a(r7=1,r8=0x1011)`，都覆盖 bit `0x10`。
  - 动态 wait-caller trace 中这些 event1 等待点和 `0x02007a10/0x02007d90` 包装入口调用次数均为 0；实际只看到 `0x02088b2e` 启动恢复、`0x0209252a` post 两次、`0x02092644` 等 `event7/0x80` 一次。
  - 当前日志没有看到该事件自然推进到 pen cache writer；下一步应找出哪条前置路径会让前台任务等待 `event1/0x10`，或确认真实硬件按下不应该 post event1。

以下是串行触摸 ADC 确认前的历史未确认项，现仅保留为排除记录：

- 固件真正从哪个 GPIO/队列状态进入触摸坐标 ADC 采样。
- 默认硬件路径怎样自然产生完整 `0x02380002/04/06/08` gate 和 `0x022f8569/6a/54` 非零 pen cache。
- `0x02380009` 的真实来源；它可能只控制 `0x02067a02` 的附加显示/状态路径，当前校准 UI 推进不依赖它。
- `0x0238f148` 的真实置位来源，以及为什么当前 touch debounce 事件没有调度到 `0x020614cc/153a/1a28/1a38` setter 分支。
- `0x02004a..`、`0x02007b..`、`0x020088..`、`0x02067a..`、`0x020681..` 这些 gate writer 的业务角色和真实调度条件。
- `0x022f8569/6a/54` 的真实含义：可能是前台事件队列、触摸/按键 translate table 结果或 UI 状态缓存。
- `0x022109b8/c8/c9/cc` 这些静态表值如何决定 `0x022f8554/69/6a` 的有效范围和写回值。
- `0x02380008` 的业务角色和非零写入条件。
- 第一次触摸是否只负责唤醒，还是缺少某个 GPIO/按键/timer/storage 初始化结果。
- 真实 9288S 触摸屏的 GPIO 驱动顺序、AD 通道顺序、去抖计数、压力/状态判断。
- 低地址 RAM 例程 `0x000015d2..0x00001bxx` 的业务角色：目前看起来是 power/idle 保存恢复，但还需要确认它是不是正常前台循环的一部分。
- 16-bit timer0 compare factor 在真实硬件上是否还通过 IDMA、门控或固件清因子流程间接影响触摸前台事件循环；当前默认只观测 compare，不把 compare B factor 直接投递到 vector `24`。

当前原则：

- 不强行把 timer0-B 做成周期 IRQ，除非确认 vector `24` 不是默认 BRK handler，或者找到固件写入 timer0 factor 的真实证据。
- timer0 后续继续用已补的 counter/compare 可观察性确认真实 factor、IDMA/CPU 路由和 vector table handler；在证据不足前不打开默认 IRQ。
- 不强行合成 timer4/5 IRQ：当前 `EIR4=0`、`FIR4` 未使用，vector44/45/47 又指向默认/空 handler，乱触发风险高。
- 不无条件产生 CTM/vector65；`TCISE=111` 必须保持 no interrupt。
- `0x02380002/04/08`、`0x022f8569/6a/54` 继续避免手工硬塞，必须找到真实 writer 和调度条件。
- 不把 `0x02380009` 加进默认触摸模型；当前它只是在静态路径中控制 `0x02067a02`，没有证据表明它是校准 UI 必需 gate。
- `debug-touch-ui-inject` 曾用于证明 UI 层可推进，现已从发布源码删除；默认路径不依赖它。
- `debug-touch-ui-gate-mask` 曾用于定位 gate 对应的固件路径，现已从发布源码删除。
- 不硬塞 `0x0238f148`；它现在看起来是 `0x022f8569/6a` setter 的输入，必须找到真实调度路径。
- 继续优先提升可观察性，然后按日志补模型。

## 下一轮执行计划

### P1：真实触摸坐标路径

状态：已完成。真实路径是 P06/FPT6 唤醒加板级串行触摸 ADC `0x00300020`，不是 FPT4 debounce 事件，也不是片内 AD1/AD2。默认模型无需 `debug-touch-ui-inject` 即可通过三点校准并进入主界面。

后续维护：

- 保留旧 FPT4 和 UI 注入实验记录作为历史诊断证据；注入入口不进入发布源码。
- 用 `scripts/bbk9288s_qmp_touch_trace.py --point 16,24 --point 144,216 --point 80,120` 回归三点校准。
- 后续进入具体应用时，继续验证校准后的点击命中、拖动和 pen move 行为。
- 如发现坐标边缘误差，再根据真实机器采样调整 host 坐标到 12-bit 原始值的线性映射，不改动已经确认的串行协议和中断线。

完成标准：

- 已完成：LCD 输出清晰，四灰阶 2bpp bit order 已校正。
- 已完成：排除 AD0=touch 的错误假设，AD0 固定为电池/电源 OK 样本。
- 已完成：CTM 遵守 `TCISE=111` no interrupt，不再制造虚假 vector `65` 路径。
- 已完成：timer4/5 register metadata 补齐；长按事件后的 timer4/5 路径确认不是当前缺失 IRQ。
- 已完成：诊断注入完整 pen cache/gate 可让校准十字移动，证明 LCD/UI 端不是当前 blocker。
- 已完成：找到并验证 `0x00300020` 板级串行触摸 ADC、P1.3 MOSI、FPT6 和 Timer3-B 采样路径。
- 已完成：三个校准点均响应点击，校准参数非零并进入主界面。

### P2：校准真实按键矩阵

状态：已完成当前游戏所需的六键映射。

任务：

- [x] 通过应用回调消息 `0x10` 记录每条实体线产生的 guest 键码。
- [x] 方向键映射到 `K5.3/K5.2/K5.1/K5.0`。
- [x] Enter/Space 映射到确定线 `P0.5`，Esc/Backspace 映射到退出线 `P0.4`。
- [x] 保留 key repeat 和共享硬件线的计数逻辑；不支持的主机按键不再误触发 `K5.0`。

完成标准：

- [x] 海盗船方向、确认和退出均被固件稳定识别。
- [x] key down/up 通过真实 GPIO wake、扫描和 guest 消息路径处理。

### P3：完善 LCD 输出

状态：当前目标已完成。四灰阶画面、校准、主菜单和海盗船均清晰，GTK/SDL 交互窗口已启用。

任务：

- 继续校准 palette/反相和未来交互窗口输出。
- 找出完整 LCDC 控制寄存器和 framebuffer base/offset。
- [x] 重配 QEMU 启用 GTK/SDL，并确认 GTK 创建独立模拟器窗口。

完成标准：

- PGM 或窗口画面与真实 UI 方向、灰阶和文字可读性一致。
- 触摸校准流程推进时画面能随固件状态更新。

### P4：继续补 CPU/MMIO 缺口

状态：按真实路径增量补；三点校准暴露的 `adc`/`sbc` 已实现并通过固件路径验证。

任务：

- 新 opcode blocker 出现时按 C33 manual 补 translator/disassembler。
- 对 timer、GPIO、ADC、LCDC、NAND 继续补 per-register hook。
- 保留未知寄存器首次访问日志，避免过早猜完。

完成标准：

- 新硬件等待点能通过日志直接定位到 PC、寄存器、MMIO 和中断状态。

### P5：实现存储

状态：完成当前阶段。真实固件已识别、格式化并重启挂载持久化 NAND；完整 `系统` 树已通过 FAT16/VFAT+FTL 镜像暴露给 guest。

任务：

- [x] 记录 NAND/SmartMedia 命令流并确认总线 GPIO。
- [x] 返回稳定 ID/status，让固件按 `EC/DA` 256 MiB 几何初始化。
- [x] 实现 page/OOB read、program、erase 和 HSDMA 数据通路。
- [x] 反查 MBR、FAT16、VFAT 长名、FTL block map 和 OOB parity。
- [x] 提供系统目录安装/镜像回包工具并验证 `HZK_LIB.BIN` 哈希。
- [x] 生成固件兼容的 GBK FAT 短名，并验证词典越过字库缺失检查。
- [ ] 后续按需要实现真实 NAND ECC 值；当前固件路径在全 `0xFF` ECC 寄存器模型下可稳定读取镜像。

完成标准：

- [x] 固件能通过存储初始化并在重启后保持 1076 个映射。
- [x] 能读取完整系统资源并渲染主菜单位图。
- [x] RTC 有效状态和 host 时间初始化正确，启动不再要求重新设置时间。

### P6：整理调试体验

状态：当前目标已完成，后续随新外设继续维护。

任务：

- 保持 `-d guest_errors,int,in_asm` 可用。
- 增加更窄的 trace 开关，减少完整 `trace-io` / `trace-mem` 噪声。
- 整理常用 QMP touch/key 测试脚本。
- 使用 `--stage-screendumps` 在同一次运行抓取稳定 UI 阶段；`--wait-after-ms` 和 `--key-after` 支持不同等待时间及键盘事件穿插。
- `--key`、`--key-delay-ms`、`--key-gap-ms` 和 `--key-hold-ms` 已用于完整游戏控制回归；QEMU stderr 会单独保留，提前退出不再丢失原因。
- `run-bbk9288s.cmd`/`.ps1` 提供可交互启动入口和持久化用户 NAND。
- 必要时加入小型 test KNL 镜像验证 IRQ/NMI/RETI/PSR/stack。

完成标准：

- 新 blocker 出现时，不需要靠大日志手工猜状态。

## 常用验证命令

构建：

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
ninja -C E:\eebbk9288s-qemu\build qemu-system-s1c33.exe
```

代码检查：

```powershell
git -C E:\eebbk9288s-qemu diff --check -- hw/s1c33/bbk9288s.c target/s1c33/cpu.c target/s1c33/cpu.h target/s1c33/helper.c
```

默认 smoke：

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /e/eebbk9288s-qemu; rm -f build/bbk9288s-test/run-default-timer16-metadata.log; timeout 4s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-default-timer16-metadata.log; printf "exit:%s\n" "$?"'
```

Key scan 窄日志：

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /e/eebbk9288s-qemu; rm -f build/bbk9288s-test/run-keyscan-debugmask.log; timeout 3s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,trace-key-scan=on,debug-key-p0-low-mask=0x08 -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-keyscan-debugmask.log; printf "exit:%s\n" "$?"'
```

LCD headless PGM dump：

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /e/eebbk9288s-qemu; rm -f build/bbk9288s-test/lcd-dump.pgm build/bbk9288s-test/run-lcd-dump.log; timeout 3s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,debug-lcd-dump-ms=1000,debug-lcd-dump-path=build/bbk9288s-test/lcd-dump.pgm -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-lcd-dump.log; printf "exit:%s\n" "$?"'
```

## 当前保留日志

- `build\bbk9288s-test\run-lcd-dump-current.log`
- `build\bbk9288s-test\lcd-dump-current-zoom3.png`
- `build\bbk9288s-test\run-lcd-dump-ctmselect-calib-5s.log`
- `build\bbk9288s-test\lcd-dump-ctmselect-calib-5s-zoom3.png`
- `build\bbk9288s-test\run-lcd-dump-ctmselect-click1-5s.log`
- `build\bbk9288s-test\lcd-dump-ctmselect-click1-5s-zoom3.png`
- `build\bbk9288s-test\run-qmp-touch-ctm-click1-traceio.log`
- `build\bbk9288s-test\run-default-ctm-smoke.log`
- `build\bbk9288s-test\run-qmp-touch-ctm-vector65.log`
- `build\bbk9288s-test\run-disas-vector65.log`
- `build\bbk9288s-test\run-qmp-touch-p0low-v53.log`
- `build\bbk9288s-test\run-qmp-touch-sleepresume-fixed.log`
- `build\bbk9288s-test\run-qmp-touch-haltpc-k5d.log`
- `build\bbk9288s-test\run-qmp-touch-iram-inasm-long.log`
- `build\bbk9288s-test\run-qmp-touch-wake-release-second.log`
- `build\bbk9288s-test\run-qmp-touch-release-inasm-traceio.log`
- `build\bbk9288s-test\run-qmp-touch-release-tracemem.log`
- `build\bbk9288s-test\run-qmp-touch-keyinput.log`
- `build\bbk9288s-test\run-default-timer16-metadata.log`
- `build\bbk9288s-test\run-qmp-touch-keyinput-timer16meta.log`
- `build\bbk9288s-test\run-qmp-touch-tracemem-filter.log`
- `build\bbk9288s-test\run-qmp-touch-tracemem-table-022109b0.log`
- `build\bbk9288s-test\run-qmp-touch-tracemem-pc-02061f40.log`
- `build\bbk9288s-test\run-qmp-touch-tracemem-0238f140.log`
- `build\bbk9288s-test\run-qmp-touch-tracemem-pc-02060000.log`
- `build\bbk9288s-test\run-qmp-touch2-tracemem-pc-02060000.log`
- `build\bbk9288s-test\run-qmp-touch2-traceexec-02060000.log`
- `build\bbk9288s-test\run-qmp-touch-traceexec-02008000.log`
- `build\bbk9288s-test\run-qmp-touch-current-state.log`
- `build\bbk9288s-test\run-qmp-touch-current-debounce.log`
- `build\bbk9288s-test\run-qmp-touch-current-inasm.log`
- `build\bbk9288s-test\run-qmp-touch-current-longhold-lite.log`
- `build\bbk9288s-test\run-qmp-touch-longhold-nolog.txt`
- `build\bbk9288s-test\lcd-dump-touch-longhold-nolog-zoom3.png`
- `build\bbk9288s-test\run-qmp-touch-longhold-xp.txt`
- `build\bbk9288s-test\run-qmp-touch-longhold-regs.txt`
- `build\bbk9288s-test\run-qmp-touch-longhold-release5s-regs.txt`
- `build\bbk9288s-test\run-qmp-touch-longhold-release5s-ioregs.txt`
- `build\bbk9288s-test\run-qmp-touch-longhold-intonly.log`
- `build\bbk9288s-test\run-qmp-touch-iram-tracemem-longhold.log`
- `build\bbk9288s-test\run-qmp-touch-k5mask01-state.log`
- `build\bbk9288s-test\run-qmp-touch-p0low-off.log`
- `build\bbk9288s-test\lcd-dump-touch-p0low-off-zoom3.png`
- `build\bbk9288s-test\run-qmp-touch-longhold-681fb-20260722-214442.log`
- `build\bbk9288s-test\run-qmp-touch-longhold-681fb-20260722-214442.txt`
- `build\bbk9288s-test\run-qmp-touch-timer45-meta-20260722-221619.log`
- `build\bbk9288s-test\run-qmp-touch-timer45-meta-20260722-221619.txt`
- `build\bbk9288s-test\run-qmp-touch-traceexec-penbridge-fpt4-20260722-231816.log`
- `build\bbk9288s-test\run-qmp-snapshot-pen-limits-fpt4-20260722-231954.txt`
- `build\bbk9288s-test\run-qmp-touch-default-after-uiinject-fpt4-20260722-232610.log`
- `build\bbk9288s-test\run-qmp-touch-uiinject-fpt4-20260722-232610.log`
- `build\bbk9288s-test\lcd-dump-uiinject-zoom3.png`
- `build\bbk9288s-test\run-qmp-touch-uiinject-fullgates-fpt4-20260722-232908.log`
- `build\bbk9288s-test\run-qmp-touch-uiinject-fullgates-fpt4-20260722-232908.txt`
- `build\bbk9288s-test\lcd-dump-uiinject-fullgates-zoom3.png`
- `build\bbk9288s-test\run-qmp-touch-default-after-fullgates-fpt4-20260722-233028.log`

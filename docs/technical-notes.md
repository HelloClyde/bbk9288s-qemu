# BBK 9288S QEMU Bring-up Notes

This worktree contains an early QEMU machine and CPU target for the BBK
9288S firmware image.

## Current Scope

Implemented pieces:

- `s1c33-softmmu` target skeleton.
- `C33L05` CPU model with registers, PC, PSR, GDB register XML, and TCG
  translation scaffolding.
- `bbk9288s` machine.
- KNL firmware loader for BBK-style `KNL ` images.
- SDRAM window mapped at `0x02000000`, with an 8 MiB default RAM size. The
  current firmware heap reaches `0x0240xxxx`, so a 4 MiB SDRAM window drops
  runtime object-table stores just past `0x02400000`.
- Initial KNL vector scan that sets the reset PC to the first plausible
  firmware address.
- Internal 16 KiB RAM mapped at `0x00000000`, with direct-load SP initialized
  to the firmware's `0x3f80` boot stack. The top four bytes retain the
  firmware RTC-valid cookie `01 02 03 04` instead of being overwritten by the
  first `pushn`.
- Board I/O stub mapped at `0x01000000`, including the early self-test
  status/command latch behavior currently observed on `0x01000082..85`.
- S1C33L05 on-chip I/O stub window mapped at `0x00040000` with 64 KiB of
  byte-addressable backing registers.
- S1C33L05 LCDC register stub mapped at `0x00380000`, with `HSIZE/VSIZE`
  seeded for the 9288S `160x240` panel during direct KNL loading.
- S1C33L05 internal VRAM mapped at `0x003c0000..0x003c9fff` as real 40 KiB
  RAM.
- Minimal QEMU graphical console rendering internal VRAM as `160x240`, 2 bpp,
  four-gray framebuffer.
- Headless LCD framebuffer dump support. `debug-lcd-dump-ms` writes the current
  internal VRAM view as a `160x240` binary PGM file, with the path selected by
  `debug-lcd-dump-path`.
- MMIO diagnostics for the I/O window: first read/write per register is logged,
  and the last 64 MMIO accesses are dumped on QEMU exit.
- Named ITC/GPIO register metadata for the 9288S early `0x000402xx` path.
  Known registers apply documented masks, reserved bits read back as zero, and
  interrupt factor flags use reset-only write-one-to-clear behavior.
- ITC pending calculation for known enable/flag register pairs. In the old
  early-HALT trace, the firmware cleared factors and enabled USB interrupt
  routing, but did not leave an interrupt pending before HALT.
- Minimal HSDMA channel model for the current firmware path. Channel 1 handles
  the documented `0x48230..0x4823f` register block, software trigger
  `0x4029a bit 1`, dual-address successive transfers, hardware clear of
  `HS1_EN`, and DMA completion factor update.
- Minimal 16-bit timer 3 comparison-B wake source. `FIR3/EIR3 bit 0x40`
  delivers vector `42`, with priority read from `0x40267` and a bring-up
  fallback to level `1` when the firmware leaves that priority field at zero.
- 16-bit timer 2 comparison-B system tick. The firmware programs compare B to
  `292`, selects the 48 MHz OSC3/PLL source with `PSCDT0=0`, and selects
  `/4096` with `P16TS2=7`. The model raises `FIR3 bit 2`, delivers vector `38`,
  resets the counter, and repeats at about 24.9 ms. This drives the firmware
  GUI tick and application `SetTimer` messages.
- Minimal port input 4/5 wake sources. `FIR7/EIR7 bit 0x04` delivers vector
  `68`, with priority read from `0x4026C D2:0`; bit `0x08` delivers vector
  `69`, with priority read from `0x4026C D6:4`.
- Port input interrupt selection for FPT4-FPT7. `SPT4_7`, `SPPT0_7`, and
  `SEPT0_7` now control the selected pin, polarity, and edge/level condition.
  The current firmware writes `SPT4_7=0x0a`, `SPPT0_7=0x00`, and
  `SEPT0_7=0x30`, so FPT4/FPT5 are falling-edge inputs on P04/P05. Port input
  5/6/7 vectors are stubbed as `69/70/71`, with priorities from `PPORT4_5`
  and `PPORT6_7`.
- Minimal A/D converter model for the observed S1C33L05 register block.
  `0x40240..0x4025c` now covers the data registers, trigger, channel select,
  enable/control, per-channel result buffers, upper/lower limits, and
  interrupt mask. A software trigger immediately generates a 10-bit sample,
  fills `ADD` and `ADxBUF`, sets `ADF`/`ADFx`, and sets `FADE` in
  `FIR7 bit 0`. The implemented ADC interrupt vector is `64`, with priority
   read from `0x4026A D6:4`.
- Board serial touch ADC mapped at `0x00300000`, with the firmware data register
  at `0x00300020`. Bit 7 is the serial clock, bit 4 is MISO, and MOSI is driven
  through on-chip `P1D bit 3`. Commands `0x93/0x90` return Y and `0xd3` returns
  X as a 12-bit serial sample.
- Board audio decoder bus mapped at `0x003a0000`. The firmware reads data at
  `+0x00`, writes decoder commands or compressed bytes at `+0x02`, controls
  the interface at `+0x04/+0x08`, and polls `+0x0a` bits `0x04/0x10` for
  read/write readiness. Active-low board selects in `0x00300020` distinguish
  control transactions from the compressed stream. The control path implements
  VS1003 SCI read (`0x03`) and write (`0x02`) transactions, including MODE
  software reset, CLOCKF, DECODE_TIME, AUDATA, and VOL state. Timer4-B raises
  FIR4 bit 2 on vector 46 so the firmware runs its periodic SCI query path.
  The optional `audio-stream` machine property writes only bytes received while
  the stream select is active. An 8 KiB effective decoder input queue drives
  the real DREQ signal on P0.3 in 32-byte grants; parallel bus write-ready at
  `+0x0a bit 0x10` remains a separate signal. The queue drains at the detected
  MPEG bitrate on a real-time clock, and DECODE_TIME reports consumed full
  seconds. Guest writes to DECODE_TIME also reposition the decoder clock, so
  stop and replay begin again at zero. The capture rotates at 32 MiB; the Web
  server forwards that hardware output to the browser and never reads audio
  files from NAND.
- Board-level Samsung-compatible NAND at `0x04000000`, identified as
  `EC DA 10 95`: 256 MiB data, 2 KiB pages, 64-byte OOB, 64 pages per block,
  and 2048 blocks. Read, program, erase, status, ID, CPU access, and HSDMA access
  are modeled. The optional `nand-image` machine property loads and saves a
  persistent 276,824,064-byte raw image including OOB.
- `scripts/bbk9288s_nand_image.py` extracts the firmware FTL into a flat FAT16
  disk, installs the downloaded system tree with VFAT long names, and packs it
  back into physical NAND. The OOB map tag is
  `(logical_block << 1) | parity(logical_block)`.
- The NAND installer rewrites Chinese names that fit FAT 8.3 as raw GBK short
  names and updates the associated VFAT checksums. This matches the firmware's
  byte-path lookup for `A:\系统\数据\HZK_LIB.BIN` while preserving long names.
- The clock timer is seeded from QEMU's RTC at reset. The firmware interprets
  its 16-bit day field as days since 1901-01-01 and accepts years 1901..2049.
- Minimal GPIO data/control model for P0/P1/P2/P3: output bits read back the
  guest latch, while input bits default high to model no key press or external
  low level.
- QEMU keyboard input mapped to the six firmware-observed hardware lines:
  Up/Down/Left/Right drive `K5.3/K5.2/K5.1/K5.0`; Enter/Space drives the
  confirm line `P0.5`; Esc/Backspace drives the exit line `P0.4`. Key-down and
  key-up pass through the normal GPIO wake, debounce, scan, and guest message
  paths.
- Keyboard state is tracked per host QKeyCode and counted per mapped P0 bit,
  so key repeat does not generate extra wake edges, and multiple host keys
  sharing one temporary P0 bit do not release the line until all are up.
- `trace-key-scan` logs compact P0/P1 keyboard scan state. For deterministic
  scan calibration, `debug-key-p0-low-mask` can force selected P0 key-matrix
  input bits low from reset while still passing through the current P1.3
  strobe gate.
- Minimal QEMU pointer/touch hook. Absolute pointer coordinates are mapped to
  the portrait `160x240` panel and converted to the board touch ADC's 12-bit
  raw samples. AD0 remains the separate battery/power monitor. Left/touch
  button down drives P06/FPT6 by default, delivering port6/vector `70`; the
  firmware then samples X/Y from `0x00300020` on Timer3-B/vector `42`.
- TTBR byte-register mirror at `0x48134..0x48137`; direct KNL loading sets the
  trap table base to `0x02000000`.
- S1C33 maskable interrupt entry/return: IRQ acceptance honors `PSR.IE` and
  `PSR.IL`, pushes return PC and PSR, dispatches through `TTBR + vector * 4`,
  and returns with `RETI`.
- LCDC/USB/SPI ITC delivery for USB factor flag: vector `113`, with USB
  priority level read from `0x402A3`.
- `debug-usb-wakeup-ms` machine property, disabled by default, can set the USB
  interrupt factor after a host-time delay for wakeup testing.
- `debug-port4-wakeup-ms` machine property, disabled by default, can set the
  port input 4 interrupt factor after a host-time delay for wakeup testing.
- `debug-port5-wakeup-ms` machine property, disabled by default, can set the
  port input 5 interrupt factor after a host-time delay for wakeup testing.
- `debug-nmi-wakeup-ms` machine property, disabled by default, can raise NMI
  after a host-time delay for wakeup testing.
- `trace-io` machine property, disabled by default, logs every board,
  S1C33 I/O, and LCDC access instead of only the first access per register.
- `debug-lcd-dump-ms` and `debug-lcd-dump-path` machine properties, disabled by
  default, dump the current `160x240` LCD framebuffer to a binary PGM file.
- `trace-key-scan` machine property, disabled by default, logs compact P0/P1
  scan state without enabling full MMIO trace.
- `debug-key-p0-low-mask` machine property, disabled by default, forces
  selected P0 key-matrix input bits low from reset for scan tracing.
- `touch-fpt` machine property, default `6`, routes QEMU touch pen-down to one
  of FPT4-FPT7 for touch GPIO calibration.
- Minimal timer0 counter/compare observability. The model advances the
  16-bit timer0 counter, masks the write-only `PRESET` bit on reads, logs
  compare A/B hits, and leaves timer0 factors disabled by default.
- `debug-timer16-factors` machine property, disabled by default, allows timer0
  compare hits to set `FIR2` for controlled interrupt-routing experiments.
- `strict-board-io` machine property, disabled by default, makes unknown board
  I/O reads return zero and prevents unknown write loopback for diagnostics.
- `trace-psr` CPU property, disabled by default, logs `PSR.IE` and `PSR.IL`
  changes. In the old early-HALT trace, the real 9288S firmware recorded no
  IE/IL transition before HALT.
- `trace-mem` CPU property, disabled by default, logs guest load/store
  address, width, value, and PC. This is useful for VRAM and handler-table
  reverse engineering; normal translated code is unchanged when it is off.
- Basic instruction handling for the early boot path: EXT, stack push/pop,
  special-register transfers, register and immediate ALU operations, load/store
  byte/half/word forms, SP-relative stack load/store, bit operations, relative
  and register branch/call/jump including common delayed forms, RET, RET.d,
  RETI, NOP, SLP, HALT, BRK, PSR bit set/clear forms, shift/rotate forms,
  scan0/scan1, `adc`/`sbc` with N/Z/V/C flags, and the NMI-path
  multiplier/divider instructions.
- C33 `swap` and `swaph`, required by the firmware MP3 frame-header parser.
- Diagnostic halt on the first unimplemented opcode, with CPU register state.
- HALT/SLP diagnostics include the instruction PC, next PC, CPU state, and
  register dump.
- CPU-side recent-TB ring records the last 16 translated-block entry PCs and
  dumps them at HALT/SLP/unimplemented stops.

The emulator is not a complete S1C33L05 peripheral implementation, but the
current hardware model loads the real firmware, completes three-point touch
calibration without guest-memory injection, mounts persistent NAND, renders the
four-gray display, runs the bundled Pirate Ship game with working direction,
confirm, and exit controls, and plays the International Phonetic Alphabet MP3
samples through the browser.

## Build

Use the MSYS2 UCRT64 shell from PowerShell:

```powershell
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'UCRT64'
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /e/eebbk9288s-qemu && rm -rf build && ./configure --target-list=s1c33-softmmu --disable-werror --disable-docs --disable-tools --enable-gtk --enable-sdl --disable-relocatable && cd build && ninja qemu-system-s1c33.exe'
```

`scripts/symlink-install-tree.py` has a local Windows fallback because this
machine cannot create symlinks without Developer Mode/admin support. The
fallback tries a hard link first, then copies files or directories.

## Run

The raw NAND image is the only required guest image. Place it at the default
runtime path:

```powershell
E:\eebbk9288s-runtime\nand-user.raw
```

At reset, the board loader reconstructs the logical image from the NAND OOB
FTL tags, parses its FAT16 partition, walks the directory tree, and loads
`kernel.bin` from inside the NAND. No separate kernel file is required for
normal use.

For the Web frontend, run:

```powershell
.\run-bbk9288s-web.cmd
```

The launcher builds `web\dist`, starts QEMU's VNC WebSocket endpoint on port
`6081`, and serves the noVNC frontend on all local interfaces at port `8000`.
Open `http://127.0.0.1:8000/` on this PC, or use one of the LAN URLs printed by
the launcher on a phone or another computer. Windows Firewall must allow the
Python HTTP server and QEMU for access from another device.

The LCD accepts mouse, pen, and touch input. The on-screen D-pad and
confirm/exit controls send the same QEMU key events as the local keyboard. The
frontend keeps short touches and keys pressed for 180 ms so the original
firmware's debounce and key-matrix scan paths can sample them reliably. Input
still travels as normal RFB pointer/key events through the QEMU GPIO and serial
touch ADC models; there is no guest-memory injection or system hook.

The Power button in the Web header calls the emulator lifecycle API. It starts
QEMU when the firmware has powered the process off and performs a full QEMU
restart when it is already running. The adjacent refresh button only reconnects
the RFB display session. The frontend polls emulator status so an automatic
power-off is shown as `已关机` instead of an indefinite reconnect state.

The speaker button starts the browser audio stream after a user gesture, as
required by browser autoplay policies. The model captures the compressed bytes
sent by the firmware to the board decoder. The HTTP layer resumes at exact byte
offsets and closes a response when the capture rotates; a MediaSource pipeline
reconnects, appends sequential MP3 data, and trims old browser buffers while
preserving continuous decode and volume control.

The `NAND 文件` tab provides directory browsing, capacity reporting, upload,
download, new directory, rename, and recursive delete. Select
`进入维护模式` before editing. The backend sends QMP `quit`, waits for QEMU to
save `nand-user.raw`, extracts the FAT16 view, and stages all changes there.
`应用并重启` patches firmware-compatible GBK short names, repacks the real
FTL/OOB layout, and starts QEMU again. `放弃` discards the staged image and
restarts without applying file changes. Do not edit `nand-user.raw` with an
external tool while the launcher is running.

The default WebSocket endpoint has no authentication or transport encryption,
and the NAND API is served by the same unauthenticated HTTP endpoint. Expose
ports `8000` and `6081` only on a trusted LAN. QMP stays bound to localhost.
Override the ports when needed:

```powershell
.\run-bbk9288s-web.ps1 -HttpPort 8080 -WebSocketPort 6082 -QmpPort 6083
```

For a local GTK/SDL window instead, run:

```powershell
.\run-bbk9288s.cmd
```

The launcher reuses `E:\eebbk9288s-runtime\nand-user.raw` for guest saves. It
opens the GTK display by default; pass `sdl` to the CMD launcher to select SDL.
The mouse drives the touch panel. Arrow keys drive the four directions,
Enter/Space confirms, and Esc/Backspace exits.

The display model renders internal VRAM at `0x003c0000` as the portrait
`160x240`, 2 bpp, four-gray framebuffer used by the firmware.

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
E:\eebbk9288s-qemu\build\qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw -display none -serial none -monitor none -d guest_errors,int -D E:\eebbk9288s-qemu\build\bbk9288s-test\display.log
```

To dump the current LCD framebuffer to PGM without a GUI backend:

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /e/eebbk9288s-qemu; rm -f build/bbk9288s-test/lcd-dump.pgm build/bbk9288s-test/run-lcd-dump.log; timeout 3s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,debug-lcd-dump-ms=1000,debug-lcd-dump-path=build/bbk9288s-test/lcd-dump.pgm -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-lcd-dump.log'
```

The firmware's exact calibration targets are `(16,24)`, `(144,216)`, and
`(80,120)`. This headless QMP regression sends those three clicks through the
default FPT6 and serial ADC hardware path:

```powershell
python scripts\bbk9288s_qmp_touch_trace.py --prefix calibrate3 --point 16,24 --point 144,216 --point 80,120 --pre-ms 1000 --hold-ms 350 --between-ms 450 --post-ms 2500 --no-trace-key-scan --machine-opt debug-lcd-dump-ms=4200 --machine-opt debug-lcd-dump-path=build/bbk9288s-test/lcd-calibrated.pgm
```

The verified result has nonzero calibration parameters at `0x02292318`, raw
three-point data at `0x02292328`, no BRK or unimplemented opcode, and a readable
main menu in the LCD dump.

### Persistent NAND and system files

The firmware recognizes a Samsung `0xEC/0xDA` large-page NAND, reserves physical
blocks `0..39`, creates an MBR plus a FAT16 partition at LBA 32, and stores the
logical erase-block map in the per-sector OOB slots. A blank image can be
formatted by running QEMU long enough to finish the initial scan and format:

```powershell
python scripts\bbk9288s_qmp_touch_trace.py --prefix nand-format --no-touch --no-default-snapshots --pre-ms 0 --post-ms 55000 --no-trace-key-scan --machine-opt nand-image=build/bbk9288s-test/nand-formatted.raw --qemu-log-items guest_errors
```

With `pyfatfs` installed, populate a formatted image from the downloaded tree:

```powershell
python scripts\bbk9288s_nand_image.py install build\bbk9288s-test\nand-formatted.raw 'D:\Downloads\步步高9288s系统文件\系统' --output build\bbk9288s-test\nand-system.raw --flat build\bbk9288s-test\nand-system.fat.img
```

Run the machine with the resulting persistent image:

```powershell
build\qemu-system-s1c33.exe -M bbk9288s,nand-image=build/bbk9288s-test/nand-system.raw -cpu c33l05,exit-on-halt=off -rtc base=localtime -nographic -serial none -monitor none
```

The verified local image contains 285 files totaling 137,874,541 bytes and maps
1076 logical blocks. `系统\数据\HZK_LIB.BIN` is 3,081,592 bytes; its host and FAT
SHA-256 values both equal
`db8b0697ab93c8defab6f54e407c3df9778a42aacc56a8dc2b9d30a64aef5fa2`.
Reopening the packed image preserves all 1076 mappings without erase/program
repair, and the main menu loads its bitmap resources from NAND.

The former `请重新设置时间!` prompt was caused by initializing SP to `0x4000`:
the entry `pushn` overwrote the RTC cookie at `0x3ffc`. Initializing SP to
`0x3f80`, retaining the cookie, and seeding CTM from QEMU RTC removes the
prompt. The dictionary now finds `HZK_LIB.BIN` and reaches its normal USB-power
tip dialog. The calibrated touch path and final six-key hardware mapping now
dismiss application dialogs reliably and drive the bundled games.
Use `--stage-screendumps` on the QMP regression script to capture `pre`, each
stable post-click stage, and `post` as PPM files in one run.

The research CPU still exits QEMU when the firmware executes HALT/SLP unless
`exit-on-halt=off` is set. The current default firmware path gets past the old
HALT point and the HSDMA poll, then reaches a later HALT at `0x0000006a`.
With `exit-on-halt=off`, the Timer3-B model wakes that HALT. In the earlier
raw I/O model this reached the IRAM idle SLP point at `0x00001cb4`; with the
current GPIO input-high model the firmware runs a stable HALT/timer IRQ loop
instead, without unimplemented opcodes or BRK stops in the short verification
Run:

```powershell
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'UCRT64'
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /e/eebbk9288s-qemu && timeout 5s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-timerwake.log; echo exit:$?'
```

To set the port input 4 interrupt factor after startup for diagnostics:

```powershell
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'UCRT64'
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /e/eebbk9288s-qemu && timeout 6s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,debug-port4-wakeup-ms=1000 -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-port4wake-gpio.log; echo exit:$?'
```

To send a real QEMU key event through QMP:

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$port = 4561
E:\eebbk9288s-qemu\build\qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -qmp tcp:127.0.0.1:$port,server=on,wait=off -d guest_errors,int -D E:\eebbk9288s-qemu\build\bbk9288s-test\run-qmp-key-a-clean.log
```

Then send. For scan validation, hold key down briefly before sending key up;
putting down/up in one QMP command only proves the port4 IRQ path:

```json
{"execute":"qmp_capabilities"}
{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"up"}}}]}}
{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"up"}}}]}}
{"execute":"quit"}
```

For deterministic key-matrix scan tracing without full `trace-io` noise, force
one P0 column low from reset. This example models the current Up mapping
(`P0.3`, mask `0x08`) and logs only compact P0/P1 scan state:

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /e/eebbk9288s-qemu; timeout 3s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,trace-key-scan=on,debug-key-p0-low-mask=0x08 -cpu c33l05,exit-on-halt=off,trace-calls=on -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/run-keyscan-debugmask.log'
```

To set the USB interrupt factor after HALT for diagnostics:

```powershell
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'UCRT64'
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /e/eebbk9288s-qemu && timeout 2s build/qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw,debug-usb-wakeup-ms=50 -cpu c33l05,exit-on-halt=off -nographic -serial none -monitor none -d guest_errors,int -D build/bbk9288s-test/wakeup-real.log'
```

To trace interrupt-enable state changes:

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
E:\eebbk9288s-qemu\build\qemu-system-s1c33.exe -M bbk9288s,nand-image=E:/eebbk9288s-runtime/nand-user.raw -cpu c33l05,trace-psr=on -nographic -serial none -monitor none -d guest_errors,int -D E:\eebbk9288s-qemu\build\bbk9288s-test\psr-trace.log
```

Verified result with the 9288S `kernel.bin`:

```text
BBK9288S KNL 0100200608021118: body=0x268084 load=0x02000000 pc=0x0200468a
bbk9288s-board: first write pc=0x02083ae2 addr=0x01000094 size=1 value=0x00
...
bbk9288s-hsdma: ch1 dual mode=1 units=9596 unit=1 src=0x003c0000 dst=0x003c0004 sctl=3 dctl=3
bbk9288s-mmio: trace read pc=0x02005378 addr=0x0004823c size=1 value=0x00 reg=HSDMA_ENABLE group=hsdma
bbk9288s-timer: 16tm3-b wake scheduled after 10 ms
s1c33: HALT at pc=0x0000006a next_pc=0x0000006c
```

In a default run the guest no longer stops at `0x02084d32`, at the
`0x02098b3c` `scan1 %r13, %r12` instruction, or at the `0x02005378` HSDMA
poll. It reaches the later HALT stub at `0x0000006a`.

With `exit-on-halt=off`, the modeled Timer3-B wake source drives the firmware
past that HALT. In the earlier raw I/O model it reached the IRAM idle SLP
point:

```text
bbk9288s-timer: 16tm3-b factor set eir=0x44 fir=0x40 pir=0x03 level=1
bbk9288s-itc: deliver source=16tm3-b vector=42 level=1
s1c33: IRQ vector=42 level=1 table=0x02000000 handler=0x0200947a old_pc=0x0000006c old_psr=0x00000010
s1c33-call: reti     from=0x02094fea to=0x0000006c aux=0x00000010
s1c33: SLP at pc=0x00001cb4 next_pc=0x00001cb6
```

The current natural stop after real Timer3-B wake is the IRAM idle SLP at
`0x00001cb4`, with `TTBR=0x00002400` and `PSR.IE=1`, before the later GPIO
input model changed the idle branch.

With `debug-port4-wakeup-ms=1000`, the current GPIO input model sets the port
input 4 factor, dispatches vector `68`, and reaches a real firmware handler
that clears the factor:

```text
bbk9288s-itc: port4 factor set by debug-port4-wakeup-ms eir=0x1c fir=0x04 pir=0x66 level=6
bbk9288s-itc: deliver source=port4 vector=68 level=6
s1c33: IRQ vector=68 level=6 table=0x02000000 handler=0x02007fa4 old_pc=0x0000006c old_psr=0x00000010
bbk9288s-itc: port4-7-ctm-adc clear
```

Before the GPIO data/control model, this diagnostic wake later executed
stack/IRAM data and stopped at a bogus `0x02d5` opcode. With P0/P1/P2/P3 input
bits defaulting high, the same 6-second run no longer reports unimplemented
opcodes, BRK, or unknown pending interrupt sources.

With `debug-port5-wakeup-ms=1000`, the FPT5/P05 diagnostic path dispatches the
neighboring port input 5 vector and reaches the same shared firmware handler:

```text
bbk9288s-port: P05 diagnostic wake line low by debug-port5-wakeup-ms p0_int_low=0x20
bbk9288s-port: FPT5 factor set by debug-port5-wakeup-ms spt=2 sept=1 sppt=0 input=low
bbk9288s-itc: deliver source=port5 vector=69 level=6
s1c33: IRQ vector=69 level=6 table=0x02000000 handler=0x02007fa4
```

With QMP `input-send-event`, a real QEMU key down now reaches the same port4
path through the modeled FPT4 falling-edge condition:

```text
bbk9288s-port: wake line low by keyboard k6_low=0x10 p0_int_low=0x10
bbk9288s-port: FPT4 factor set by keyboard spt=2 sept=1 sppt=0 input=low
bbk9288s-itc: deliver source=port4 vector=68 level=6
s1c33: IRQ vector=68 level=6 table=0x02000000 handler=0x02007fa4 old_pc=0x0000006c old_psr=0x00000010
bbk9288s-itc: port4-7-ctm-adc clear
s1c33-call: reti     from=0x02007fda to=0x0000006c aux=0x00000010
```

The key-up event releases the mapped P0 line:

```text
bbk9288s-input: key up qcode=36 mapped=P0.0-high mask=0x00
```

Holding `qcode=up` with `trace-io=on` verifies that the firmware-visible GPIO
state changes during the scan loop. Before the key-down, the idle scan reads
`P0D=0xfb` at `pc=0x02009c9e`; while Up is held, QEMU maps it to P0.3 low and
the same firmware read returns `0xf3`; after key-up it returns to `0xfb`.
P1D reads/writes around `0x02009510..0x02009556` toggle `0x7f/0x77`, so P1 is
acting as a scan/strobe side of the matrix. Current QEMU therefore gates the
temporary P0 key lows behind P1.3 low:

```text
bbk9288s-input: key down qcode=100 mapped=P0.3-low mask=0x08 wake=port4
bbk9288s-mmio: trace read pc=0x02009c9e addr=0x000402d1 size=1 value=0xf3 reg=P0D group=gpio-port
bbk9288s-input: key up qcode=100 mapped=P0.3-high mask=0x00
bbk9288s-mmio: trace read pc=0x02009c9e addr=0x000402d1 size=1 value=0xfb reg=P0D group=gpio-port
```

The newer `trace-key-scan` path captures the same P0.3-low condition in a much
smaller log. With `debug-key-p0-low-mask=0x08`, early scan reads show:

```text
bbk9288s-keyscan: read pc=0x0203867e reg=P0D value=0xf3 p1-latch=0x00 p1-ioc=0x00 strobe=on p0-latch=0x00 p0-ioc=0x04 host-low=0x00 debug-low=0x08 raw-low=0x08 gated-low=0x08 touch=up
```

A separate QMP repeat/shared-bit test sends `ret down`, repeated `ret down`,
`spc down`, `ret up`, and `spc up`. `ret` and `spc` both map to P0.0 in the
temporary table. The log shows only the first down creates an FPT4 wake edge;
repeat and same-bit second key-down are suppressed, and P0.0 remains low until
the last key sharing that bit is released. In the current log,
`FPT4 factor set by keyboard` appears once.

The minimal A/D converter model is now exercised by the real firmware. The
default short run configures `ADCH=0x00` and `ADTRG=0x00`, writes
`ADCTL=0x16`, and then reads `0x40240/41`. QEMU completes the software trigger
synchronously. AD0 is treated as a battery/power monitor and returns a normal
level such as `0x03c0`; touch candidate channels return open-circuit high
values when the pen is up. This lets the firmware leave the earlier
`0x020677xx` sampling loop without showing the low-battery popup:

```text
bbk9288s-adc: complete software-trigger ch=0..0 last=0 add=0x3c0 flags=0x01 touch=up x=512 y=512
bbk9288s-adc: factor set by software-trigger ch=0 eir=0x0c fir=0x01 pir=0x00 level=1
bbk9288s-mmio: first read pc=0x020677e0 addr=0x00040240 size=2 value=0x03c0 reg=ADD_LO group=adc-touch
```

A QMP touch smoke test sends absolute X/Y coordinates plus left down/up. The
default path now drives P06/FPT6, reaches port6/vector `70`, starts Timer3-B,
and clocks the external touch protocol through `0x00300020`. A typical sample
looks like:

```text
bbk9288s-touch-adc: command=0x93 sample=0x188 pen=1 pixel=15,23
bbk9288s-touch-adc: command=0xd3 sample=0x184 pen=1 pixel=15,23
```

The exact three-point regression stores the first, center, and lower-right raw
samples at `0x02292328..0x02292333`, writes nonzero floating-point calibration
steps at `0x02292318`, and exits the calibration UI to the main menu. This path
uses no guest-memory injection. Timer3-B also observes `T16_CTRL3.PRUN`, so
clearing PRUN on pen release stops the sampling interrupt loop.

A fourth click at `(45,105)` opens the dictionary. The persistent NAND backend
exposes the complete downloaded `系统` tree, and the image tool emits GBK 8.3
aliases for the firmware's byte-oriented FAT lookup. Direct FAT verification
confirms `系统\数据\HZK_LIB.BIN` byte-for-byte, and the application now advances
to its normal `可以用USB线给机器供电!` tip instead of reporting a missing font.

Earlier FPT4, vector53, event1, and guest pen-cache traces remain useful as
negative evidence, but their conclusion that the coordinate producer was
missing is superseded by the board serial ADC path. The temporary diagnostic UI
injection properties used during that investigation were removed before the
public release.

With `debug-usb-wakeup-ms`, the old HALT trace reaches ITC pending for USB but
does not enter the handler because that HALT-time PSR has IE clear:

```text
bbk9288s-itc: USB factor set by debug-usb-wakeup-ms eir=0x40 fir=0x40 pir=0x06
bbk9288s-itc: deliver source=usb vector=113 level=6
```

With `trace-psr=on`, the old HALT path logged no IE/IL transition before HALT,
so that path did not enable maskable interrupts.

With `debug-nmi-wakeup-ms`, the real firmware enters NMI vector `7`, reaches
handler `0x020046f0`, runs through the common handler path, and still stops at
the firmware's own `BRK` instruction rather than a QEMU unimplemented opcode:

```text
bbk9288s-itc: NMI raised by debug timer
s1c33: NMI vector=7 level=0 table=0x02000000 handler=0x020046f0 old_pc=0x0200537a old_psr=0x00000000 sp=0x02280264
0x0200470a:  0400    brk
s1c33: BRK at pc=0x0200470a next_pc=0x0200470c
```

With `trace-io=on`, the handler's display helper reads LCDC `HSIZE` as
`0x0013`, matching a 160-pixel line, before returning to the vector thunk's
`BRK`:

```text
bbk9288s-lcdc: trace read pc=0x02066556 addr=0x00380042 size=2 value=0x0013 reg=HSIZE
...
s1c33: BRK at pc=0x0200470a next_pc=0x0200470c
```

The headless LCD dump path writes a valid `P5` PGM. With 8 MiB SDRAM, the
2-second dump reaches the touch calibration UI instead of a blank framebuffer:

```text
bbk9288s-lcd: dumped 160x240 2bpp framebuffer to build/bbk9288s-test/lcd-dump-8m.pgm gray-counts=551,0,0,37849
```

The visible prompt is a white 160x240 screen with a crosshair near the upper
left and Chinese text asking the user to tap the center of the crosshair.
Current recheck:

- Log: `build\bbk9288s-test\run-lcd-dump-recheck-now.log`
- Image: `build\bbk9288s-test\lcd-dump-recheck-now-zoom3.png`
- Result: the calibration page is readable, with
  `gray-counts=551,0,0,37849`. If a GUI window still looks scrambled, first
  confirm it is running the rebuilt `qemu-system-s1c33.exe`; the current
  headless framebuffer no longer shows the old LCD bit-order corruption.

## S1C33L05 Match Notes

Primary data source:

- Epson S1C33L05 Technical Manual:
  https://global.epson.com/products_and_drivers/semicon/pdf/id000276.pdf

Important parameters from the manual:

- CPU core: Seiko Epson C33 STD, 32-bit RISC.
- Instruction set: 105 basic instructions, fixed 16-bit instruction size.
- Registers: sixteen 32-bit general-purpose registers.
- Clock: CPU up to 48 MHz.
- Internal RAM: 16 KiB.
- Internal VRAM: 40 KiB, also usable as general RAM.
- LCD controller: 4/8-bit monochrome or color passive LCD interface.
- Display modes include 2 bpp / 4-level gray scale, 4 bpp / 16-level gray
  scale, and 8 bpp / 64-level gray scale.
- Internal-VRAM typical LCD resolutions include 320x240 at 4 bpp, 160x240 at
  8 bpp, and 160x160 at 12 bpp.
- External VRAM via SDRAM/UMA: 320x240 at 8 bpp or 16 bpp.
- SDRAM controller: up to 256 Mbit / 32 MiB with 16-bit data width.
- A/D converter: 10-bit, 5 channels.
- NAND flash interface: direct SmartMedia/NAND support, 8/16-bit NAND,
  NAND boot, and ECC.
- USB: USB 1.1 function controller.
- Package option: QFP21-176pin.

Fit against the 9288S:

- The 9288S four-gray-level screen should be treated as a 2 bpp monochrome
  grayscale panel. This is explicitly supported by the S1C33L05 LCDC.
- A portrait 160x240, 2 bpp framebuffer is only `160 * 240 * 2 / 8 = 9600`
  bytes, so it easily fits in the 40 KiB internal VRAM. The manual's
  160x240 at 8 bpp note is a higher-color internal-VRAM example, not the most
  likely 9288S mode.
- A resistive touchscreen would fit the 10-bit ADC plus GPIO model. The manual
  does not need a separate named touch controller for this to be plausible.
- The local firmware image is a KNL image whose first plausible execution
  address sits in the external SDRAM window used by this QEMU machine.
- External RAM, NAND flash, USB, LCDC, and low-power portable-device focus all
  line up with the known 9288S device class.

The next bring-up step is concrete device behavior. Current MMIO trace shows
early writes and read-modify-writes in the S1C33L05 interrupt-controller and
port-function registers:

- `0x40270..0x40279`: interrupt enable registers for key/port, DMA, timers,
  serial I/F, port/clock timer/A-D, and other factors.
- `0x40280..0x40289`: matching interrupt factor flag registers.
- `0x402A3`: USB/SPI interrupt priority register.
- `0x402A6`: LCDC/USB/SPI interrupt enable register.
- `0x402A9`: LCDC/USB/SPI interrupt factor flag register.
- `0x402C0`: key port selection.
- `0x402DF`: port function extension register, initialized to `0x03` on cold
  reset for CFEX0/CFEX1.

The next useful work is explaining the IRAM idle SLP path at
`0x00001c82..0x00001cb4`. The HSDMA poll at `0x02005378` is now modeled and
the Timer3-B source can wake the preceding HALT. Current traces show the
firmware entering SLP after scanning/clearing interrupt, port, timer, and LCDC
state with `TTBR=0x00002400`.

The next input-model work should replace the tentative P1.3-gated P0
input-column mapping with the real 9288S key matrix by tracing which P1
strobes correspond to physical keys. `trace-key-scan` plus
`debug-key-p0-low-mask` is now the preferred narrow trace path for this. Touch
already follows the confirmed P06/FPT6, Timer3-B, and board serial ADC path;
the next touch work is application-level click/drag regression after
calibration. For LCD, `debug-lcd-dump-ms` gives a headless snapshot path and the
8 MiB default path now reaches a readable 160x240 four-gray main menu. Remaining
display work is interactive backend support plus polarity/palette comparison
against a real unit.
The raw debug NMI remains useful as a diagnostic
because it proves CPU-side NMI
delivery works and that the handler can reach the LCD helper, but it still
does not provide the real status/factor sequence the firmware expects for
normal idle wake.

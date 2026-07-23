# Contributing

This repository carries an experimental S1C33 CPU target and BBK 9288S machine
on top of QEMU 11.0.

Before submitting a change:

1. Keep firmware-specific behavior in `hw/s1c33/` and architectural behavior
   in `target/s1c33/`.
2. Do not add firmware, NAND images, downloaded system files, logs, or build
   output to Git.
3. Preserve the hardware path. Do not add guest-memory injection, system API
   hooks, or application-specific shortcuts as compatibility fixes.
4. Run:

   ```powershell
   python -m py_compile scripts\bbk9288s_*.py
   npm --prefix web ci
   npm --prefix web run build
   git diff --check
   ```

5. Build `qemu-system-s1c33.exe` and test boot, calibration, one NAND restart,
   and Web input.

Changes derived from hardware observation should include the relevant address,
register state, firmware path, or trace evidence in the commit message or
`plan.md`.

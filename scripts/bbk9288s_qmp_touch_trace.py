#!/usr/bin/env python3
"""Run a focused BBK 9288S QMP touch trace."""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


INPUT_ABS_MAX = 0x7FFF
LCD_WIDTH = 160
LCD_HEIGHT = 240


DEFAULT_SNAPSHOTS = [
    "info registers",
    "xp /32xb 0x00040240",
    "xp /16xb 0x02380000",
    "xp /16xb 0x023800b0",
    "xp /32xb 0x022f8550",
    "xp /16xb 0x0238f140",
    "xp /16xb 0x00040268",
    "xp /16xb 0x00040277",
    "xp /16xb 0x00040287",
    "xp /16xb 0x00040147",
    "xp /16xb 0x00048180",
]


def parse_u32(value: str) -> int:
    return int(value, 0)


def parse_point(value: str) -> tuple[int, int]:
    try:
        x_text, y_text = value.split(",", 1)
        return int(x_text, 0), int(y_text, 0)
    except ValueError as err:
        raise argparse.ArgumentTypeError("point must be X,Y") from err


def parse_key_after(value: str) -> tuple[int, str]:
    try:
        point_text, qcode = value.split(",", 1)
        point = int(point_text, 0)
        if point < 1 or not qcode:
            raise ValueError
        return point, qcode
    except ValueError as err:
        raise argparse.ArgumentTypeError(
            "key-after must be POINT_NUMBER,QCODE"
        ) from err


def qmp_recv(sock: socket.socket) -> dict:
    data = b""
    while b"\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("QMP connection closed")
        data += chunk
    return json.loads(data.decode("utf-8"))


def qmp_cmd(sock: socket.socket, execute: str, arguments: dict | None = None) -> dict:
    msg = {"execute": execute}
    if arguments is not None:
        msg["arguments"] = arguments
    sock.sendall((json.dumps(msg) + "\r\n").encode("utf-8"))
    return qmp_recv(sock)


def hmp_cmd(sock: socket.socket, command: str) -> str:
    rsp = qmp_cmd(sock, "human-monitor-command", {"command-line": command})
    return rsp.get("return", "")


def screen_dump(sock: socket.socket, path: Path) -> None:
    rsp = qmp_cmd(sock, "screendump", {"filename": str(path)})
    if "error" in rsp:
        raise RuntimeError(f"screendump failed: {rsp['error']}")


def send_key(sock: socket.socket, qcode: str, hold_ms: int) -> None:
    key = {"type": "qcode", "data": qcode}
    for down in (True, False):
        rsp = qmp_cmd(
            sock,
            "input-send-event",
            {"events": [{"type": "key", "data": {"down": down, "key": key}}]},
        )
        if "error" in rsp:
            raise RuntimeError(f"key event failed: {rsp['error']}")
        time.sleep(hold_ms / 1000.0 if down else 0.05)


def reserve_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def pixel_to_abs(pixel: int, max_pixel: int) -> int:
    pixel = max(0, min(pixel, max_pixel))
    return (pixel * INPUT_ABS_MAX + max_pixel // 2) // max_pixel


def connect_qmp(port: int, timeout: float = 5.0) -> socket.socket:
    deadline = time.time() + timeout
    last_error: OSError | None = None
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError as err:
            last_error = err
            time.sleep(0.05)
    raise RuntimeError(f"could not connect to QMP: {last_error}")


def machine_opts(args: argparse.Namespace) -> str:
    opts = [f"touch-fpt={args.touch_fpt}"]
    if args.trace_key_scan:
        opts.append("trace-key-scan=on")
    if args.trace_io:
        opts.append("trace-io=on")
    opts.extend(args.machine_opt)
    return "bbk9288s" + ("," + ",".join(opts) if opts else "")


def cpu_opts(args: argparse.Namespace) -> str:
    opts = ["c33l05", "exit-on-halt=off"]
    if args.trace_calls or args.trace_calls_start is not None or args.trace_calls_end is not None:
        opts.append("trace-calls=on")
        if args.trace_calls_start is not None:
            opts.append(f"trace-calls-start=0x{args.trace_calls_start:08x}")
        if args.trace_calls_end is not None:
            opts.append(f"trace-calls-end=0x{args.trace_calls_end:08x}")
    if args.trace_exec_start is not None or args.trace_exec_end is not None:
        opts.append("trace-exec=on")
        if args.trace_exec_start is not None:
            opts.append(f"trace-exec-start=0x{args.trace_exec_start:08x}")
        if args.trace_exec_end is not None:
            opts.append(f"trace-exec-end=0x{args.trace_exec_end:08x}")
    if args.trace_mem_start is not None or args.trace_mem_end is not None:
        opts.append("trace-mem=on")
        if args.trace_mem_start is not None:
            opts.append(f"trace-mem-start=0x{args.trace_mem_start:08x}")
        if args.trace_mem_end is not None:
            opts.append(f"trace-mem-end=0x{args.trace_mem_end:08x}")
    if args.trace_mem_pc_start is not None or args.trace_mem_pc_end is not None:
        if "trace-mem=on" not in opts:
            opts.append("trace-mem=on")
        if args.trace_mem_pc_start is not None:
            opts.append(f"trace-mem-pc-start=0x{args.trace_mem_pc_start:08x}")
        if args.trace_mem_pc_end is not None:
            opts.append(f"trace-mem-pc-end=0x{args.trace_mem_pc_end:08x}")
    opts.extend(args.cpu_opt)
    return ",".join(opts)


def send_touch(sock: socket.socket, x_pixel: int, y_pixel: int, down: bool) -> None:
    events = []
    if down:
        events.extend(
            [
                {
                    "type": "abs",
                    "data": {
                        "axis": "x",
                        "value": pixel_to_abs(x_pixel, LCD_WIDTH - 1),
                    },
                },
                {
                    "type": "abs",
                    "data": {
                        "axis": "y",
                        "value": pixel_to_abs(y_pixel, LCD_HEIGHT - 1),
                    },
                },
            ]
        )
    events.append(
        {"type": "btn", "data": {"button": "left", "down": down}}
    )
    qmp_cmd(sock, "input-send-event", {"events": events})


def sample_lines(lines: list[str], limit: int) -> list[str]:
    return lines[:limit]


def summarize_log(path: Path, watch_addrs: list[int],
                  watch_calls: list[int], watch_from: list[int],
                  max_samples: int) -> list[str]:
    text = path.read_text(errors="replace") if path.exists() else ""
    gate_addrs = ["0x02380002", "0x02380004", "0x02380008", "0x023800b0"]
    pen_targets = {
        "0x02061f40",
        "0x020622d4",
        "0x020622ee",
        "0x020622f6",
        "0x02062338",
        "0x02062340",
        "0x02062386",
        "0x0206238e",
        "0x020623d4",
    }
    for addr in watch_addrs:
        formatted = f"0x{addr:08x}"
        if formatted not in gate_addrs:
            gate_addrs.append(formatted)
    call_targets = [f"0x{addr:08x}" for addr in watch_calls]
    call_sources = [f"0x{addr:08x}" for addr in watch_from]
    vectors = re.findall(
        r"bbk9288s-itc: deliver source=([^ ]+) vector=(\d+) level=(\d+)", text
    )
    brks = re.findall(r"s1c33: BRK at pc=(0x[0-9a-fA-F]+) next_pc=(0x[0-9a-fA-F]+)", text)
    adcs = re.findall(
        r"bbk9288s-adc: complete .*ch=(\d+)\.\.(\d+).*touch=(\w+).*x=(\d+) y=(\d+)",
        text,
    )
    pen_calls = []
    watched_calls: dict[str, list[str]] = {addr: [] for addr in call_targets}
    watched_from: dict[str, list[str]] = {addr: [] for addr in call_sources}
    for line in text.splitlines():
        if "s1c33-call:" not in line:
            continue
        match = re.search(r"from=(0x[0-9a-fA-F]+) to=(0x[0-9a-fA-F]+)", line)
        if match is None:
            continue
        source = match.group(1).lower()
        target = match.group(2).lower()
        if target in pen_targets:
            pen_calls.append(line)
        if target in watched_calls:
            watched_calls[target].append(line)
        if source in watched_from:
            watched_from[source].append(line)
    lines = [
        f"log: {path}",
        f"bytes: {path.stat().st_size if path.exists() else 0}",
        f"vectors: {vectors[:12]}",
        f"brk: {brks[:8]}",
        f"adc: {adcs[:12]}",
        f"pen-calls: {sample_lines(pen_calls, max_samples)}",
    ]
    for addr, matching in watched_calls.items():
        lines.append(
            f"{addr}: calls={len(matching)} "
            f"samples={sample_lines(matching, max_samples)}"
        )
    for addr, matching in watched_from.items():
        lines.append(
            f"from {addr}: calls={len(matching)} "
            f"samples={sample_lines(matching, max_samples)}"
        )
    for addr in gate_addrs:
        matching = [line for line in text.splitlines() if addr in line]
        nonzero_writes = [
            line
            for line in matching
            if " W" in line
            and "value=0x00" not in line
            and "value=0x00000000" not in line
        ]
        lines.append(
            f"{addr}: accesses={len(matching)} "
            f"nonzero_writes={sample_lines(nonzero_writes, max_samples)}"
        )
    return lines


def capture_snapshots(sock: socket.socket, commands: list[str],
                      label: str | None = None) -> list[str]:
    snapshots = []
    for command in commands:
        if label is None:
            snapshots.append(f"### {command}\n{hmp_cmd(sock, command)}")
        else:
            snapshots.append(
                f"### {label}: {command}\n{hmp_cmd(sock, command)}"
            )
    return snapshots


def run(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve()
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = root / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    qemu = Path(args.qemu)
    if not qemu.is_absolute():
        qemu = root / qemu
    kernel = Path(args.kernel)
    if not kernel.is_absolute():
        kernel = root / kernel

    stamp = time.strftime("%Y%m%d-%H%M%S")
    prefix = f"{args.prefix}-fpt{args.touch_fpt}-{stamp}"
    log_path = out_dir / f"{prefix}.log"
    stderr_path = out_dir / f"{prefix}.stderr.log"
    state_path = out_dir / f"{prefix}.txt"
    port = reserve_tcp_port()
    points = args.point or [(args.x, args.y)] * args.clicks
    keys_after: dict[int, list[str]] = {}
    for point, qcode in args.key_after:
        keys_after.setdefault(point, []).append(qcode)

    cmd = [
        str(qemu),
        "-M",
        machine_opts(args),
        "-cpu",
        cpu_opts(args),
        "-kernel",
        str(kernel),
        "-rtc",
        f"base={args.rtc_base}",
        "-nographic",
        "-serial",
        "none",
        "-monitor",
        "none",
        "-qmp",
        f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-d",
        args.qemu_log_items,
        "-D",
        str(log_path),
    ]
    if args.paused:
        cmd.insert(7, "-S")

    env = os.environ.copy()
    if args.msys_path:
        env["PATH"] = args.msys_path + os.pathsep + env.get("PATH", "")

    stderr_stream = stderr_path.open("wb")
    proc = subprocess.Popen(
        cmd,
        cwd=str(root),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=stderr_stream,
    )
    sock: socket.socket | None = None
    early_error: Exception | None = None
    screen_paths: list[Path] = []
    try:
        sock = connect_qmp(port)
        qmp_recv(sock)
        qmp_cmd(sock, "qmp_capabilities")
        if args.paused:
            qmp_cmd(sock, "cont")
        time.sleep(args.pre_ms / 1000.0)
        try:
            snapshots = []
            if args.stage_screendumps:
                path = out_dir / f"{prefix}-pre.ppm"
                screen_dump(sock, path)
                screen_paths.append(path)
            if args.timeline_snapshots:
                snapshots.extend(capture_snapshots(sock, args.snapshot, "pre"))
            if not args.no_touch:
                for click, (x, y) in enumerate(points):
                    send_touch(sock, x, y, True)
                    if args.timeline_snapshots:
                        time.sleep(args.snapshot_settle_ms / 1000.0)
                        snapshots.extend(
                            capture_snapshots(sock, args.snapshot,
                                              f"down{click + 1}")
                        )
                    time.sleep(args.hold_ms / 1000.0)
                    if args.timeline_snapshots:
                        snapshots.extend(
                            capture_snapshots(sock, args.snapshot,
                                              f"hold{click + 1}")
                        )
                    send_touch(sock, x, y, False)
                    if args.timeline_snapshots:
                        time.sleep(args.snapshot_settle_ms / 1000.0)
                        snapshots.extend(
                            capture_snapshots(sock, args.snapshot,
                                              f"up{click + 1}")
                        )
                    if click + 1 < len(points):
                        wait_ms = (
                            args.wait_after_ms[click]
                            if click < len(args.wait_after_ms)
                            else args.between_ms
                        )
                        time.sleep(wait_ms / 1000.0)
                        for qcode in keys_after.get(click + 1, []):
                            send_key(sock, qcode, args.key_hold_ms)
                        if args.stage_screendumps:
                            path = out_dir / f"{prefix}-after{click + 1}.ppm"
                            screen_dump(sock, path)
                            screen_paths.append(path)
            if args.key:
                time.sleep(args.key_delay_ms / 1000.0)
            for index, qcode in enumerate(args.key, start=1):
                send_key(sock, qcode, args.key_hold_ms)
                time.sleep(args.key_gap_ms / 1000.0)
                if args.stage_screendumps:
                    path = out_dir / f"{prefix}-key{index}-{qcode}.ppm"
                    screen_dump(sock, path)
                    screen_paths.append(path)
            time.sleep(args.post_ms / 1000.0)
            if args.stage_screendumps:
                path = out_dir / f"{prefix}-post.ppm"
                screen_dump(sock, path)
                screen_paths.append(path)
            if args.timeline_snapshots:
                snapshots.extend(capture_snapshots(sock, args.snapshot, "post"))
            else:
                snapshots.extend(capture_snapshots(sock, args.snapshot))
            state_path.write_text("\n\n".join(snapshots), encoding="utf-8")
            qmp_cmd(sock, "quit")
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=3)
        except (OSError, RuntimeError) as err:
            early_error = err
            state_path.write_text(
                f"QMP connection closed before snapshots: {err}\n",
                encoding="utf-8",
            )
    finally:
        if sock is not None:
            sock.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        stderr_stream.close()

    print(f"state: {state_path}")
    for path in screen_paths:
        print(f"screen: {path}")
    if early_error is not None:
        print(f"early-exit: {early_error}")
    if stderr_path.stat().st_size:
        print(f"stderr: {stderr_path}")
    if args.no_touch:
        print("touch: skipped")
    else:
        absolute_points = [
            (pixel_to_abs(x, LCD_WIDTH - 1),
             pixel_to_abs(y, LCD_HEIGHT - 1))
            for x, y in points
        ]
        print(f"points: pixel={points} abs={absolute_points}")
    print("\n".join(summarize_log(log_path, args.watch_addr,
                                  args.watch_call, args.watch_from,
                                  args.max_samples)))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--qemu", default="build/qemu-system-s1c33.exe")
    parser.add_argument("--kernel", default="build/bbk9288s-test/kernel.bin")
    parser.add_argument(
        "--rtc-base",
        default="localtime",
        help="QEMU RTC base (default: localtime)",
    )
    parser.add_argument("--out-dir", default="build/bbk9288s-test")
    parser.add_argument("--prefix", default="run-qmp-touch")
    parser.add_argument("--x", type=int, default=14)
    parser.add_argument("--y", type=int, default=23)
    parser.add_argument("--point", type=parse_point, action="append",
                        help="click X,Y; repeat for a coordinate sequence")
    parser.add_argument(
        "--key-after",
        type=parse_key_after,
        action="append",
        default=[],
        help="send QCODE after the numbered point and its between delay",
    )
    parser.add_argument(
        "--key",
        action="append",
        default=[],
        help="send QCODE after the touch sequence; repeat for multiple keys",
    )
    parser.add_argument("--key-delay-ms", type=int, default=500)
    parser.add_argument("--key-gap-ms", type=int, default=500)
    parser.add_argument("--key-hold-ms", type=int, default=80)
    parser.add_argument("--touch-fpt", type=int, default=6)
    parser.add_argument("--touch-k5-low-mask", type=parse_u32, default=None)
    parser.add_argument("--clicks", type=int, default=1)
    parser.add_argument("--pre-ms", type=int, default=1200)
    parser.add_argument("--hold-ms", type=int, default=200)
    parser.add_argument("--between-ms", type=int, default=300)
    parser.add_argument(
        "--wait-after-ms",
        type=int,
        action="append",
        default=[],
        help="override the delay after successive points; repeat as needed",
    )
    parser.add_argument("--post-ms", type=int, default=2200)
    parser.add_argument("--timeline-snapshots", action="store_true")
    parser.add_argument("--stage-screendumps", action="store_true")
    parser.add_argument("--snapshot-settle-ms", type=int, default=50)
    parser.add_argument("--paused", action="store_true")
    parser.add_argument("--no-touch", action="store_true")
    parser.add_argument("--trace-key-scan", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--trace-io", action="store_true")
    parser.add_argument("--trace-calls", action="store_true")
    parser.add_argument("--trace-calls-start", type=parse_u32)
    parser.add_argument("--trace-calls-end", type=parse_u32)
    parser.add_argument("--trace-exec-start", type=parse_u32)
    parser.add_argument("--trace-exec-end", type=parse_u32)
    parser.add_argument("--trace-mem-start", type=parse_u32)
    parser.add_argument("--trace-mem-end", type=parse_u32)
    parser.add_argument("--trace-mem-pc-start", type=parse_u32)
    parser.add_argument("--trace-mem-pc-end", type=parse_u32)
    parser.add_argument("--machine-opt", action="append", default=[])
    parser.add_argument("--cpu-opt", action="append", default=[])
    parser.add_argument("--qemu-log-items", default="guest_errors,int")
    parser.add_argument(
        "--msys-path",
        default=r"C:\msys64\ucrt64\bin;C:\msys64\usr\bin",
    )
    parser.add_argument("--watch-addr", type=parse_u32, action="append", default=[])
    parser.add_argument("--watch-call", type=parse_u32, action="append", default=[])
    parser.add_argument("--watch-from", type=parse_u32, action="append", default=[])
    parser.add_argument("--max-samples", type=int, default=8)
    parser.add_argument("--snapshot", action="append", default=[])
    parser.add_argument("--no-default-snapshots", action="store_true")
    args = parser.parse_args()
    if not args.no_default_snapshots:
        args.snapshot = DEFAULT_SNAPSHOTS + args.snapshot
    if args.touch_k5_low_mask is not None:
        args.machine_opt.append(f"touch-k5-low-mask=0x{args.touch_k5_low_mask:x}")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Serve the BBK 9288S Web UI and manage its offline NAND workspace."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from functools import partial
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from typing import BinaryIO, Iterator
from urllib.parse import parse_qs, quote, unquote, urlsplit

from bbk9288s_nand_image import (
    PARTITION_LBA,
    SECTOR_SIZE,
    extract_image,
    pack_image,
    patch_gbk_short_names,
)


MAX_JSON_BYTES = 64 * 1024
MAX_UPLOAD_BYTES = 128 * 1024 * 1024
COPY_CHUNK_SIZE = 1024 * 1024
INVALID_NAME_CHARS = set('<>:"/\\|?*')


class ApiError(Exception):
    def __init__(self, message: str, status: HTTPStatus = HTTPStatus.BAD_REQUEST):
        super().__init__(message)
        self.status = status


def normalize_fat_path(value: str | None) -> str:
    raw = unquote(value or "/").replace("\\", "/")
    parts = [part for part in raw.split("/") if part not in ("", ".")]
    if any(part == ".." for part in parts):
        raise ApiError("路径不能包含上级目录")
    return "/" + "/".join(parts)


def validate_name(value: str) -> str:
    name = value.strip()
    if not name or name in (".", ".."):
        raise ApiError("名称不能为空")
    if len(name) > 120:
        raise ApiError("名称不能超过 120 个字符")
    if any(ord(char) < 32 or char in INVALID_NAME_CHARS for char in name):
        raise ApiError('名称不能包含 <>:"/\\|?* 或控制字符')
    return name


def child_path(parent: str, name: str) -> str:
    parent_path = normalize_fat_path(parent)
    return f"/{name}" if parent_path == "/" else f"{parent_path}/{name}"


class QemuController:
    def __init__(
        self,
        root: Path,
        executable: Path,
        nand: Path,
        websocket_port: int,
        qmp_port: int,
        log_path: Path,
    ) -> None:
        self.root = root
        self.executable = executable
        self.nand = nand
        self.websocket_port = websocket_port
        self.qmp_port = qmp_port
        self.log_path = log_path
        self.process: subprocess.Popen[bytes] | None = None
        self._log: BinaryIO | None = None

    @property
    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def _relative(self, path: Path) -> str:
        resolved = path.resolve()
        try:
            return resolved.relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return resolved.as_posix()

    def start(self) -> None:
        if self.running:
            return
        self._close_finished_process()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log = self.log_path.open("ab", buffering=0)
        args = [
            str(self.executable),
            "-name",
            "BBK 9288S Web",
            "-machine",
            f"bbk9288s,nand-image={self._relative(self.nand)}",
            "-cpu",
            "c33l05,exit-on-halt=off",
            "-rtc",
            "base=localtime",
            "-display",
            (
                "vnc=127.0.0.1:0,"
                f"websocket=0.0.0.0:{self.websocket_port}"
            ),
            "-qmp",
            f"tcp:127.0.0.1:{self.qmp_port},server=on,wait=off",
            "-serial",
            "none",
            "-monitor",
            "none",
        ]
        self.process = subprocess.Popen(
            args,
            cwd=self.root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=self._log,
        )

        deadline = time.monotonic() + 15
        qmp_ready = False
        websocket_ready = False
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                code = self.process.returncode
                self._close_finished_process()
                raise RuntimeError(f"QEMU 启动失败，退出码 {code}")
            if not qmp_ready:
                try:
                    with socket.create_connection(
                        ("127.0.0.1", self.qmp_port), timeout=0.1
                    ):
                        qmp_ready = True
                except OSError:
                    pass
            if not websocket_ready:
                try:
                    with socket.create_connection(
                        ("127.0.0.1", self.websocket_port), timeout=0.1
                    ):
                        websocket_ready = True
                except OSError:
                    pass
            if qmp_ready and websocket_ready:
                if self.process.poll() is None:
                    return
            time.sleep(0.1)
        self.stop()
        raise RuntimeError("QEMU 服务端口启动超时")

    def _read_qmp_response(self, stream: BinaryIO) -> dict:
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError("QMP 连接提前关闭")
            payload = json.loads(line)
            if "return" in payload:
                return payload
            if "error" in payload:
                raise RuntimeError(payload["error"].get("desc", "QMP 命令失败"))

    def _qmp_command(self, execute: str) -> None:
        with socket.create_connection(
            ("127.0.0.1", self.qmp_port), timeout=2
        ) as connection:
            stream = connection.makefile("rwb", buffering=0)
            greeting = json.loads(stream.readline())
            if "QMP" not in greeting:
                raise RuntimeError("无效的 QMP 握手")
            stream.write(b'{"execute":"qmp_capabilities"}\n')
            self._read_qmp_response(stream)
            command = json.dumps({"execute": execute}, separators=(",", ":"))
            stream.write(command.encode("ascii") + b"\n")
            if execute != "quit":
                self._read_qmp_response(stream)

    def stop(self) -> None:
        process = self.process
        if process is None:
            return
        if process.poll() is None:
            try:
                self._qmp_command("quit")
            except (OSError, RuntimeError, json.JSONDecodeError):
                process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        self._close_finished_process()

    def restart(self) -> None:
        self.stop()
        self.start()

    def _close_finished_process(self) -> None:
        if self.process is not None and self.process.poll() is not None:
            self.process = None
        if self.process is None and self._log is not None:
            self._log.close()
            self._log = None


class NandWorkspace:
    def __init__(
        self,
        nand_path: Path,
        flat_path: Path,
        qemu: QemuController,
    ) -> None:
        self.nand_path = nand_path
        self.flat_path = flat_path
        self.qemu = qemu
        self.lock = threading.RLock()
        self.active = False
        self.dirty = False
        self.busy: str | None = None

    @contextmanager
    def open_fs(self, read_only: bool) -> Iterator:
        if not self.active or not self.flat_path.exists():
            raise ApiError("请先进入 NAND 维护模式", HTTPStatus.CONFLICT)
        try:
            from pyfatfs.PyFatFS import PyFatFS
        except ImportError as exc:
            raise RuntimeError("NAND 文件管理需要 pyfatfs") from exc

        fat = PyFatFS(
            str(self.flat_path),
            encoding="ibm437",
            offset=PARTITION_LBA * SECTOR_SIZE,
            preserve_case=True,
            read_only=read_only,
        )
        try:
            yield fat
        finally:
            fat.close()

    @staticmethod
    def _display_name(name: str) -> str:
        try:
            return name.encode("ibm437").decode("gbk")
        except (UnicodeEncodeError, UnicodeDecodeError):
            return name

    def _find_child(self, fat, internal_parent: str, display_name: str):
        target = display_name.casefold()
        for entry in fat.scandir(internal_parent):
            if self._display_name(entry.name).casefold() == target:
                return entry
        return None

    def _resolve_existing(self, fat, display_path: str) -> str:
        normalized = normalize_fat_path(display_path)
        if normalized == "/":
            return "/"
        internal = "/"
        for segment in normalized.strip("/").split("/"):
            entry = self._find_child(fat, internal, segment)
            if entry is None:
                raise ApiError("文件或目录不存在", HTTPStatus.NOT_FOUND)
            internal = child_path(internal, entry.name)
        return internal

    def status(self) -> dict:
        with self.lock:
            self.qemu._close_finished_process()
            return {
                "emulatorRunning": self.qemu.running,
                "maintenance": self.active,
                "dirty": self.dirty,
                "busy": self.busy,
                "maxUploadBytes": MAX_UPLOAD_BYTES,
            }

    def enter(self) -> dict:
        with self.lock:
            if self.active:
                return self.status()
            self.busy = "正在提取 NAND"
            try:
                self.qemu.stop()
                extract_image(self.nand_path, self.flat_path)
                self.active = True
                self.dirty = False
            finally:
                self.busy = None
            return self.status()

    def _commit(self, restart: bool) -> None:
        if not self.active:
            raise ApiError("NAND 维护模式尚未开启", HTTPStatus.CONFLICT)
        if self.dirty:
            patch_gbk_short_names(self.flat_path)
            pack_image(self.flat_path, self.nand_path)
        self.flat_path.unlink(missing_ok=True)
        self.active = False
        self.dirty = False
        if restart:
            self.qemu.start()

    def apply(self) -> dict:
        with self.lock:
            self.busy = "正在回包并重启"
            try:
                self._commit(restart=True)
            finally:
                self.busy = None
            return self.status()

    def cancel(self) -> dict:
        with self.lock:
            if not self.active:
                raise ApiError("NAND 维护模式尚未开启", HTTPStatus.CONFLICT)
            self.busy = "正在放弃更改"
            try:
                self.flat_path.unlink(missing_ok=True)
                self.active = False
                self.dirty = False
                self.qemu.start()
            finally:
                self.busy = None
            return self.status()

    def restart_emulator(self) -> dict:
        with self.lock:
            if self.active:
                raise ApiError("请先应用或放弃 NAND 更改", HTTPStatus.CONFLICT)
            self.busy = "正在重启模拟器"
            try:
                self.qemu.restart()
            finally:
                self.busy = None
            return self.status()

    def _capacity(self, fat) -> tuple[int, int]:
        header = fat.fs.bpb_header
        bytes_per_sector = header["BPB_BytsPerSec"]
        sectors_per_cluster = header["BPB_SecPerClus"]
        total_sectors = header["BPB_TotSec16"] or header["BPB_TotSec32"]
        fat_sectors = header["BPB_FATSz16"]
        root_sectors = (
            header["BPB_RootEntCnt"] * 32 + bytes_per_sector - 1
        ) // bytes_per_sector
        data_sectors = (
            total_sectors
            - header["BPB_RsvdSecCnt"]
            - header["BPB_NumFATs"] * fat_sectors
            - root_sectors
        )
        cluster_count = data_sectors // sectors_per_cluster
        cluster_bytes = bytes_per_sector * sectors_per_cluster
        free_clusters = sum(
            value == 0 for value in fat.fs.fat[2 : 2 + cluster_count]
        )
        return cluster_count * cluster_bytes, free_clusters * cluster_bytes

    def list_directory(self, path: str) -> dict:
        normalized = normalize_fat_path(path)
        with self.lock, self.open_fs(read_only=True) as fat:
            internal = self._resolve_existing(fat, normalized)
            if not fat.isdir(internal):
                raise ApiError("目标不是目录")
            entries = []
            for entry in fat.scandir(internal, namespaces=["details"]):
                details = entry.raw.get("details", {})
                entries.append(
                    {
                        "name": self._display_name(entry.name),
                        "directory": bool(entry.is_dir),
                        "size": int(entry.size or 0),
                        "modified": details.get("modified"),
                    }
                )
            entries.sort(key=lambda item: (not item["directory"], item["name"].casefold()))
            capacity, free = self._capacity(fat)
            return {
                "path": normalized,
                "entries": entries,
                "capacityBytes": capacity,
                "freeBytes": free,
                "dirty": self.dirty,
            }

    def make_directory(self, parent: str, name: str) -> dict:
        normalized_parent = normalize_fat_path(parent)
        clean_name = validate_name(name)
        with self.lock, self.open_fs(read_only=False) as fat:
            internal_parent = self._resolve_existing(fat, normalized_parent)
            if self._find_child(fat, internal_parent, clean_name) is not None:
                raise ApiError("同名文件或目录已经存在", HTTPStatus.CONFLICT)
            fat.makedir(child_path(internal_parent, clean_name))
            self.dirty = True
        return {"path": child_path(normalized_parent, clean_name)}

    def rename(self, source: str, new_name: str) -> dict:
        normalized = normalize_fat_path(source)
        if normalized == "/":
            raise ApiError("不能重命名根目录")
        display_parent = str(PurePosixPath(normalized).parent)
        clean_name = validate_name(new_name)
        with self.lock, self.open_fs(read_only=False) as fat:
            internal = self._resolve_existing(fat, normalized)
            internal_parent = str(PurePosixPath(internal).parent)
            if self._find_child(fat, internal_parent, clean_name) is not None:
                raise ApiError("目标名称已经存在", HTTPStatus.CONFLICT)
            fat.move(internal, child_path(internal_parent, clean_name))
            self.dirty = True
        return {"path": child_path(display_parent, clean_name)}

    def delete(self, target: str) -> dict:
        normalized = normalize_fat_path(target)
        if normalized == "/":
            raise ApiError("不能删除根目录")
        with self.lock, self.open_fs(read_only=False) as fat:
            internal = self._resolve_existing(fat, normalized)
            if fat.isdir(internal):
                fat.removetree(internal)
            else:
                fat.remove(internal)
            self.dirty = True
        return {"deleted": normalized}

    def upload(
        self,
        parent: str,
        name: str,
        source: BinaryIO,
        length: int,
    ) -> dict:
        if length < 0 or length > MAX_UPLOAD_BYTES:
            raise ApiError("单个文件最大支持 128 MiB", HTTPStatus.REQUEST_ENTITY_TOO_LARGE)
        normalized_parent = normalize_fat_path(parent)
        clean_name = validate_name(name)

        with tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024) as staged:
            remaining = length
            while remaining:
                chunk = source.read(min(COPY_CHUNK_SIZE, remaining))
                if not chunk:
                    raise ApiError("上传数据提前结束")
                staged.write(chunk)
                remaining -= len(chunk)
            staged.seek(0)

            with self.lock, self.open_fs(read_only=False) as fat:
                internal_parent = self._resolve_existing(fat, normalized_parent)
                existing = self._find_child(fat, internal_parent, clean_name)
                existing_size = 0
                if existing is not None:
                    if existing.is_dir:
                        raise ApiError("同名目录已经存在", HTTPStatus.CONFLICT)
                    target = child_path(internal_parent, existing.name)
                    existing_size = fat.getsize(target)
                else:
                    target = child_path(internal_parent, clean_name)
                _, free = self._capacity(fat)
                if length > free + existing_size:
                    raise ApiError("NAND 剩余空间不足", HTTPStatus.INSUFFICIENT_STORAGE)
                with fat.openbin(target, "w") as destination:
                    shutil.copyfileobj(staged, destination, length=COPY_CHUNK_SIZE)
                self.dirty = True
        return {
            "path": child_path(normalized_parent, clean_name),
            "size": length,
        }

    def stream_download(
        self,
        target: str,
        destination: BinaryIO,
    ) -> tuple[str, int]:
        normalized = normalize_fat_path(target)
        with self.lock, self.open_fs(read_only=True) as fat:
            internal = self._resolve_existing(fat, normalized)
            if not fat.isfile(internal):
                raise ApiError("目标不是文件")
            name = PurePosixPath(normalized).name
            size = fat.getsize(internal)
            with fat.openbin(internal, "r") as source:
                shutil.copyfileobj(source, destination, length=COPY_CHUNK_SIZE)
            return name, size

    def shutdown(self) -> None:
        with self.lock:
            self.busy = "正在关闭"
            try:
                if self.active:
                    self._commit(restart=False)
                else:
                    self.qemu.stop()
            finally:
                self.busy = None


class WebHandler(SimpleHTTPRequestHandler):
    server_version = "BBK9288SWeb/1.0"

    @property
    def workspace(self) -> NandWorkspace:
        return self.server.workspace  # type: ignore[attr-defined]

    def log_message(self, format: str, *args) -> None:
        sys.stdout.write(
            f"{self.address_string()} - [{self.log_date_time_string()}] "
            f"{format % args}\n"
        )
        sys.stdout.flush()

    def _send_json(self, payload: dict, status: HTTPStatus = HTTPStatus.OK) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ApiError("无效的请求长度") from exc
        if length <= 0 or length > MAX_JSON_BYTES:
            raise ApiError("无效的 JSON 请求")
        try:
            return json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ApiError("无效的 JSON 请求") from exc

    def _api_error(self, error: Exception) -> None:
        if isinstance(error, ApiError):
            self._send_json({"error": str(error)}, error.status)
            return
        self.log_error("API error: %s", error)
        self._send_json(
            {"error": str(error) or "服务器操作失败"},
            HTTPStatus.INTERNAL_SERVER_ERROR,
        )

    def do_GET(self) -> None:
        parsed = urlsplit(self.path)
        if parsed.path == "/api/status":
            self._send_json(self.workspace.status())
            return
        if parsed.path == "/api/nand/list":
            try:
                path = parse_qs(parsed.query).get("path", ["/"])[0]
                self._send_json(self.workspace.list_directory(path))
            except Exception as error:
                self._api_error(error)
            return
        if parsed.path == "/api/nand/download":
            try:
                path = parse_qs(parsed.query).get("path", [""])[0]
                normalized = normalize_fat_path(path)
                with self.workspace.lock, self.workspace.open_fs(True) as fat:
                    internal = self.workspace._resolve_existing(fat, normalized)
                    if not fat.isfile(internal):
                        raise ApiError("目标不是文件")
                    name = PurePosixPath(normalized).name
                    size = fat.getsize(internal)
                    fallback = "".join(
                        char if char.isascii() and char.isalnum() else "_"
                        for char in name
                    ) or "download.bin"
                    self.send_response(HTTPStatus.OK)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(size))
                    self.send_header(
                        "Content-Disposition",
                        (
                            f'attachment; filename="{fallback}"; '
                            f"filename*=UTF-8''{quote(name)}"
                        ),
                    )
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    with fat.openbin(internal, "r") as source:
                        shutil.copyfileobj(
                            source, self.wfile, length=COPY_CHUNK_SIZE
                        )
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as error:
                self._api_error(error)
            return
        super().do_GET()

    def do_POST(self) -> None:
        parsed = urlsplit(self.path)
        try:
            if parsed.path == "/api/nand/open":
                self._send_json(self.workspace.enter())
                return
            if parsed.path == "/api/nand/apply":
                self._send_json(self.workspace.apply())
                return
            if parsed.path == "/api/nand/cancel":
                self._send_json(self.workspace.cancel())
                return
            if parsed.path == "/api/emulator/restart":
                self._send_json(self.workspace.restart_emulator())
                return

            body = self._read_json()
            if parsed.path == "/api/nand/mkdir":
                self._send_json(
                    self.workspace.make_directory(
                        str(body.get("parent", "/")),
                        str(body.get("name", "")),
                    ),
                    HTTPStatus.CREATED,
                )
                return
            if parsed.path == "/api/nand/rename":
                self._send_json(
                    self.workspace.rename(
                        str(body.get("path", "")),
                        str(body.get("name", "")),
                    )
                )
                return
            if parsed.path == "/api/nand/delete":
                self._send_json(self.workspace.delete(str(body.get("path", ""))))
                return
            raise ApiError("API 不存在", HTTPStatus.NOT_FOUND)
        except Exception as error:
            self._api_error(error)

    def do_PUT(self) -> None:
        parsed = urlsplit(self.path)
        if parsed.path != "/api/nand/upload":
            self._send_json({"error": "API 不存在"}, HTTPStatus.NOT_FOUND)
            return
        try:
            query = parse_qs(parsed.query)
            parent = query.get("path", ["/"])[0]
            name = query.get("name", [""])[0]
            try:
                length = int(self.headers.get("Content-Length", "-1"))
            except ValueError as exc:
                raise ApiError("无效的上传长度") from exc
            self._send_json(
                self.workspace.upload(parent, name, self.rfile, length),
                HTTPStatus.CREATED,
            )
        except Exception as error:
            self._api_error(error)


class WebServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, handler, workspace: NandWorkspace):
        super().__init__(address, handler)
        self.workspace = workspace


def build_parser() -> argparse.ArgumentParser:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument("--http-port", type=int, default=8000)
    parser.add_argument("--websocket-port", type=int, default=6081)
    parser.add_argument("--qmp-port", type=int, default=6082)
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--nand", type=Path)
    parser.add_argument("--flat", type=Path)
    parser.add_argument("--dist", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        import pyfatfs  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(
            "pyfatfs is required: install it with 'python -m pip install pyfatfs'"
        ) from exc

    root = args.root.resolve()
    packaged = (root / "qemu-system-s1c33.exe").exists()
    default_runtime = (
        root / "runtime" if packaged else root.parent / "eebbk9288s-runtime"
    )
    runtime_dir = Path(
        args.runtime_dir
        or os.environ.get("BBK9288S_RUNTIME_DIR")
        or default_runtime
    ).resolve()
    qemu_path = (
        args.qemu
        or (root / "qemu-system-s1c33.exe" if packaged else None)
        or root / "build/qemu-system-s1c33.exe"
    ).resolve()
    nand_path = (args.nand or runtime_dir / "nand-user.raw").resolve()
    flat_path = (
        args.flat or runtime_dir / "nand-manager.fat.img"
    ).resolve()
    dist_path = (args.dist or root / "web/dist").resolve()
    log_path = runtime_dir / "web-qemu.stderr.log"

    for path in (qemu_path, nand_path, dist_path):
        if not path.exists():
            raise FileNotFoundError(f"required path is missing: {path}")

    qemu = QemuController(
        root,
        qemu_path,
        nand_path,
        args.websocket_port,
        args.qmp_port,
        log_path,
    )
    workspace = NandWorkspace(nand_path, flat_path, qemu)
    handler = partial(WebHandler, directory=str(dist_path))
    server = WebServer(("0.0.0.0", args.http_port), handler, workspace)

    def request_shutdown(_signum=None, _frame=None) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, request_shutdown)
    signal.signal(signal.SIGTERM, request_shutdown)

    try:
        qemu.start()
        print(f"BBK 9288S Web server: http://127.0.0.1:{args.http_port}/")
        print(
            f"QEMU WebSocket: 0.0.0.0:{args.websocket_port}; "
            f"QMP: 127.0.0.1:{args.qmp_port}"
        )
        sys.stdout.flush()
        server.serve_forever(poll_interval=0.25)
    finally:
        server.server_close()
        workspace.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())

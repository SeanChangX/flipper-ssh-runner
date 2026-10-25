"""Minimal client for the Flipper Zero protobuf RPC over USB serial.

Only the handful of messages the on-device tests need are implemented, with a
tiny hand-written protobuf wire codec (field numbers from the public
flipperzero-protobuf schema, tag 0.25), so no generated code or protoc is needed.

SAFETY: the client is READ-ONLY unless it was created
with allow_write=True. Read-only = device info, storage list/stat/read/md5,
screen capture, lock status, and the CLI `free` command. Every mutating call
(write, delete, mkdir, app start/exit, input) raises WriteNotAllowed otherwise.
Factory reset, RPC reboot, update, settings, datetime and GPIO are not implemented
at all, on purpose; the one restart is power_reboot_os() (CLI, OS mode, write mode).

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass, field
from typing import Iterator

# ---------------------------------------------------------------------------
# protobuf wire format


def enc_varint(v: int) -> bytes:
    if v < 0:
        v += 1 << 64
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def dec_varint(buf: bytes, pos: int) -> tuple[int, int]:
    shift = 0
    v = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def f_varint(num: int, v: int) -> bytes:
    return enc_varint(num << 3 | 0) + enc_varint(int(v))


def f_bytes(num: int, data: bytes) -> bytes:
    return enc_varint(num << 3 | 2) + enc_varint(len(data)) + data


def f_str(num: int, s: str) -> bytes:
    return f_bytes(num, s.encode("utf-8"))


def decode(buf: bytes) -> dict[int, list]:
    """Generic decode: {field_number: [values]} (varints as int, LEN as bytes)."""
    out: dict[int, list] = {}
    pos = 0
    while pos < len(buf):
        key, pos = dec_varint(buf, pos)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, pos = dec_varint(buf, pos)
        elif wt == 2:
            n, pos = dec_varint(buf, pos)
            if pos + n > len(buf):
                raise ValueError("truncated field")
            v = buf[pos : pos + n]
            pos += n
        elif wt == 5:
            v = int.from_bytes(buf[pos : pos + 4], "little")
            pos += 4
        elif wt == 1:
            v = int.from_bytes(buf[pos : pos + 8], "little")
            pos += 8
        else:
            raise ValueError(f"unsupported wire type {wt}")
        out.setdefault(num, []).append(v)
    return out


def first(msg: dict[int, list], num: int, default=None):
    v = msg.get(num)
    return v[0] if v else default


# ---------------------------------------------------------------------------
# Main message field numbers (flipper.proto)

class F:
    COMMAND_ID = 1
    STATUS = 2
    HAS_NEXT = 3
    EMPTY = 4
    STOP_SESSION = 19
    PING_REQ = 5
    PING_RESP = 6
    DEVICE_INFO_REQ = 32
    DEVICE_INFO_RESP = 33
    STORAGE_INFO_REQ = 28
    STORAGE_INFO_RESP = 29
    STAT_REQ = 24
    STAT_RESP = 25
    LIST_REQ = 7
    LIST_RESP = 8
    READ_REQ = 9
    READ_RESP = 10
    WRITE_REQ = 11
    DELETE_REQ = 12
    MKDIR_REQ = 13
    MD5_REQ = 14
    MD5_RESP = 15
    APP_START_REQ = 16
    APP_LOCK_STATUS_REQ = 17
    APP_LOCK_STATUS_RESP = 18
    APP_EXIT_REQ = 47
    SCREEN_STREAM_START = 20
    SCREEN_STREAM_STOP = 21
    SCREEN_FRAME = 22
    SEND_INPUT = 23
    DESKTOP_IS_LOCKED = 66


STATUS_NAMES = {
    0: "OK", 1: "ERROR", 2: "ERROR_DECODE", 3: "ERROR_NOT_IMPLEMENTED", 4: "ERROR_BUSY",
    5: "ERROR_STORAGE_NOT_READY", 6: "ERROR_STORAGE_EXIST", 7: "ERROR_STORAGE_NOT_EXIST",
    8: "ERROR_STORAGE_INVALID_PARAMETER", 9: "ERROR_STORAGE_DENIED",
    10: "ERROR_STORAGE_INVALID_NAME", 11: "ERROR_STORAGE_INTERNAL",
    12: "ERROR_STORAGE_NOT_IMPLEMENTED", 13: "ERROR_STORAGE_ALREADY_OPEN",
    14: "ERROR_CONTINUOUS_COMMAND_INTERRUPTED", 15: "ERROR_INVALID_PARAMETERS",
    16: "ERROR_APP_CANT_START", 17: "ERROR_APP_SYSTEM_LOCKED", 18: "ERROR_STORAGE_DIR_NOT_EMPTY",
    21: "ERROR_APP_NOT_RUNNING", 22: "ERROR_APP_CMD_ERROR",
}
STATUS_NOT_EXIST = 7

KEYS = {"up": 0, "down": 1, "right": 2, "left": 3, "ok": 4, "back": 5}
INPUT_PRESS, INPUT_RELEASE, INPUT_SHORT, INPUT_LONG = 0, 1, 2, 3

FLIPPER_VID_PID = (0x0483, 0x5740)
READ_CHUNK = 512


class WriteNotAllowed(PermissionError):
    """A mutating RPC was attempted without --allow-write (consent gate)."""


class RpcError(RuntimeError):
    def __init__(self, status: int, what: str):
        super().__init__(f"{what}: {STATUS_NAMES.get(status, status)}")
        self.status = status


@dataclass
class Entry:
    path: str
    is_dir: bool
    size: int = 0
    md5: str = ""


@dataclass
class _Reply:
    status: int
    parts: list[dict[int, list]] = field(default_factory=list)


# ---------------------------------------------------------------------------
# transport


def find_port() -> str | None:
    from serial.tools import list_ports

    for p in list_ports.comports():
        if (p.vid, p.pid) == FLIPPER_VID_PID:
            return p.device
    return None


class Transport:
    """Byte transport; the serial one is replaced by a fake in unit tests."""

    def write(self, data: bytes) -> None: ...
    def read(self, n: int, timeout: float) -> bytes: ...
    def close(self) -> None: ...


class SerialTransport(Transport):
    def __init__(self, port: str):
        import serial

        self.s = serial.Serial(port, 230400, timeout=0.1)

    def write(self, data: bytes) -> None:
        self.s.write(data)
        self.s.flush()

    def read(self, n: int, timeout: float) -> bytes:
        self.s.timeout = timeout
        return self.s.read(n)

    def close(self) -> None:
        self.s.close()


# ---------------------------------------------------------------------------
# client


class FlipperRpc:
    def __init__(self, transport: Transport, allow_write: bool = False, log=None):
        self.t = transport
        self.allow_write = allow_write
        self.cmd_id = 0
        self.rx = bytearray()
        self.frames: list[bytes] = []
        self.in_rpc = False
        self.log = log or (lambda *_: None)
        self.mutations: list[str] = []  # audit trail of every mutating call

    # -- session -------------------------------------------------------------
    def _read_until(self, marker: bytes, timeout: float) -> bytes:
        end = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < end:
            chunk = self.t.read(256, 0.05)
            if chunk:
                buf += chunk
                if marker in buf:
                    return bytes(buf)
        raise TimeoutError(f"no {marker!r} from Flipper (got {bytes(buf[-80:])!r})")

    def cli_prompt(self) -> None:
        self.t.write(b"\r")
        self._read_until(b">: ", 3.0)

    def cli(self, command: str, timeout: float = 3.0) -> str:
        """Run a read-only CLI command (allow-list) outside the RPC session."""
        if command not in ("free", "info device", "device_info", "uptime"):
            raise WriteNotAllowed(f"CLI command not on the read-only allow-list: {command}")
        if self.in_rpc:
            self.stop_session()
        self.cli_prompt()
        self.t.write(command.encode() + b"\r")
        out = self._read_until(b">: ", timeout).decode("utf-8", "replace")
        lines = out.replace("\r", "").split("\n")
        return "\n".join(l for l in lines if l and not l.startswith(">:") and l.strip() != command)

    def power_reboot_os(self) -> None:
        """Restart the Flipper into its normal firmware (CLI `power reboot`: OS mode, never
        DFU or update). Only after a restore with an empty diff, never while test files are
        on the SD card; the port goes away and comes back."""
        self._mutating("power reboot (OS)")
        if self.in_rpc:
            self.stop_session()
        self.cli_prompt()
        self.t.write(b"power reboot\r")

    def log_capture(self, seconds: float) -> str:
        """Read-only: the CLI `log` stream for `seconds` (a crash's reason shows up here),
        then Ctrl+C. Leaves the CLI; call start_session() for RPC again."""
        if self.in_rpc:
            self.stop_session()
        self.cli_prompt()
        self.t.write(b"log\r")
        end = time.monotonic() + seconds
        buf = bytearray()
        while time.monotonic() < end:
            try:
                buf += self.t.read(256, 0.05)
            except Exception as e:  # noqa: BLE001 - the port goes away when the Flipper reboots
                buf += f"\n[port lost: {e}]\n".encode()
                break
        try:
            self.t.write(b"\x03")
            buf += self.t.read(4096, 0.3)
        except Exception:  # noqa: BLE001
            pass
        return buf.decode("utf-8", "replace").replace("\r", "")

    def start_session(self) -> None:
        self.cli_prompt()
        self.t.write(b"start_rpc_session\r")
        self._read_until(b"start_rpc_session\r\n", 3.0)
        self.in_rpc = True
        self.rx.clear()

    def stop_session(self) -> None:
        if not self.in_rpc:
            return
        self._send(F.STOP_SESSION, b"")
        try:
            self._wait(self.cmd_id, 3.0)
        except TimeoutError:
            pass
        self.in_rpc = False
        time.sleep(0.2)
        self.t.read(4096, 0.2)  # drain

    def close(self) -> None:
        try:
            self.stop_session()
        finally:
            self.t.close()

    # -- framing -------------------------------------------------------------
    def _send(self, content_field: int, content: bytes, has_next: bool = False) -> int:
        self.cmd_id += 1
        msg = f_varint(F.COMMAND_ID, self.cmd_id) + f_bytes(content_field, content)
        if has_next:
            msg += f_varint(F.HAS_NEXT, 1)
        self.t.write(enc_varint(len(msg)) + msg)
        return self.cmd_id

    def _next_message(self, timeout: float) -> dict[int, list]:
        end = time.monotonic() + timeout
        while True:
            try:
                n, pos = dec_varint(bytes(self.rx), 0)
                if len(self.rx) >= pos + n:
                    raw = bytes(self.rx[pos : pos + n])
                    del self.rx[: pos + n]
                    return decode(raw)
            except ValueError:
                pass
            if time.monotonic() > end:
                raise TimeoutError("RPC reply timeout")
            self.rx += self.t.read(4096, 0.05)

    def _wait(self, cid: int, timeout: float = 5.0) -> _Reply:
        reply = _Reply(status=0)
        while True:
            m = self._next_message(timeout)
            if F.SCREEN_FRAME in m:
                self.frames.append(decode(m[F.SCREEN_FRAME][0]).get(1, [b""])[0])
                continue
            if first(m, F.COMMAND_ID, 0) != cid:
                continue
            reply.status = first(m, F.STATUS, 0)
            reply.parts.append(m)
            if not first(m, F.HAS_NEXT, 0):
                return reply

    def _call(self, content_field: int, content: bytes, what: str, timeout: float = 5.0,
              ok_status: tuple[int, ...] = (0,)) -> _Reply:
        cid = self._send(content_field, content)
        r = self._wait(cid, timeout)
        if r.status not in ok_status:
            raise RpcError(r.status, what)
        return r

    def _mutating(self, what: str) -> None:
        if not self.allow_write:
            raise WriteNotAllowed(
                f"{what} is a mutating RPC; it needs --allow-write, which "
                "covers one on-device session only")
        self.mutations.append(what)
        self.log(f"MUTATE {what}")

    # -- read-only -------------------------------------------------------------
    def ping(self) -> bool:
        r = self._call(F.PING_REQ, f_bytes(1, b"ping"), "ping")
        return first(decode(first(r.parts[-1], F.PING_RESP, b"")), 1, b"") == b"ping"

    def device_info(self) -> dict[str, str]:
        r = self._call(F.DEVICE_INFO_REQ, b"", "device_info", timeout=10)
        out = {}
        for part in r.parts:
            body = first(part, F.DEVICE_INFO_RESP)
            if body is None:
                continue
            d = decode(body)
            out[first(d, 1, b"").decode()] = first(d, 2, b"").decode()
        return out

    def desktop_locked(self) -> bool:
        cid = self._send(F.DESKTOP_IS_LOCKED, b"")
        return self._wait(cid).status == 0

    def app_locked(self) -> bool:
        """True while an app is running (loader lock)."""
        r = self._call(F.APP_LOCK_STATUS_REQ, b"", "lock_status")
        body = first(r.parts[-1], F.APP_LOCK_STATUS_RESP, b"")
        return bool(first(decode(body), 1, 0))

    def stat(self, path: str) -> Entry | None:
        cid = self._send(F.STAT_REQ, f_str(1, path))
        r = self._wait(cid)
        if r.status == STATUS_NOT_EXIST:
            return None
        if r.status != 0:
            raise RpcError(r.status, f"stat {path}")
        f = decode(first(decode(first(r.parts[-1], F.STAT_RESP, b"")), 1, b""))
        return Entry(path, bool(first(f, 1, 0)), first(f, 3, 0))

    def list(self, path: str, md5: bool = False) -> list[Entry]:
        content = f_str(1, path) + (f_varint(2, 1) if md5 else b"")
        r = self._call(F.LIST_REQ, content, f"list {path}", timeout=20)
        out = []
        for part in r.parts:
            body = first(part, F.LIST_RESP)
            if body is None:
                continue
            for fb in decode(body).get(1, []):
                f = decode(fb)
                name = first(f, 2, b"").decode("utf-8", "replace")
                out.append(Entry(
                    path.rstrip("/") + "/" + name,
                    bool(first(f, 1, 0)),
                    first(f, 3, 0),
                    first(f, 5, b"").decode(),
                ))
        return out

    def walk(self, root: str) -> Iterator[Entry]:
        stack = [root]
        while stack:
            d = stack.pop()
            try:
                entries = self.list(d)
            except RpcError as e:
                self.log(f"list {d} failed: {e}")
                continue
            for e in sorted(entries, key=lambda x: x.path):
                yield e
                if e.is_dir:
                    stack.append(e.path)

    def read(self, path: str) -> bytes:
        r = self._call(F.READ_REQ, f_str(1, path), f"read {path}", timeout=30)
        data = bytearray()
        for part in r.parts:
            body = first(part, F.READ_RESP)
            if body is None:
                continue
            f = decode(first(decode(body), 1, b""))
            data += first(f, 4, b"")
        return bytes(data)

    def md5(self, path: str) -> str:
        r = self._call(F.MD5_REQ, f_str(1, path), f"md5 {path}", timeout=30)
        return first(decode(first(r.parts[-1], F.MD5_RESP, b"")), 1, b"").decode()

    def screenshot(self, timeout: float = 3.0) -> bytes:
        """One 1024-byte frame (128x64, 1 bpp, 8 pages of 128 column bytes)."""
        self.frames.clear()
        self._call(F.SCREEN_STREAM_START, b"", "screen_stream_start")
        end = time.monotonic() + timeout
        try:
            while not self.frames and time.monotonic() < end:
                try:
                    m = self._next_message(0.2)
                except TimeoutError:
                    continue
                if F.SCREEN_FRAME in m:
                    self.frames.append(first(decode(m[F.SCREEN_FRAME][0]), 1, b""))
        finally:
            self._call(F.SCREEN_STREAM_STOP, b"", "screen_stream_stop")
        if not self.frames:
            raise TimeoutError("no screen frame")
        return self.frames[-1]

    # -- mutating (gated) ----------------------------------------------------------
    def write(self, path: str, data: bytes) -> None:
        self._mutating(f"write {path} ({len(data)} B)")
        chunks = [data[i : i + READ_CHUNK] for i in range(0, len(data), READ_CHUNK)] or [b""]
        self.cmd_id += 1
        cid = self.cmd_id
        for i, chunk in enumerate(chunks):
            content = f_str(1, path) + f_bytes(2, f_bytes(4, chunk))
            msg = f_varint(F.COMMAND_ID, cid) + f_bytes(F.WRITE_REQ, content)
            if i < len(chunks) - 1:
                msg += f_varint(F.HAS_NEXT, 1)
            self.t.write(enc_varint(len(msg)) + msg)
        r = self._wait(cid, 30)
        if r.status != 0:
            raise RpcError(r.status, f"write {path}")
        if self.md5(path) != hashlib.md5(data).hexdigest():
            raise RpcError(1, f"write {path}: md5 mismatch after write")

    def mkdir(self, path: str) -> None:
        self._mutating(f"mkdir {path}")
        cid = self._send(F.MKDIR_REQ, f_str(1, path))
        r = self._wait(cid)
        if r.status not in (0, 6):  # 6 = already exists
            raise RpcError(r.status, f"mkdir {path}")

    def delete(self, path: str, recursive: bool = False) -> None:
        self._mutating(f"delete {path}{' (recursive)' if recursive else ''}")
        cid = self._send(F.DELETE_REQ, f_str(1, path) + (f_varint(2, 1) if recursive else b""))
        r = self._wait(cid, 30)
        if r.status not in (0, STATUS_NOT_EXIST):
            raise RpcError(r.status, f"delete {path}")

    def app_start(self, name: str, args: str = "") -> None:
        self._mutating(f"app_start {name}")
        self._call(F.APP_START_REQ, f_str(1, name) + (f_str(2, args) if args else b""), f"app_start {name}", 10)

    def app_exit(self) -> None:
        self._mutating("app_exit")
        cid = self._send(F.APP_EXIT_REQ, b"")
        self._wait(cid)

    def input(self, key: str, long: bool = False) -> None:
        self._mutating(f"input {key}{' long' if long else ''}")
        k = KEYS[key]
        seq = [INPUT_PRESS, INPUT_LONG if long else INPUT_SHORT, INPUT_RELEASE]
        for t in seq:
            self._call(F.SEND_INPUT, f_varint(1, k) + f_varint(2, t), f"input {key}")

"""HIL key matrix: SSH keys staged into the ESP32 exactly as the Flipper does it
(KEY_BLOB_*), driven from the ESP32's USB console, against the --rm test sshd.

    venv\\Scripts\\python tools\\hil\\keys_matrix.py [--port COM8] [--only NAME ...] [--cycles 100]

Before: the test sshd runs (tools/hil/README.md) and tools/hil/target/local.env names it
(SSH_TARGET_HOST/PORT/USER, SSH_TARGET_CONTAINER). Nothing else holds the port.

Test keys and passphrases are generated fresh into .cache/hil/keys (git-ignored) with
ssh-keygen and never committed; their public keys are appended to the container's
authorized_keys, which goes away with the --rm container. The ESP32 console prints
generated private keys (keygen, legacy export): test keys only.

What it checks: every type and format, with and without a
passphrase; wrong passphrase re-prompts; a remembered passphrase connects silently and
prompts again after forgetting it; public-only, too short, DSA and unknown files are
refused before any TCP connection; a key the host does not know is offered once and
never followed by another key or a password; RSA signs with rsa-sha2-* only; the staging
buffer reads back as zeros after every attempt; generated and legacy keys work; N key
connects without heap growth.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import re
import secrets
import shutil
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[2]
KEYS = ROOT / ".cache" / "hil" / "keys"
OUT = ROOT / ".cache" / "hil" / "keys_matrix.out"
ENV = ROOT / "tools" / "hil" / "target" / "local.env"


def read_env() -> dict[str, str]:
    env = {}
    for line in ENV.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def keygen_exe() -> str:
    exe = shutil.which("ssh-keygen")
    if not exe:
        sys.exit("ssh-keygen not found (Windows: the OpenSSH client feature)")
    return exe


# ---------------------------------------------------------------- keys


@dataclass
class Case:
    name: str
    args: list[str]  # ssh-keygen arguments for type/size/format
    passphrase: bool = False
    expect: str = "ready"  # ready | failed:<text>
    authorize: bool = True
    note: str = ""
    path: Path = field(default=None)  # type: ignore[assignment]


CASES = [
    Case("ed25519", ["-t", "ed25519"]),
    Case("ed25519-pass", ["-t", "ed25519"], passphrase=True),
    Case("ecdsa256", ["-t", "ecdsa", "-b", "256"]),
    Case("ecdsa384-pass", ["-t", "ecdsa", "-b", "384"], passphrase=True),
    Case("ecdsa521", ["-t", "ecdsa", "-b", "521"]),
    Case("ecdsa521-pass", ["-t", "ecdsa", "-b", "521"], passphrase=True),
    Case("rsa2048", ["-t", "rsa", "-b", "2048"]),
    Case("rsa4096-pass", ["-t", "rsa", "-b", "4096"], passphrase=True),
    Case("rsa2048-pem", ["-t", "rsa", "-b", "2048", "-m", "PEM"]),
    Case("rsa3072-pem-pass", ["-t", "rsa", "-b", "3072", "-m", "PEM"], passphrase=True),
    Case("ecdsa256-pem", ["-t", "ecdsa", "-b", "256", "-m", "PEM"]),
    Case("ecdsa384-pem-pass", ["-t", "ecdsa", "-b", "384", "-m", "PEM"], passphrase=True),
    Case("rsa2048-pkcs8", ["-t", "rsa", "-b", "2048", "-m", "PKCS8"]),
    Case("rsa2048-pkcs8-pass", ["-t", "rsa", "-b", "2048", "-m", "PKCS8"], passphrase=True),
    Case("rsa1024", ["-t", "rsa", "-b", "1024"], expect="failed:RSA key too short"),
    Case("dsa", ["-t", "dsa"], expect="failed:DSA keys are not supported"),
    Case("unknown-to-host", ["-t", "ed25519"], authorize=False, expect="failed:Key not accepted"),
]


def make_keys(cases: list[Case], passphrase: str) -> None:
    KEYS.mkdir(parents=True, exist_ok=True)
    exe = keygen_exe()
    for c in cases:
        c.path = KEYS / c.name
        for p in (c.path, c.path.with_suffix(".pub")):
            p.unlink(missing_ok=True)
        r = subprocess.run([exe, "-q", *c.args, "-N", passphrase if c.passphrase else "", "-C", c.name, "-f", str(c.path)],
                           capture_output=True, text=True)
        if r.returncode:
            c.expect = "skip"
            c.note = "ssh-keygen: " + (r.stderr.strip() or r.stdout.strip())[:60]


def fingerprint(pub_line: str) -> str:
    blob = base64.b64decode(pub_line.split()[1])
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


def file_id(data: bytes) -> str:
    return "file:SHA256:" + base64.b64encode(hashlib.sha256(data).digest()).decode().rstrip("=")


# ---------------------------------------------------------------- sshd


class Sshd:
    def __init__(self, container: str):
        self.c = container
        self.log_mark = 0

    def sh(self, script: str, stdin: str | None = None) -> str:
        r = subprocess.run(["docker", "exec", "-i", self.c, "sh", "-c", script], input=stdin,
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"docker exec failed: {r.stderr.strip()}")
        return r.stdout

    def authorize(self, pub_lines: list[str]) -> None:
        self.sh("cat >> /config/.ssh/authorized_keys", "\n".join(pub_lines) + "\n")

    def debug_log(self) -> None:
        """LogLevel DEBUG3 shows each publickey attempt's algorithm."""
        self.sh("sed -i 's/^#\\?LogLevel.*/LogLevel DEBUG3/' /config/sshd/sshd_config && "
                "s6-svc -r /run/service/svc-openssh-server")
        time.sleep(3)
        self.mark()

    def mark(self) -> None:
        self.log_mark = int(self.sh("wc -l < /config/logs/openssh/current").strip() or 0)

    def new_log(self) -> str:
        return self.sh(f"tail -n +{self.log_mark + 1} /config/logs/openssh/current")


def pubkey_attempts(log: str) -> list[str]:
    """Algorithms of the publickey requests (queries and signatures) since the mark."""
    return re.findall(r"userauth_pubkey: .*?pkalg (\S+)", log)


# ---------------------------------------------------------------- console


class Console:
    def __init__(self, port: str):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.1
        self.s.dtr = False  # never reset the board
        self.s.rts = False
        self.s.open()
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def send(self, line: str, until: str | None = None, timeout: float = 2.0) -> str:
        self.s.write((line + "\r\n").encode())
        out = ""
        end = time.time() + timeout
        while time.time() < end:
            out += self.s.read(4096).decode("utf-8", "replace").replace("\r", "")
            if until and re.search(until, out):
                # let the rest of the line arrive
                time.sleep(0.15)
                out += self.s.read(4096).decode("utf-8", "replace").replace("\r", "")
                break
        return out

    def stage(self, data: bytes, keep: bool = False) -> bool:
        # a console line now and then arrives garbled; the CRC refuses it, so try once more
        return self._stage(data, keep) or self._stage(data, keep)

    def _stage(self, data: bytes, keep: bool) -> bool:
        out = self.send(f"keyblob begin {len(data)}", r"keyblob: \w+")
        # the USB console takes lines of up to 256 characters
        for i in range(0, len(data), HEX_CHUNK):
            out += self.send("keyblob hex " + data[i:i + HEX_CHUNK].hex(), r"keyblob: \w+")
        out += self.send(f"keyblob end 0x{zlib.crc32(data):08x}" + (" keep" if keep else ""), r"keyblob: \w+")
        return "error" not in out

    def nonzero(self) -> int:
        m = re.search(r"keyblob: (\d+) non-zero", self.send("keyblob check", r"non-zero"))
        return int(m.group(1)) if m else -1


HEX_CHUNK = 96
END = r"\[ssh\] (ready|failed[^\n]*|passphrase\?[^\n]*)"


@dataclass
class Result:
    name: str
    ok: bool
    detail: str


class Matrix:
    def __init__(self, con: Console, sshd: Sshd, env: dict[str, str], passphrase: str):
        self.con, self.sshd, self.env, self.pp = con, sshd, env, passphrase
        self.results: list[Result] = []
        self.target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"

    def record(self, name: str, ok: bool, detail: str) -> None:
        self.results.append(Result(name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name:24} {detail}", flush=True)

    def connect(self, profile: str, key_id: str = "", want_pub: bool = False, answers: list[str] | None = None,
                timeout: float = 60) -> tuple[str, str, float]:
        """Returns (final state line, all output, seconds to the final state)."""
        cmd = f"ssh {self.target} -a key -p off -t lan -n {profile}"
        if key_id:
            cmd += f" -k {key_id}"
        if want_pub:
            cmd += " -P"
        t0 = time.time()
        out = self.con.send(cmd, END, timeout)
        answers = list(answers or [])
        while True:
            m = list(re.finditer(END, out))
            last = m[-1].group(1) if m else "timeout"
            if not last.startswith("passphrase?") or not answers:
                break
            # "connecting: Unlocking key" does not match END: this waits for the outcome
            out += self.con.send(f"pp {answers.pop(0)}", END, timeout)
        dt = time.time() - t0
        m = list(re.finditer(END, out))
        return (m[-1].group(1) if m else "timeout"), out, dt

    def bye(self) -> None:
        self.con.send("bye", r"\[ssh\] closed", 5)

    def exec_ok(self, tag: str) -> bool:
        out = self.con.send(f"exec echo ok-{tag}", r"\[ssh\] (exit|lost|signal)", 15)
        return f"ok-{tag}" in out and "exit 0" in out

    # -- the cases --

    def run_case(self, c: Case) -> None:
        if c.expect == "skip":
            self.record(c.name, True, f"skipped ({c.note})")
            return
        data = c.path.read_bytes()
        pub = c.path.with_suffix(".pub").read_text().strip()
        kid = fingerprint(pub)
        self.con.send("forgetpp " + kid, None, 0.5)
        self.sshd.mark()
        if not self.con.stage(data):
            self.record(c.name, False, "staging refused")
            return
        state, out, dt = self.connect("k-" + c.name, kid, answers=[self.pp] if c.passphrase else None)
        zeros = self.con.nonzero()
        log = self.sshd.new_log()
        algos = pubkey_attempts(log)
        detail = f"{state} in {dt:.1f} s, staging buffer {zeros} non-zero, pubkey attempts {algos}"
        if c.expect == "ready":
            ok = state == "ready" and self.exec_ok(c.name.replace("-", "")) and zeros == 0
            if c.args[1] == "rsa":
                ok = ok and algos and all(a.startswith("rsa-sha2-") for a in algos)
            if c.passphrase:
                ok = ok and "passphrase?" in out
            self.bye()
        else:
            want = c.expect.split(":", 1)[1]
            ok = state.startswith("failed") and want in state and zeros == 0
            if c.authorize:
                ok = ok and not algos  # refused before any TCP connection
            else:
                # IdentitiesOnly: one key, offered once (query), no other key, no password
                ok = ok and len(set(algos)) == 1
                ok = ok and not re.search(r"method password|Failed password", log)
        self.record(c.name, ok, detail)

    def wrong_then_right(self, c: Case) -> None:
        pub = c.path.with_suffix(".pub").read_text().strip()
        kid = fingerprint(pub)
        self.con.send("forgetpp " + kid, None, 0.5)
        self.con.stage(c.path.read_bytes())
        state, out, dt = self.connect("k-wrongpp", kid, answers=["not-" + self.pp, self.pp])
        ok = state == "ready" and "Wrong passphrase" in out
        self.record("wrong-passphrase", ok, f"re-prompted: {'Wrong passphrase' in out}, then {state}")
        self.bye()
        # remembered: no prompt now
        self.con.stage(c.path.read_bytes())
        state, out, dt = self.connect("k-remember", kid)
        ok = state == "ready" and "passphrase?" not in out
        self.record("remembered-passphrase", ok, f"{state} without a prompt in {dt:.1f} s")
        self.bye()
        # forgotten: prompts again
        self.con.send("forgetpp " + kid, None, 0.5)
        self.con.stage(c.path.read_bytes())
        state, out, dt = self.connect("k-forgot", kid, answers=[self.pp])
        ok = state == "ready" and "passphrase?" in out
        self.record("forgotten-passphrase", ok, f"prompted again: {'passphrase?' in out}, then {state}")
        self.bye()

    def want_pub(self, c: Case) -> None:
        """Encrypted PEM without a .pub: the ESP32 returns the public key after the first
        parse and remembers the passphrase under the real fingerprint."""
        data = c.path.read_bytes()
        pub = c.path.with_suffix(".pub").read_text().strip()
        fp = fingerprint(pub)
        self.con.send("forgetpp " + fp, None, 0.5)
        self.con.send("forgetpp " + file_id(data), None, 0.5)
        self.con.stage(data)
        state, out, dt = self.connect("k-wantpub", file_id(data), want_pub=True, answers=[self.pp])
        m = re.search(r"\[key\] pub (\S+ \S+)", out)
        same = bool(m) and m.group(1) == " ".join(pub.split()[:2])
        self.record("pem-without-pub", state == "ready" and same, f"{state}, public key returned and equal: {same}")
        self.bye()
        self.con.stage(data)
        state, out, dt = self.connect("k-wantpub2", fp)
        self.record("pem-pub-then-silent", state == "ready" and "passphrase?" not in out,
                    f"by fingerprint: {state} without a prompt")
        self.bye()

    def refusals(self) -> None:
        pub = (KEYS / "ed25519.pub").read_bytes()
        self.sshd.mark()
        self.con.stage(pub)
        state, out, dt = self.connect("k-pubonly")
        ok = "This is a public key" in state and not pubkey_attempts(self.sshd.new_log()) and self.con.nonzero() == 0
        self.record("public-only", ok, state)
        self.con.stage(b"PuTTY-User-Key-File-3: ssh-ed25519\nEncryption: none\n")
        state, out, dt = self.connect("k-putty")
        self.record("putty", "PuTTY" in state, state)
        self.con.stage(b"hello, not a key\n")
        state, out, dt = self.connect("k-junk")
        self.record("not-a-key", "Not a private key file" in state, state)
        self.con.send("keyblob drop", r"keyblob: \w+")
        state, out, dt = self.connect("k-nokey")
        self.record("nothing-staged", "No key file sent" in state, state)
        # a staged key not used within 30 s is wiped
        self.con.stage((KEYS / "ed25519").read_bytes())
        before = self.con.nonzero()
        time.sleep(31)
        after = self.con.nonzero()
        self.record("stage-timeout-wipe", before > 0 and after == 0, f"{before} -> {after} non-zero bytes after 31 s")

    def generated(self) -> None:
        for kind, pp in (("ed25519", ""), ("ecdsa", ""), ("ed25519", self.pp), ("ecdsa", self.pp)):
            name = f"gen-{kind}{'-pass' if pp else ''}"
            out = self.con.send(f"keygen {kind} {pp}".strip(), r"\[key\] (fp \S+|failed[^\n]*)", 120)
            m = re.search(r"\[key\] file (\d+) bytes crc \S+\n([\s\S]*?)\[key\] pub ([^\n]+)\n\[key\] fp (\S+)", out)
            if not m:
                self.record(name, False, "no key printed: " + out[-120:].replace("\n", " | "))
                continue
            pem = m.group(2).encode()
            pem = pem[: int(m.group(1))]
            pub, fp = m.group(3).strip(), m.group(4)
            (KEYS / name).write_bytes(pem)
            (KEYS / (name + ".pub")).write_text(pub + "\n")
            enc = b"none" not in base64.b64decode(b"".join(pem.splitlines()[1:-1]))[15:25]  # the cipher name
            self.sshd.authorize([pub])
            self.con.stage(pem)
            state, sout, dt = self.connect("k-" + name, fp)
            ok = state == "ready" and "passphrase?" not in sout and fingerprint(pub) == fp and enc == bool(pp)
            self.record(name, ok, f"{state} in {dt:.1f} s, encrypted {enc}, remembered at generation")
            self.bye()

    def legacy(self) -> None:
        out = self.con.send("legacy export", r"\[key\] (fp \S+|failed[^\n]*)|legacy export: \S+", 30)
        if "ESP_ERR_NOT_FOUND" in out:
            self.record("legacy-export", True, "no v0 key on this board (already migrated)")
            return
        m = re.search(r"\[key\] file (\d+) bytes crc (\S+)\n([\s\S]*?)\[key\] pub ([^\n]+)\n\[key\] fp (\S+)", out)
        if not m:
            self.record("legacy-export", False, out[-120:].replace("\n", " | "))
            return
        pem = m.group(3).encode()[: int(m.group(1))]
        crc = m.group(2)
        pub = m.group(4).strip()
        # the board's real v0 key: kept locally (git-ignored) in case a migration goes wrong
        (KEYS / "legacy-backup").write_bytes(pem)
        self.sshd.authorize([pub])
        self.con.stage(pem)
        state, sout, dt = self.connect("k-legacy", m.group(5))
        self.record("legacy-export", state == "ready", f"exported key connects: {state}")
        self.bye()
        wrong = self.con.send("legacy delete 0x1", r"legacy delete: \S+")
        still = self.con.send("legacy export", r"\[key\] (fp \S+|failed[^\n]*)|legacy export: \S+", 30)
        # deleting with the right CRC is the Flipper's migration (device walk), not done here
        ok = "ESP_ERR_INVALID_ARG" in wrong and crc in still
        self.record("legacy-delete-guard", ok, f"wrong CRC refused, key still there (right CRC {crc} not sent)")

    def unlocked(self, c: Case, cycles: int) -> None:
        """Agent-like cache: after one unlock an encrypted key connects without the bcrypt
        cost until it is forgotten, locked or the board restarts."""
        kid = fingerprint(c.path.with_suffix(".pub").read_text().strip())
        data = c.path.read_bytes()
        self.con.send("forgetpp " + kid, None, 0.5)

        def go(profile: str, answers=None):
            self.con.stage(data)
            r = self.connect(profile, kid, answers=answers)
            self.bye()
            return r

        state, out, t1 = go("k-cache1", [self.pp])
        state2, out2, t2 = go("k-cache2")
        ok = state == state2 == "ready" and "passphrase?" not in out2 and t2 < 2.5 and t1 > 5
        self.record("unlocked-second-fast", ok, f"first {t1:.1f} s (prompt), second {t2:.1f} s without prompt")
        self.con.send("forgetpp " + kid, None, 0.5)
        state, out, t3 = go("k-cache3", [self.pp])
        self.record("unlocked-forget", state == "ready" and "passphrase?" in out,
                    f"after Forget: prompted {'passphrase?' in out}, {t3:.1f} s")
        self.con.send("keys lock", r"unlocked in RAM", 2)
        state, out, t4 = go("k-cache4")
        self.record("unlocked-lock", state == "ready" and "passphrase?" not in out and t4 > 5,
                    f"after keys lock: no prompt (remembered), unlocks again in {t4:.1f} s")
        state, out, t5 = go("k-cache5")
        self.con.send("restart", None, 8)
        self.con.s.reset_input_buffer()
        n = self.con.send("keys", r"unlocked in RAM", 3)
        state, out, t6 = go("k-cache6")
        self.record("unlocked-restart", state == "ready" and t5 < 2.5 and t6 > 5 and "0 unlocked" in n,
                    f"before restart {t5:.1f} s; after: {n.strip().splitlines()[-1] if n.strip() else '?'}, {t6:.1f} s")
        # heap with an unlocked key present
        self.con.stage(data, keep=True)
        self.connect("k-cache-cycles", kid)
        self.bye()
        out = self.con.send(f"cycles {cycles} true", rf"cycle +{cycles}:[^\n]*\n|connect failed|staging the key failed",
                            40 + 4 * cycles)
        deltas = [int(x) for x in re.findall(r"delta (-?\d+)", out)]
        self.con.send("keyblob drop", r"keyblob: \w+")
        half = deltas[len(deltas) // 2:] or [0]
        ok = len(deltas) == cycles and max(half) - min(half) < 2048
        self.record(f"unlocked-cycles-{cycles}", ok, f"{len(deltas)} cycles, second-half spread {max(half) - min(half)} B")

    def fallback(self) -> None:
        """Quick Connect: a refused default key falls back to the password; saved hosts not."""
        c = KEYS / "unknown-to-host"
        self.con.stage(c.read_bytes())
        cmd = f"ssh {self.target} -a key -p off -t lan -n {self.env['SSH_TARGET_HOST']}:{self.env['SSH_TARGET_PORT']} -F"
        out = self.con.send(cmd, r"\[ssh\] (ready|failed[^\n]*|password\?)", 30)
        asked = "password?" in out
        out2 = self.con.send(f"pw {self.env['SSH_TARGET_PASSWORD']}", r"\[ssh\] (ready|failed[^\n]*)", 20) if asked else ""
        self.record("quick-connect-fallback", asked and "[ssh] ready" in out2,
                    f"password asked after the refused key: {asked}, then {'ready' if '[ssh] ready' in out2 else 'not ready'}")
        self.bye()

    def cycles(self, n: int) -> None:
        c = KEYS / "ed25519"
        pub = c.with_suffix(".pub").read_text().strip()
        self.con.stage(c.read_bytes(), keep=True)
        state, out, dt = self.connect("k-cycles", fingerprint(pub))
        self.bye()
        out = self.con.send(f"cycles {n} true", rf"cycle +{n}:[^\n]*\n|connect failed|staging the key failed", 40 + 4 * n)
        deltas = [int(x) for x in re.findall(r"delta (-?\d+)", out)]
        self.con.send("keyblob drop", r"keyblob: \w+")
        ok = len(deltas) == n and max(deltas[n // 2:]) - min(deltas[n // 2:]) < 2048 and self.con.nonzero() == 0
        self.record(f"cycles-{n}", ok, f"{len(deltas)} cycles, heap delta first/last {deltas[:1]}/{deltas[-1:]}"
                    f", second-half spread {max(deltas[n // 2:]) - min(deltas[n // 2:]) if deltas else '-'} B")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--only", nargs="*", help="case names (or: refusals wrong want-pub generated legacy cycles unlocked fallback)")
    ap.add_argument("--cycles", type=int, default=100)
    args = ap.parse_args()
    env = read_env()
    sshd = Sshd(env.get("SSH_TARGET_CONTAINER", "fes-sshd"))
    passphrase = "pp-" + secrets.token_hex(6)
    want = set(args.only or [])
    cases = [c for c in CASES if not want or c.name in want]
    make_keys(CASES, passphrase)
    sshd.authorize([c.path.with_suffix(".pub").read_text().strip() for c in CASES if c.authorize and c.expect != "skip"])
    sshd.debug_log()
    con = Console(args.port)
    m = Matrix(con, sshd, env, passphrase)
    con.send("bye", None, 1)
    for c in cases:
        m.run_case(c)
    by = {c.name: c for c in CASES}
    if not want or "wrong" in want:
        m.wrong_then_right(by["ecdsa384-pass"])
    if not want or "want-pub" in want:
        m.want_pub(by["rsa3072-pem-pass"])
    if not want or "refusals" in want:
        m.refusals()
    if not want or "generated" in want:
        m.generated()
    if not want or "legacy" in want:
        m.legacy()
    if not want or "cycles" in want:
        m.cycles(args.cycles)
    if not want or "unlocked" in want:
        m.unlocked(by["ed25519-pass"], 30)
    if not want or "fallback" in want:
        m.fallback()
    con.send("keyblob drop", r"keyblob: \w+")
    con.s.close()
    bad = [r for r in m.results if not r.ok]
    lines = [f"{'PASS' if r.ok else 'FAIL'}  {r.name:24} {r.detail}" for r in m.results]
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\n{len(m.results) - len(bad)}/{len(m.results)} passed; log in {OUT.relative_to(ROOT)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

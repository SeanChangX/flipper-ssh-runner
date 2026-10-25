"""On-device session tool: the ONLY way this project's tooling touches a Flipper.

Every session runs snapshot -> test -> restore -> verify -> report, so the Flipper ends
exactly as it started (files and settings under /int and /ext).

    python -m tools.flipper.session info                      # read-only
    python -m tools.flipper.session dryrun                    # read-only: snapshot, restore, verify
    python -m tools.flipper.session test --allow-write        # FAP picked by firmware
    python -m tools.flipper.session verify  tools/flipper/snapshots/<ts>
    python -m tools.flipper.session restore tools/flipper/snapshots/<ts> --allow-write

Rules enforced here:
  * read-only unless --allow-write (rpc.FlipperRpc raises on any mutating call);
    --allow-write covers THIS session only and is never on by default;
  * the test may create/modify only the TOUCH_LIST; restore deletes new paths only
    inside it and writes back backed-up files byte-for-byte;
  * anything that differs after restore is reported, never silently "fixed";
  * no flashing, updating, formatting, factory reset or settings RPCs exist here.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

from . import rpc as rpcmod
from .rpc import FlipperRpc, WriteNotAllowed

ROOT = Path(__file__).resolve().parents[2]
SNAP_ROOT = ROOT / "tools" / "flipper" / "snapshots"
FAP_DEST = "/ext/apps/GPIO/ssh_runner.fap"
APP_DATA = "/ext/apps_data/ssh_runner"
# Paths the test may create or modify; anything else is a bug. Firmware-written files
# found during dry runs are added to tools/flipper/touchlist_extra.json.
# The loader unpacks the FAP's files/ (the built-in examples) to apps_assets on first launch.
APP_ASSETS = "/ext/apps_assets/ssh_runner"
TOUCH_LIST = [FAP_DEST, APP_DATA, APP_ASSETS]
EXTRA_FILE = ROOT / "tools" / "flipper" / "touchlist_extra.json"


def touch_list() -> list[str]:
    extra = json.loads(EXTRA_FILE.read_text()) if EXTRA_FILE.exists() else []
    return TOUCH_LIST + list(extra)


def under(path: str, roots: list[str]) -> bool:
    return any(path == r or path.startswith(r.rstrip("/") + "/") for r in roots)


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def connect(allow_write: bool) -> FlipperRpc:
    port = rpcmod.find_port()
    if not port:
        sys.exit("No Flipper found on USB. Plug it in, unlock it, and close qFlipper.")
    log(f"connecting to Flipper on {port} ({'READ-WRITE' if allow_write else 'read-only'})")
    try:
        t = rpcmod.SerialTransport(port)
    except Exception as e:  # noqa: BLE001
        sys.exit(f"Cannot open {port}: {e}\nIs qFlipper (or another terminal) still open?")
    r = FlipperRpc(t, allow_write=allow_write, log=log)
    r.start_session()
    if not r.ping():
        sys.exit("Flipper did not answer the RPC ping")
    if r.desktop_locked() and allow_write and not r.app_locked():
        # Plain auto-lock (no PIN): unlock the standard way, Back x3 (it is button input, so
        # only with --allow-write). Settings stay untouched.
        log("desktop auto-locked: unlocking with Back x3")
        for _ in range(3):
            r.input("back")
            time.sleep(0.3)
        time.sleep(0.8)
    if r.desktop_locked():
        r.close()
        sys.exit("The Flipper is locked (PIN?). Unlock it, then re-run.")
    return r


# ---------------------------------------------------------------------------
# snapshot


def manifest(r: FlipperRpc, roots=("/int", "/ext")) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for root in roots:
        for e in r.walk(root):
            out[e.path] = {"dir": e.is_dir, "size": 0 if e.is_dir else e.size}
    return out


def md5_files(r: FlipperRpc, paths: list[str]) -> dict[str, str]:
    return {p: r.md5(p) for p in paths}


def snapshot(r: FlipperRpc) -> Path:
    ts = time.strftime("%Y%m%d-%H%M%S")
    d = SNAP_ROOT / ts
    (d / "files").mkdir(parents=True)
    info = r.device_info()
    fw = {k: v for k, v in info.items() if k.startswith(("firmware", "hardware_model", "hardware_name", "protobuf"))}
    log(f"firmware: {fw.get('firmware_version', '?')} {fw.get('firmware_origin_fork', '')} "
        f"({fw.get('firmware_commit', '?')})")
    log("manifest of /int and /ext ...")
    man = manifest(r)
    log(f"  {len(man)} entries")

    files = [p for p, m in man.items() if not m["dir"] and (p.startswith("/int/") or under(p, touch_list()))]
    log(f"backing up {len(files)} files (/int + existing touch-list paths) ...")
    md5 = {}
    for p in files:
        data = r.read(p)
        sum_dev = r.md5(p)
        if hashlib.md5(data).hexdigest() != sum_dev:
            raise RuntimeError(f"read of {p} does not match its md5 on the device")
        md5[p] = sum_dev
        dst = d / "files" / p.lstrip("/")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    (d / "manifest.json").write_text(json.dumps(
        {"created": ts, "firmware": fw, "touch_list": touch_list(), "entries": man, "md5": md5}, indent=1))
    log(f"snapshot saved: {d.relative_to(ROOT)}")
    return d


def load_snapshot(d: Path) -> dict:
    return json.loads((d / "manifest.json").read_text())


# ---------------------------------------------------------------------------
# restore & verify


# System-managed state the firmware rewrites by itself: reported as
# "cosmetic", never counted against an empty diff.
COSMETIC = {"/int/.dolphin.state"}


def restore(r: FlipperRpc, d: Path) -> list[str]:
    """Bring the touch-list and /int back to the snapshot. Returns actions taken."""
    snap = load_snapshot(d)
    tl = snap["touch_list"]
    before = snap["entries"]
    now = manifest(r)
    actions: list[str] = []

    # 1. delete paths created inside the touch-list (deepest first)
    created = sorted((p for p in now if p not in before and under(p, tl)), key=lambda p: -p.count("/"))
    for p in created:
        r.delete(p, recursive=now[p]["dir"])
        actions.append(f"deleted {p}")
    # 2. recreate directories removed inside the touch-list
    for p in sorted(p for p, m in before.items() if m["dir"] and under(p, tl) and p not in now):
        r.mkdir(p)
        actions.append(f"recreated dir {p}")
    # 3. write back every backed-up file that changed or vanished; never the firmware's own
    #    counters (COSMETIC): they are logged, not rewritten
    for p, want in snap["md5"].items():
        if p in COSMETIC:
            continue
        cur = r.md5(p) if p in now and not now[p]["dir"] else None
        if cur != want:
            data = (d / "files" / p.lstrip("/")).read_bytes()
            r.write(p, data)
            actions.append(f"restored {p} ({len(data)} B)")
    return actions


def verify(r: FlipperRpc, d: Path) -> dict[str, list]:
    snap = load_snapshot(d)
    before = snap["entries"]
    now = manifest(r)
    diff: dict[str, list] = {"added": [], "removed": [], "changed": [], "md5": [], "cosmetic": []}
    for p in sorted(set(before) | set(now)):
        if p in COSMETIC:
            if before.get(p) != now.get(p):
                diff["cosmetic"].append(f"{p}: {before.get(p)} -> {now.get(p)}")
        elif p not in before:
            diff["added"].append(p)
        elif p not in now:
            diff["removed"].append(p)
        elif before[p] != now[p]:
            diff["changed"].append(f"{p}: {before[p]} -> {now[p]}")
    for p, want in snap["md5"].items():
        if p in now and not now[p]["dir"]:
            got = r.md5(p)
            if got != want:
                diff["cosmetic" if p in COSMETIC else "md5"].append(f"{p}: {want} -> {got}")
    return diff


def diff_empty(diff: dict[str, list]) -> bool:
    return not any(v for k, v in diff.items() if k != "cosmetic")


def print_diff(diff: dict[str, list]) -> None:
    for it in diff.get("cosmetic", []):
        log(f"VERIFY: cosmetic system state changed (logged): {it}")
    if diff_empty(diff):
        log("VERIFY: diff is EMPTY - the Flipper is exactly as before")
        return
    log("VERIFY: diff is NOT empty - stop and report it; do not fix silently:")
    for k, items in diff.items():
        for it in items:
            print(f"    {k:8} {it}")


def close_app(r: FlipperRpc) -> bool:
    """Close a still-running app with Back presses (the RPC app_exit does not close a FAP
    that ignores it); Right once in case an editor asks "Discard Changes?". True if closed."""
    for i in range(12):
        if not r.app_locked():
            return True
        if i == 8:
            r.input("right")
            time.sleep(0.5)
        r.input("back")
        time.sleep(0.5)
    if r.app_locked():
        r.app_exit()
        time.sleep(1.0)
    return not r.app_locked()


def heap_free(r: FlipperRpc) -> int | None:
    out = r.cli("free")
    for line in out.splitlines():
        if line.lower().startswith("free heap size"):
            return int(line.split(":")[1].strip().split()[0])
    return None


def reconnect_after_reboot(wait_s: float = 60.0) -> FlipperRpc:
    """The link dropped mid-walk (a crash reboots the Flipper): wait for it to come back on
    USB, then reconnect so the restore still runs."""
    log(f"waiting up to {wait_s:.0f} s for the Flipper to come back on USB")
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        time.sleep(3)
        if rpcmod.find_port():
            time.sleep(5)  # let it finish booting
            try:
                return connect(True)
            except SystemExit:
                continue
    sys.exit("The Flipper did not come back on USB: nothing was restored. Once it is back, run "
             "`session --allow-write restore <snapshot>`.")


HEAP_MARKER = f"{APP_DATA}/.demo_heap_report"  # either build appends to .heap_report while it exists
HEAP_REPORT = f"{APP_DATA}/.heap_report"


def heap_report_summary(text: str) -> dict | None:
    """The per-run lines ("entry E low L min_entry A min_exit B [stack S]") -> the lowest
    free heap any run reached, the most any run needed after entry and the least free app
    stack. A spike between two ticks only shows as min_exit < min_entry (the firmware's
    since-boot minimum), so that counts too; after a reboot it is exact."""
    runs = []
    for line in text.splitlines():
        f = line.split()
        if len(f) not in (8, 10) or f[0:8:2] != ["entry", "low", "min_entry", "min_exit"]:
            continue
        if len(f) == 10 and f[8] != "stack":
            continue
        entry, low, min_entry, min_exit = (int(x) for x in f[1:8:2])
        lowest = min(low, min_exit) if min_exit < min_entry else low
        run = {"entry": entry, "lowest": lowest, "need": entry - lowest}
        if len(f) == 10:
            run["stack_free"] = int(f[9])
        runs.append(run)
    if not runs:
        return None
    out = {"runs": len(runs), "lowest_free": min(r["lowest"] for r in runs),
           "max_need": max(r["need"] for r in runs), "min_entry": min(r["entry"] for r in runs)}
    stacks = [r["stack_free"] for r in runs if "stack_free" in r]
    if stacks:
        out["min_stack_free"] = min(stacks)
    return out


def write_report(d: Path, report: dict) -> None:
    (d / "report.json").write_text(json.dumps(report, indent=1))
    lines = [f"# On-device session {d.name}", ""]
    for k, v in report.items():
        if isinstance(v, list):
            lines.append(f"## {k}")
            lines += [f"- {x}" for x in v] or ["- (none)"]
        elif isinstance(v, dict):
            lines.append(f"## {k}")
            lines += [f"- {a}: {b}" for a, b in v.items()]
        else:
            lines.append(f"- **{k}**: {v}")
        lines.append("")
    (d / "report.md").write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# commands


def cmd_info(args) -> int:
    r = connect(False)
    try:
        info = r.device_info()
        for k in sorted(info):
            if k.startswith(("firmware", "hardware_model", "hardware_name", "protobuf", "radio_alive")):
                print(f"{k:28} {info[k]}")
        print(f"{'app_running':28} {r.app_locked()}")
        print(f"{'free_heap':28} {heap_free(r)}")
    finally:
        r.close()
    return 0


def cmd_dryrun(args) -> int:
    """snapshot -> restore -> verify with no test in between: proves the tooling leaves no trace."""
    r = connect(args.allow_write)
    try:
        d = snapshot(r)
        try:
            actions = restore(r, d)
        except WriteNotAllowed as e:
            log(f"restore wanted to write although nothing was tested: {e}")
            actions = ["BLOCKED: restore attempted a write during a read-only dry run"]
        diff = verify(r, d)
    finally:
        r.close()
    print_diff(diff)
    write_report(d, {"kind": "dry run", "restore_actions": actions, "mutations": r.mutations,
                     "diff_empty": diff_empty(diff), "diff": diff})
    log(f"report: {(d / 'report.md').relative_to(ROOT)}")
    return 0 if diff_empty(diff) and not actions else 1


def cmd_reboot(args) -> int:
    """Restart the Flipper, only with --allow-write and only when it matches `snapshot` (a
    restore with an empty diff came first). OS mode, logged."""
    if not args.allow_write:
        sys.exit("reboot needs --allow-write")
    r = connect(True)
    diff = verify(r, Path(args.snapshot))
    if not diff_empty(diff):
        r.close()
        print_diff(diff)
        sys.exit("refusing to reboot: the Flipper does not match the snapshot (restore first)")
    log("REBOOT (OS mode): power reboot")
    r.power_reboot_os()
    r.close()
    time.sleep(8)
    r = reconnect_after_reboot()
    log(f"back after the reboot: {r.cli('uptime').strip()}")
    r.close()
    return 0


def cmd_verify(args) -> int:
    r = connect(False)
    try:
        diff = verify(r, Path(args.snapshot))
    finally:
        r.close()
    print_diff(diff)
    return 0 if diff_empty(diff) else 1


def cmd_restore(args) -> int:
    if not args.allow_write:
        sys.exit("restore writes to the Flipper: it needs --allow-write")
    d = Path(args.snapshot)
    r = connect(True)
    try:
        for a in restore(r, d):
            log(a)
        diff = verify(r, d)
    finally:
        r.close()
    print_diff(diff)
    return 0 if diff_empty(diff) else 1


def clean_app_data(r: FlipperRpc, d: Path) -> None:
    """The walks expect the app's first-run state: the user's own app data (hosts, keys,
    settings) goes for the test and comes back with the restore. Only once the snapshot holds
    every file of it, md5-checked."""
    saved = load_snapshot(d)["md5"]
    files = [e for e in r.walk(APP_DATA) if not e.is_dir] if r.stat(APP_DATA) else []
    missing = [e.path for e in files if e.path not in saved]
    if missing:
        raise RuntimeError(f"not in the snapshot, app data left alone: {missing}")
    log(f"app data: {len(files)} files saved in {d.relative_to(ROOT)}; removing it for the walks")
    r.delete(APP_DATA, recursive=True)


def cmd_test(args) -> int:
    if not args.allow_write:
        sys.exit("test installs and drives the app: it needs --allow-write")
    from . import fapmeta, walks

    r = connect(True)
    info = r.device_info()
    fork = info.get("firmware_origin_fork", "")
    fap_path = args.fap
    if fap_path == "auto":
        flavour = "unleashed" if "unleashed" in fork.lower() else "official"
        # mock walks run the demo build (mock backend), hardware walks the release build
        hw = [n for n in (args.walk or []) if n in walks.HW_WALKS]
        mock = [n for n in (args.walk or list(walks.WALKS)) if n in walks.WALKS]
        if hw and mock:
            r.close()
            sys.exit(f"run mock walks ({', '.join(mock)}) and hardware walks ({', '.join(hw)}) separately: "
                     "they need different builds (demo / release)")
        fap_path = f"flipper-app/dist/{flavour}/{'ssh_runner.fap' if hw else 'ssh_runner_demo.fap'}"
    fap = (ROOT / fap_path).read_bytes()
    major, minor, _ = fapmeta.api_version(fap)
    dev_major = int(info.get("firmware_api_major", -1))
    dev_minor = int(info.get("firmware_api_minor", -1))
    log(f"firmware {info.get('firmware_version', '?')} ({fork or 'official'}) API {dev_major}.{dev_minor}; "
        f"FAP {fap_path} API {major}.{minor}")
    if dev_major != major or dev_minor < minor:
        r.close()
        sys.exit("FAP API does not match the Flipper firmware; nothing was written. Build against "
                 "the firmware the Flipper runs (flipper-app/SDK_VERSION) and retry.")
    report: dict = {"kind": "UI test", "fap": fap_path, "fap_md5": hashlib.md5(fap).hexdigest(),
                    "firmware": f"{info.get('firmware_version', '?')} {fork}".strip()}
    d = snapshot(r)
    walk_ok = False
    try:
        heap_before = heap_free(r)
        r.start_session()
        if args.clean_app_data:
            clean_app_data(r, d)
        log(f"installing {fap_path} -> {FAP_DEST}")
        if not r.stat("/ext/apps/GPIO"):
            r.mkdir("/ext/apps/GPIO")
        r.write(FAP_DEST, fap)
        report_heap = True  # the RAM check of the walks (both builds), without stopping RPC mid-walk
        if report_heap:
            if not r.stat(APP_DATA):
                r.mkdir(APP_DATA)
            r.write(HEAP_MARKER, b"")
        walks.preseed(r)
        results = walks.run_all(
            r,
            ROOT / "docs" / "screenshots",
            only=args.walk,
            log=log,
            hw_shots=ROOT / ".cache" / "hw-shots",
            real_backend=fap_path.endswith("/ssh_runner.fap"),  # the release build
        )
        report["walks"] = results
        if any(w.get("link_lost") for w in results.values()):
            report["link_lost"] = True
            try:
                r.close()
            except Exception:  # noqa: BLE001 - the port is already gone
                pass
            r = reconnect_after_reboot()
        if r.app_locked():
            log("app still running after the walks; closing it with Back")
            close_app(r)
        if report_heap and r.stat(HEAP_REPORT):
            text = r.read(HEAP_REPORT).decode("ascii", "replace")
            for line in text.splitlines():
                log(f"  heap run: {line}")
            report["heap_runs"] = heap_report_summary(text)
            log(f"heap over the walks: {report['heap_runs']}")
        heap_after = heap_free(r)
        report["heap"] = {"before": heap_before, "after": heap_after,
                          "delta": (heap_before - heap_after) if heap_before and heap_after else None}
        walk_ok = all(w["ok"] for w in results.values())
        r.start_session()
    finally:
        # restore + verify always run, even if a walk failed
        if not r.in_rpc:
            r.start_session()
        if r.app_locked():
            log("app still running; closing it with Back before the restore")
            close_app(r)
        actions = restore(r, d)
        diff = verify(r, d)
        app_left = r.app_locked()
        heap_end = heap_free(r)
        r.start_session()
        r.close()
        report.update({"restore_actions": actions, "mutations": r.mutations,
                       "diff_empty": diff_empty(diff), "diff": diff,
                       "app_running_after": app_left, "heap_end": heap_end})
        write_report(d, report)
    for a in actions:
        log(a)
    print_diff(diff)
    log(f"report: {(d / 'report.md').relative_to(ROOT)}")
    if report.get("heap"):
        log(f"heap before/after: {report['heap']}, after the restore: {heap_end}")
    if app_left:
        log("NOT RESTORED: an app is still running on the Flipper")
    before = (report.get("heap") or {}).get("before")
    if before and heap_end and heap_end < before - 8192:
        log(f"WARNING: free heap is {before - heap_end} B lower than before the session")
    return 0 if diff_empty(diff) and walk_ok and not app_left else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="session", description=__doc__.split("\n\n")[0])
    ap.add_argument("--allow-write", action="store_true",
                    help="permit mutating RPCs for this session only")
    # --allow-write is accepted before or after the subcommand
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--allow-write", action="store_true", default=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", parents=[common])
    sub.add_parser("dryrun", parents=[common])
    rb = sub.add_parser("reboot", parents=[common])
    rb.add_argument("snapshot")
    v = sub.add_parser("verify", parents=[common])
    v.add_argument("snapshot")
    rs = sub.add_parser("restore", parents=[common])
    rs.add_argument("snapshot")
    t = sub.add_parser("test", parents=[common])
    t.add_argument("--fap", default="auto", help="auto = pick the Unleashed/official build by firmware")
    t.add_argument("--walk", action="append", help="run only these walks")
    t.add_argument("--clean-app-data", action="store_true",
                   help="start the walks from an empty app data folder (the user's data is in the "
                        "snapshot and comes back with the restore)")
    args = ap.parse_args(argv)
    print("=" * 72)
    print(f"SSH Runner on-device session: {args.cmd} "
          f"({'READ-WRITE' if args.allow_write else 'read-only'})")
    print("=" * 72)
    return {"info": cmd_info, "dryrun": cmd_dryrun, "verify": cmd_verify, "reboot": cmd_reboot,
            "restore": cmd_restore, "test": cmd_test}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())

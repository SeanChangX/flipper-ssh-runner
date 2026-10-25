"""Offline tests for tools/flipper: never touches a real Flipper. SPDX-License-Identifier: GPL-3.0-only"""

import re
import zlib

import pytest

from tools.flipper import kbd, rpc, screen, session, walks


class FakeTransport(rpc.Transport):
    def __init__(self):
        self.sent = bytearray()

    def write(self, data):
        self.sent += data

    def read(self, n, timeout):
        return b""

    def close(self):
        pass


def test_varint_roundtrip():
    for v in (0, 1, 127, 128, 300, 2**31, 2**63 - 1):
        b = rpc.enc_varint(v)
        assert rpc.dec_varint(b, 0) == (v, len(b))
    with pytest.raises(ValueError):
        rpc.dec_varint(b"\x80", 0)


def test_message_decode():
    inner = rpc.f_varint(1, 1) + rpc.f_str(2, "ssh_runner.fap") + rpc.f_varint(3, 1234)
    msg = rpc.f_varint(1, 7) + rpc.f_bytes(8, rpc.f_bytes(1, inner)) + rpc.f_varint(3, 1)
    d = rpc.decode(msg)
    assert rpc.first(d, 1) == 7
    assert rpc.first(d, 3) == 1
    f = rpc.decode(rpc.decode(rpc.first(d, 8))[1][0])
    assert rpc.first(f, 2) == b"ssh_runner.fap"
    assert rpc.first(f, 3) == 1234


@pytest.mark.parametrize(
    "call",
    [
        lambda r: r.write("/ext/x", b"1"),
        lambda r: r.delete("/ext/x"),
        lambda r: r.mkdir("/ext/x"),
        lambda r: r.app_start("/ext/apps/GPIO/ssh_runner.fap"),
        lambda r: r.app_exit(),
        lambda r: r.input("ok"),
    ],
)
def test_mutating_calls_blocked_without_allow_write(call):
    t = FakeTransport()
    r = rpc.FlipperRpc(t, allow_write=False)
    with pytest.raises(rpc.WriteNotAllowed):
        call(r)
    assert t.sent == b"", "nothing may reach the Flipper before the gate"
    assert r.mutations == []


def test_cli_allow_list():
    r = rpc.FlipperRpc(FakeTransport())
    for bad in ("storage format /ext", "update", "power reboot", "factory_reset", "led r 255"):
        with pytest.raises(rpc.WriteNotAllowed):
            r.cli(bad)


def test_no_dangerous_rpcs_exist():
    for name in ("factory_reset", "reboot", "update", "set_datetime", "backup_restore", "gpio_write"):
        assert not hasattr(rpc.FlipperRpc, name)


def simulate(presses, start=kbd.START):
    """Replay presses against a model of kbd.c and return the typed text."""
    row, col = start
    text = ""
    confirmed = False
    for key, lng in presses:
        if key == "ok":
            k = kbd.ROWS[row][col][0]
            if k == kbd.BS:
                text = "" if lng else text[:-1]
            elif k == kbd.ENTER:
                confirmed = True
            else:
                text += kbd.shift(k) if lng else k
        else:
            nxt = kbd.move(row, col, key)
            assert nxt is not None, f"planner used {key} where kbd.c would not move a key"
            row, col = nxt
    return text, confirmed


@pytest.mark.parametrize(
    "text",
    ["user@192.168.1.10", "sudo systemctl restart nginx", "df -h | tail -3 > /tmp/x",
     "Hello World", "a\\b`c^d,e?f", "~$\"'<>:;!#%&*()[]{}=+_", "correct horse", ""],
)
def test_keyboard_plan_types_exactly(text):
    presses = kbd.plan(text)
    typed, confirmed = simulate(presses)
    assert typed == text
    assert confirmed


def test_keyboard_plan_continues():
    p1, state = kbd.plan_ex("pi@", confirm=False)
    p2, _ = kbd.plan_ex("10.0.0.1", start=state)
    typed, confirmed = simulate(p1 + p2)
    assert typed == "pi@10.0.0.1" and confirmed


def test_keyboard_layout_matches_firmware_source():
    """kbd.py's table and shift map are exactly flipper-app/views/kbd_layout.c's."""
    src = (session.ROOT / "flipper-app" / "views" / "kbd_layout.c").read_text(encoding="utf-8")
    c_keys = re.findall(r"\{('(?:\\.|[^'])'|KBD_BS|KBD_ENTER|KBD_SHIFT), (\d+), (\d+)\}", src)
    def py(k):
        if k == "KBD_BS":
            return kbd.BS
        if k == "KBD_ENTER":
            return kbd.ENTER
        if k == "KBD_SHIFT":
            return kbd.SHIFT_KEY
        return k[1:-1].encode().decode("unicode_escape")
    flat = [(py(k), int(x), int(y)) for k, x, y in c_keys]
    assert flat == [key for row in kbd.ROWS for key in row]
    for a, b in re.findall(r"case '((?:\\.|[^']))': return '((?:\\.|[^']))';", src):
        assert kbd.shift(py(f"'{a}'")) == py(f"'{b}'"), (a, b)


def test_screen_png():
    frame = bytearray(1024)
    frame[0] = 0x01  # pixel (0,0)
    frame[1023] = 0x80  # pixel (127,63)
    px = screen.pixels(bytes(frame))
    assert px[0][0] == 1 and px[63][127] == 1 and px[1][0] == 0
    png = screen.to_png(bytes(frame), scale=2)
    assert png.startswith(b"\x89PNG")
    # IHDR width/height
    assert int.from_bytes(png[16:20], "big") == 256 and int.from_bytes(png[20:24], "big") == 128
    with pytest.raises(ValueError):
        screen.pixels(b"\x00" * 10)


def test_touch_list_scope():
    assert session.under("/ext/apps_data/ssh_runner/hosts/nas.conf", session.TOUCH_LIST)
    assert session.under("/ext/apps/GPIO/ssh_runner.fap", session.TOUCH_LIST)
    assert not session.under("/ext/apps_data/ssh_runner2/x", session.TOUCH_LIST)
    assert not session.under("/ext/apps/GPIO/other.fap", session.TOUCH_LIST)
    assert not session.under("/int/.desktop.settings", session.TOUCH_LIST)


def test_session_commands_require_allow_write():
    for cmd in (["restore", "x"], ["test"]):
        with pytest.raises(SystemExit):
            session.main(cmd)


def test_fapmeta_reads_built_faps():
    from tools.flipper import fapmeta

    for flavour in ("unleashed", "official"):
        p = session.ROOT / "flipper-app" / "dist" / flavour / "ssh_runner.fap"
        if not p.exists():
            pytest.skip("FAPs not built yet (run the fap gate step first)")
        major, minor, target = fapmeta.api_version(p.read_bytes())
        assert major >= 80 and target == 7
    with pytest.raises(ValueError):
        fapmeta.api_version(b"not an elf")


def test_allow_write_flag_positions(monkeypatch):
    seen = {}
    monkeypatch.setattr(session, "cmd_info", lambda a: seen.setdefault("a", a.allow_write) or 0)
    session.main(["info"])
    assert seen.pop("a") is False
    monkeypatch.setattr(session, "cmd_info", lambda a: seen.setdefault("a", a.allow_write) and 0)
    session.main(["info", "--allow-write"])
    assert seen.pop("a") is True
    session.main(["--allow-write", "info"])
    assert seen.pop("a") is True


class _GuardRpc:
    """Stands in for FlipperRpc: the app 'runs' for `alive` lock checks, then ends."""

    def __init__(self, alive):
        self.alive, self.keys = alive, []

    def app_locked(self):
        self.alive -= 1
        return self.alive >= 0

    def input(self, key, long=False):
        self.keys.append(key)


def test_walker_stops_sending_keys_when_the_app_ends(tmp_path):
    """If SSH Runner ends mid-walk, typed keys land in another app (Sub-GHz, say). After an
    unexpected exit not one more key may be sent, Back included."""
    from tools.flipper import walks

    r = _GuardRpc(alive=3)
    w = walks.Walker(r, tmp_path, log=lambda *_: None)
    w.in_app = True
    with pytest.raises(walks.AppGone):
        w.type("correct horse")
    assert len(r.keys) == 3  # the keys sent while the app still ran, none after
    w.exit_app()  # must not press Back into whatever runs now
    w.press("ok")  # in_app is off: a key outside the app is the caller's decision
    assert r.keys[-1] == "ok" and r.keys.count("back") == 0


def test_run_all_stops_after_app_gone(monkeypatch, tmp_path):
    from tools.flipper import walks

    ran = []

    def gone(w):
        ran.append("gone")
        raise walks.AppGone("ended")

    def later(w):
        ran.append("later")

    monkeypatch.setattr(walks, "WALKS", {"a": gone, "b": later})
    monkeypatch.setattr(walks.Walker, "shot", lambda self, name, same_ok=False: None)
    res = walks.run_all(object(), tmp_path, log=lambda *_: None)
    assert ran == ["gone"] and res["a"]["app_gone"] and "b" not in res


class _ShotRpc:
    """Just enough FlipperRpc for Walker.shot()."""

    def screenshot(self):
        return bytes(1024)


def test_real_backend_shots_never_go_to_docs(tmp_path):
    # a real-backend build shows real addresses: refused before anything is captured
    w = walks.Walker(_ShotRpc(), walks.PUBLIC_SHOTS, lambda *_: None, real_backend=True)
    with pytest.raises(walks.WalkFailed):
        w.shot("leak")
    assert not (walks.PUBLIC_SHOTS / "leak.png").exists()
    # the same capture into .cache-like folders is fine, and the demo build may use docs
    w = walks.Walker(_ShotRpc(), tmp_path, lambda *_: None, real_backend=True)
    w.shot("ok")
    assert (tmp_path / "ok.png").exists()
    assert walks.is_public_dir(walks.PUBLIC_SHOTS / "sub")
    assert not walks.is_public_dir(tmp_path)


def test_heap_report_summary():
    text = ("entry 60000 low 30000 min_entry 13000 min_exit 13000\n"   # min unchanged: the sampled low
            "entry 58000 low 31000 min_entry 13000 min_exit 9000\n"    # a spike between ticks
            "garbage line\n")
    s = session.heap_report_summary(text)
    assert s == {"runs": 2, "lowest_free": 9000, "max_need": 49000, "min_entry": 58000}
    assert session.heap_report_summary("") is None
    # the app's lowest free stack, when the build reports it
    s2 = session.heap_report_summary("entry 30000 low 20000 min_entry 9000 min_exit 9000 stack 2100\n"
                                     "entry 30000 low 21000 min_entry 9000 min_exit 9000 stack 1800\n")
    assert s2["min_stack_free"] == 1800 and s2["max_need"] == 10000


def test_reboot_is_os_mode_and_gated():
    """The one reboot call: write mode only, and it sends exactly the CLI's OS-mode reboot."""
    r = rpc.FlipperRpc(FakeTransport())
    with pytest.raises(rpc.WriteNotAllowed):
        r.power_reboot_os()

    class Prompt(FakeTransport):
        def read(self, n, timeout):
            return b">: "

    t = Prompt()
    w = rpc.FlipperRpc(t, allow_write=True)
    w.power_reboot_os()
    assert bytes(t.sent).endswith(b"power reboot\r")
    assert b"dfu" not in bytes(t.sent).lower() and b"update" not in bytes(t.sent).lower()


def test_keyboard_on_screen_signature():
    """kbd.on_screen() sees the backspace and save keys' frames, nothing else."""
    px = [[0] * 128 for _ in range(64)]
    assert not kbd.on_screen(px)
    for key in (kbd.BS, kbd.ENTER):
        x, y, w, h = kbd.key_rect(*kbd.find(key))
        for i in range(x, x + w):
            px[y][i] = px[y + h - 1][i] = 1
    assert kbd.on_screen(px)
    px[kbd.key_rect(*kbd.find(kbd.ENTER))[1]][70] = 0
    assert not kbd.on_screen(px)


def test_without_busy_sign():
    from tools.flipper import walks

    a = bytearray(1024)
    b = bytearray(1024)
    b[6 * 128 + 3] = 0xFF  # the spinner turned
    assert walks.without_busy_sign(bytes(a)) == walks.without_busy_sign(bytes(b))
    b[7 * 128 + 40] = 0x01  # the footer text changed
    assert walks.without_busy_sign(bytes(a)) != walks.without_busy_sign(bytes(b))


def test_contains_secret():
    from tools.flipper import walks

    assert not walks.contains_secret(b"User: tester\nHost: 192.168.2.10\n", "test")
    assert walks.contains_secret(b"Password: test\n", "test")
    assert walks.contains_secret(b"x=test", "test")

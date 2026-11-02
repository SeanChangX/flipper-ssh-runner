"""Offline tests for tools/flipper: never touches a real Flipper. SPDX-License-Identifier: GPL-3.0-only"""

import re
import zlib

import pytest

from tools.flipper import kbd, rpc, screen, session, ui, walks


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

    def screenshot(self):
        return bytes(1024)  # a blank screen: not the Security screen


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


# ---------------------------------------------------------------------------
# finding menu items by their pixels (ui.py) instead of counting presses


def _shot(name):
    return screen.from_png((ui.SHOTS / name).read_bytes())


@pytest.mark.parametrize("name", sorted(ui.REFS))
def test_templates_match_the_committed_screenshots(name):
    # the constants are what the committed screenshots give (regenerate: python -m tools.flipper.ui)
    assert ui.template(name) == ui.cut_ref(name)
    assert set(ui.TEMPLATES) == set(ui.REFS)


@pytest.mark.parametrize("name", sorted(ui.REFS))
def test_every_label_is_found_in_its_own_screenshot(name):
    png, (x0, y0, x1, y1) = ui.REFS[name]
    hit = ui.find_px(_shot(png), ui.template(name))
    assert hit and x0 <= hit.x <= x1 and y0 <= hit.y <= y1


def test_labels_and_selection_on_real_frames():
    px = _shot("02-settings.png")  # About (selected), Developer, Security
    assert ui.find_px(px, ui.template("About")).selected
    assert not ui.find_px(px, ui.template("Developer")).selected
    assert not ui.find_px(px, ui.template("Security")).selected
    assert ui.find_px(px, ui.template("Restore Examples")) is None
    px = _shot("x01-settings-restore-examples.png")
    assert ui.find_px(px, ui.template("Restore Examples")).selected
    assert ui.find_px(px, ui.template("About")) is None
    # Home's "Settings" item is not the Settings screen's bold header
    assert ui.find_px(_shot("02-settings.png"), ui.template("Settings")) is None


def _frame(px):
    """Pixels back to an RPC screen frame (8 pages of 128 bytes, LSB on top)."""
    out = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if px[y][x]:
                out[(y // 8) * 128 + x] |= 1 << (y % 8)
    return bytes(out)


def test_on_security_only_on_the_security_screen():
    for png in ("s01-security-plain.png", "s09-security-protected.png"):
        assert ui.on_security(_frame(_shot(png)))
    for png in ("02-settings.png", "x01-settings-restore-examples.png", "s02-protect-confirm.png",
                "52b-home-quick-connect.png", "03-about.png"):
        assert not ui.on_security(_frame(_shot(png)))


class _MenuRpc:
    """A Settings-like submenu drawn from the real label templates: a header band, three
    16-row items, the selected one inverted; Up/Down move and wrap like the stock submenu."""

    def __init__(self, items, cursor=0):
        self.items, self.cursor, self.keys = items, cursor, []

    def app_locked(self):
        return True

    def input(self, key, long=False):
        self.keys.append(key)
        if key == "down":
            self.cursor = (self.cursor + 1) % len(self.items)
        elif key == "up":
            self.cursor = (self.cursor - 1) % len(self.items)

    def screenshot(self):
        px = [[0] * 128 for _ in range(64)]
        top = min(max(self.cursor - 1, 0), max(len(self.items) - 3, 0))
        for row, i in enumerate(range(top, min(top + 3, len(self.items)))):
            y0 = 16 * (row + 1)
            sel = i == self.cursor
            if sel:
                for y in range(y0 + 1, y0 + 15):
                    for x in range(1, 122):
                        px[y][x] = 1
            t = ui.template(self.items[i])
            for dy, bits in enumerate(t.rows):
                for dx in range(t.width):
                    if bits >> dx & 1:
                        px[y0 + 5 + dy][6 + dx] = 0 if sel else 1
        return _frame(px)


def _menu_walker(tmp_path, rpc_):
    w = walks.Walker(rpc_, tmp_path, log=lambda *_: None)
    w.in_app = True
    w.settle = lambda timeout=15.0: rpc_.screenshot()
    return w


def test_select_finds_items_whatever_the_debug_flag_and_cursor(tmp_path):
    for items in (["About", "Developer", "Security", "Restore Examples"],   # Debug on
                  ["About", "Security", "Restore Examples"]):               # Debug off
        for cursor in range(len(items)):                                    # remembered item
            r = _MenuRpc(items, cursor)
            w = _menu_walker(tmp_path, r)
            w.select("Restore Examples")
            assert items[r.cursor] == "Restore Examples" and "ok" not in r.keys


def test_select_skips_a_missing_developer(tmp_path):
    r = _MenuRpc(["About", "Security", "Restore Examples"])
    w = _menu_walker(tmp_path, r)
    with pytest.raises(walks.WalkSkipped):
        walks._settings_item(w, "Developer")
    assert "ok" not in r.keys  # nothing was opened on the way round


class _FrozenRpc(_MenuRpc):
    """The app is still starting: the first `deaf` Down presses change nothing."""

    def __init__(self, items, deaf):
        super().__init__(items)
        self.deaf = deaf

    def input(self, key, long=False):
        if self.deaf:
            self.deaf -= 1
            self.keys.append(key)
            return
        super().input(key, long)


def test_select_does_not_skip_on_a_screen_that_ignores_keys(tmp_path):
    # a starting app ignored the first Down: not "no Settings in this menu"
    r = _FrozenRpc(["About", "Security", "Restore Examples"], deaf=1)
    _menu_walker(tmp_path, r).select("Restore Examples")
    assert r.items[r.cursor] == "Restore Examples"
    # a screen that never moves fails clearly instead of skipping the walk
    r = _FrozenRpc(["About", "Security"], deaf=99)
    with pytest.raises(walks.WalkFailed, match="did not move"):
        _menu_walker(tmp_path, r).select("Restore Examples", limit=5)


class _SecurityRpc(_GuardRpc):
    def __init__(self):
        super().__init__(alive=100)

    def screenshot(self):
        return _frame(_shot("s01-security-plain.png"))


def test_walk_on_security_by_accident_sends_nothing_more(tmp_path):
    r = _SecurityRpc()
    w = walks.Walker(r, tmp_path, log=lambda *_: None)
    w.in_app = True
    for send in (lambda: w.press("right"), lambda: w.press("ok"), lambda: w.type("x")):
        with pytest.raises(walks.SecurityReached):
            send()
    assert r.keys == []
    w.allow_security = True  # w_security, on the demo build, goes there on purpose
    w.press("back")
    assert r.keys == ["back"]


def test_close_app_never_presses_right_on_security():
    r = _SecurityRpc()
    r.app_exit = lambda: None
    session.close_app(r)
    assert "right" not in r.keys


def test_run_all_reports_a_skipped_walk(monkeypatch, tmp_path):
    def needs_debug(w):
        raise walks.WalkSkipped("no Developer")

    monkeypatch.setattr(walks, "WALKS", {"a": needs_debug})
    res = walks.run_all(object(), tmp_path, log=lambda *_: None)
    assert res["a"]["ok"] and res["a"]["skipped"] == "no Developer"

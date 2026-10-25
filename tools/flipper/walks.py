"""Scripted UI walks for SSH Runner on the real Flipper (mock backend).

Each walk launches the app, drives it with button presses, saves screenshots to
docs/screenshots/, and must end with the app exited by Back presses alone
(Back works on every screen, and the app exits cleanly from every screen).
Menu positions below mirror the scene code in flipper-app/scenes/.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import time
from pathlib import Path

from . import kbd, screen
from .rpc import FlipperRpc

FAP = "/ext/apps/GPIO/ssh_runner.fap"
APP_DATA_DIR = "/ext/apps_data/ssh_runner"
SCRIPTS = "/ext/apps_data/ssh_runner/scripts"

# Every output line fits one Output row (20 FontKeyboard characters) and says what it
# is: a bare `df -h` row read as noise on the screen.
SAMPLE_SCRIPT = b"""#!/bin/bash
# sample script used by the on-device UI walk
set -e
echo "backup: starting"
hostname
echo "copied 128 files"
echo "disk 61% used"
echo "backup: done"
exit 0
"""


class WalkFailed(RuntimeError):
    pass


class AppGone(WalkFailed):
    """SSH Runner stopped running in the middle of a walk. Every later key would land in
    the desktop or another app (typed keys can open Sub-GHz > Frequency Analyzer),
    so the whole run stops: no more keys at all, straight to restore."""


PUBLIC_SHOTS = Path(__file__).resolve().parents[2] / "docs" / "screenshots"


def is_public_dir(d: Path) -> bool:
    """docs/screenshots (or below): files there are committed and published."""
    try:
        Path(d).resolve().relative_to(PUBLIC_SHOTS.resolve())
        return True
    except ValueError:
        return False


class Walker:
    def __init__(self, r: FlipperRpc, shots: Path, log, real_backend: bool = False):
        self.r = r
        self.shots = shots
        # a release (real-backend) build shows the real network: never into git
        self.real_backend = real_backend
        self.log = log
        self.taken: list[str] = []
        self.kbd_state = kbd.START
        self.locks = 0
        self.last_frame: bytes | None = None
        self.in_app = False  # SSH Runner was launched by this walk and must still run
        # FES_WALK_TRACE=<dir>: a frame after every key press, to see where a walk went off
        import os

        trace = os.environ.get("FES_WALK_TRACE")
        self.trace = Path(trace) if trace else None
        self.trace_n = 0
        if self.trace:
            self.trace.mkdir(parents=True, exist_ok=True)

    def _trace(self, key: str) -> None:
        if not self.trace:
            return
        self.trace_n += 1
        (self.trace / f"{self.trace_n:04d}-{key}.png").write_bytes(screen.to_png(self.r.screenshot()))

    def _guard(self) -> None:
        """Before every key while in the app: is an app still running? If not, SSH Runner
        ended unexpectedly; send nothing more."""
        if self.in_app and not self.r.app_locked():
            self.in_app = False
            raise AppGone("SSH Runner is no longer running; no more keys are sent")

    def unlock_if_locked(self) -> None:
        """Auto-lock (plain lock, no PIN) may engage between walks: unlock the standard
        way, Back x3. A PIN lock is never entered; the walk fails instead."""
        if not self.r.desktop_locked():
            return
        self.locks += 1
        self.log("  desktop is locked (auto-lock); unlocking with Back x3")
        self.press("back", n=3, delay=0.3)
        self.wait(0.8)
        if self.r.desktop_locked():
            raise WalkFailed("desktop still locked after Back x3 (PIN lock?); stopping")

    def launch(self) -> None:
        self.unlock_if_locked()
        self.r.app_start(FAP)
        self.wait(1.2)
        if not self.r.app_locked():
            raise WalkFailed("app did not start")
        self.in_app = True
        self.wait_ready()

    def wait_ready(self, min_s: float = 2.5, timeout: float = 12.0) -> None:
        """The loader shows an hourglass while the FAP loads from SD; keys sent then are
        lost and every later step is off by one. Wait until the screen has stopped
        changing (3 equal frames 0.4 s apart) and at least min_s has passed."""
        t0 = time.monotonic()
        same, prev = 0, None
        while time.monotonic() - t0 < timeout:
            frame = self.r.screenshot()
            same = same + 1 if frame == prev else 0
            prev = frame
            ink = sum(bin(b).count("1") for b in frame)
            # the loading hourglass is ~150 lit pixels; every app screen has far more
            if same >= 2 and ink > 400 and time.monotonic() - t0 >= min_s:
                self.log(f"  app ready after {time.monotonic() - t0:.1f} s ({ink} px)")
                return
            time.sleep(0.4)
        raise WalkFailed("app screen never settled after launch")

    def settle(self, timeout: float = 15.0) -> bytes:
        """The frame once the screen has stopped changing (3 equal frames 0.4 s apart)."""
        t0 = time.monotonic()
        same, prev = 0, None
        while time.monotonic() - t0 < timeout:
            self._guard()
            frame = self.r.screenshot()
            same = same + 1 if frame == prev else 0
            if same >= 2:
                return frame
            prev = frame
            time.sleep(0.4)
        raise WalkFailed(f"screen never settled within {timeout:.0f} s")

    def keyboard_up(self, frame: bytes | None = None) -> bool:
        return kbd.on_screen(screen.pixels(frame if frame is not None else self.r.screenshot()))

    def wait_prompt_or_keyboard(self, timeout: float = 30.0) -> bool:
        """After a connect starts: True once the host-key prompt is up (its stock right
        button fills the bottom-right corner; the Connecting screen has none), False once
        the password keyboard is (the board already trusts the host)."""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            frame = self.settle()
            px = screen.pixels(frame)
            if kbd.on_screen(px):
                return False
            if all(px[63][x] for x in range(112, 128)):
                return True
            time.sleep(0.4)
        raise WalkFailed(f"neither a host-key prompt nor a keyboard within {timeout:.0f} s")

    def wait_keyboard(self, timeout: float = 20.0) -> None:
        """Keys typed before the keyboard is up land on the screen before it (a connect can
        take seconds): wait for it instead of a fixed delay."""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            self._guard()
            if self.keyboard_up():
                time.sleep(0.4)  # the view takes input once drawn
                return
            time.sleep(0.4)
        raise WalkFailed(f"no keyboard within {timeout:.0f} s")

    def press(self, key: str, n: int = 1, long: bool = False, delay: float = 0.4) -> None:
        for _ in range(n):
            self._guard()
            self.r.input(key, long=long)
            time.sleep(delay)
            self._trace(("long-" if long else "") + key)

    def type(self, text: str, confirm: bool = True, clear_first: bool = False, fresh: bool = True) -> None:
        """Type on the keyboard. fresh=True for a keyboard that was just opened;
        fresh=False continues from where the previous type() left the cursor."""
        start = kbd.START if fresh else self.kbd_state
        presses, self.kbd_state = kbd.plan_ex(text, clear_first=clear_first, confirm=confirm, start=start)
        for key, lng in presses:
            self._guard()
            self.r.input(key, long=lng)
            time.sleep(0.04)
        time.sleep(0.2)

    def heap(self, tag: str) -> None:
        """Log the free heap while the app runs (CLI `free`, read-only), for RAM checks."""
        out = self.r.cli("free")
        self.r.start_session()
        for line in out.splitlines():
            low = line.lower()
            if low.startswith("free heap size") or low.startswith("minimum heap size"):
                self.log(f"  heap at {tag}: {line.strip()}")

    def wait(self, s: float) -> None:
        time.sleep(s)

    def shot(self, name: str, same_ok: bool = False) -> None:
        """Save the screen. Each shot must differ from the walk's previous one: an identical
        frame means a key press was lost and the file would claim a state it does not show."""
        if self.real_backend and is_public_dir(self.shots):
            raise WalkFailed(f"{name}: a real-backend screen would go to {self.shots}, which is committed; "
                             "real-backend screenshots belong in .cache/ only")
        frame = self.r.screenshot()
        if not same_ok and frame == self.last_frame:
            raise WalkFailed(f"{name}: the screen did not change since the previous shot")
        self.last_frame = frame
        self.shots.mkdir(parents=True, exist_ok=True)
        (self.shots / f"{name}.png").write_bytes(screen.to_png(frame))
        self.taken.append(name)
        self.log(f"  screenshot {name}")

    def exit_app(self, max_backs: int = 15) -> None:
        if not self.in_app:
            # never press Back into whatever else may be running
            self.log("  SSH Runner is not running: no Back presses")
            return
        for i in range(max_backs):
            if not self.r.app_locked():
                self.in_app = False
                self.log(f"  app exited after {i} Back presses")
                return
            if i == 8:
                # an editor's "Discard Changes?" dialog answers Back with Cancel: Discard
                self.log("  still running after 8 Back presses: Right (Discard) once")
                self.press("right", delay=0.5)
            self.press("back", delay=0.35)
        if self.r.app_locked():
            raise WalkFailed(f"app still running after {max_backs} Back presses")


# ---------------------------------------------------------------------------
# walks (run in this order; later walks rely on data created by earlier ones)


def w_tour(w: Walker) -> None:
    """First launch: real backend without a board, then switch to the mock backend."""
    w.launch()
    w.wait(0.5)
    # No screenshot of the real backend here: with the board wired it shows a live
    # address. The board states are captured on the mock by the "board" walk.
    w.press("down", 5)  # Settings
    w.press("ok")
    w.shot("02-settings")
    w.press("ok")  # About
    w.wait(0.3)
    w.shot("03-about")
    w.press("back")
    w.press("down")  # Developer
    w.press("ok")
    w.shot("04-developer")
    w.press("right")  # Mock backend: On
    w.wait(0.8)
    w.shot("05-developer-mock-on")
    w.press("back")
    w.press("back")
    w.wait(0.5)
    w.shot("06-home-demo")
    w.exit_app()


def w_quick_connect(w: Walker) -> None:
    """Quick Connect, trust a new key, password, run a one-liner, save as alias."""
    w.launch()
    w.wait(0.6)
    w.press("ok")  # Quick Connect
    w.shot("07-quick-connect-empty")
    w.type("user@192.168.1.10", confirm=False)
    w.shot("08-quick-connect-typed")
    w.type("", confirm=True, fresh=False)
    w.shot("09-connecting")  # at once: the mock checks the host after 700 ms
    w.wait(1.0)
    w.shot("10-hostkey-new")
    w.press("right")  # Trust
    w.wait(0.6)
    w.shot("11-password-empty")
    w.type("hunter2", confirm=False)
    w.shot("12-password-typed")
    w.type("", confirm=True, fresh=False)
    w.wait(1.2)
    w.shot("13-run-menu-quick")
    w.press("down", 2)  # Type command
    w.shot("13b-run-menu-type-command")
    w.press("ok")
    w.type("uname -a", confirm=False)
    w.shot("13c-command-typed")
    w.type("", confirm=True, fresh=False)
    w.wait(1.0)
    w.shot("14-output-uname")
    w.press("back")  # offers Save as alias
    w.wait(0.3)
    w.shot("15-save-alias-offer")
    w.press("right")  # Save
    w.shot("16-alias-suggested")
    w.type("nas", confirm=False)
    w.shot("16b-alias-typed")
    w.type("", confirm=True, fresh=False)
    w.wait(0.3)
    w.shot("17-saved")
    w.press("ok")  # close the result screen (OK, no timeout)
    w.shot("18-run-menu-saved-alias")
    w.exit_app()


def w_saved_host(w: Walker) -> None:
    """Hosts list, editor, saved command, repeat run, scripts, long output."""
    w.r.mkdir(SCRIPTS)
    w.r.write(f"{SCRIPTS}/backup.sh", SAMPLE_SCRIPT)
    w.launch()
    w.wait(0.6)
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)  # the list must be up before OK
    w.shot("19-hosts")
    w.press("ok")  # the host page, on Connect
    w.shot("20-host-page")
    w.press("down")  # Alias
    w.shot("21-host-edit")
    w.press("down", 2)  # Host: the address is in the label, drawn whole, no marquee
    w.shot("21b-host-selected")
    w.press("up", 2)
    w.press("down", 7)
    w.shot("22-host-edit-lower")
    w.press("down")  # Commands
    w.press("ok")
    w.shot("23-host-cmds-empty")
    w.press("ok")  # + Add command
    w.type("sudo systemctl restart nginx")
    w.shot("24-host-cmds")
    w.press("up")  # from "+ Add Command" to the command
    w.press("ok")  # the whole command, Edit / Delete
    w.shot("24b-command-detail-editor")
    w.press("back")
    w.press("back")
    w.press("down", 2)  # Save
    w.press("ok")
    w.wait(0.3)
    w.shot("25-host-saved")
    w.press("ok")  # close the result screen (OK, no timeout): the Hosts list
    # Repeat run: Hosts > nas > Connect (keys are pinned per alias, so the aliased profile
    # asks once more).
    w.press("ok")  # nas
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(1.2)
    w.shot("26-hostkey-new-alias")
    w.press("right")
    w.wait(0.6)
    w.type("hunter2")
    w.wait(1.2)
    w.shot("27-run-menu-host")
    w.press("ok", long=True)  # the whole command, Edit / Run / Delete
    w.shot("27b-command-detail")
    w.press("back")
    w.press("ok")  # $ sudo systemctl restart nginx
    w.wait(1.0)
    w.shot("28-output-error")
    w.press("back")
    w.press("down", 4)  # Scripts
    w.press("ok")
    w.shot("29-scripts")
    w.press("ok")
    w.shot("30-script-confirm")
    w.press("right")  # Run
    w.wait(1.2)
    w.shot("31-output-script")
    w.press("back")
    w.press("up")  # Type command
    w.press("ok")
    w.type("seq 3000")
    w.wait(0.6)
    w.shot("32-output-running")
    w.wait(3.5)
    w.shot("33-output-long-end")
    w.press("up", 150, delay=0.1)  # gently (0.05 s once dropped the USB link); far enough that the proportional thumb visibly moves
    w.shot("34-output-long-scrolled")
    w.press("back")
    w.press("down", 3)  # Save last output
    w.press("ok")
    w.wait(1.5)  # it creates logs/<alias>/ first
    w.shot("35-output-saved")
    w.press("ok")  # close the result screen (OK, no timeout)
    w.exit_app()


def w_fleet(w: Walker) -> None:
    """Same IP, different device: mismatch prompt, options, cancel; Recent."""
    w.launch()
    w.wait(0.6)
    w.press("ok")
    w.type("pi@192.168.4.1")
    w.wait(1.0)
    w.press("right")  # Trust
    w.wait(0.6)
    w.type("raspberry")
    w.wait(1.2)
    w.press("back")  # disconnect
    w.wait(0.5)
    w.press("down", 2)  # Recent
    w.press("ok")
    w.shot("36-recent")
    w.press("ok")  # pi@192.168.4.1 again: another robot answers
    w.wait(1.2)
    w.shot("37-hostkey-changed")
    w.press("down", 3, delay=0.1)
    w.shot("38-hostkey-changed-scrolled")
    w.press("right")  # Options
    w.shot("39-hostkey-options")
    w.press("ok")  # Cancel (default)
    w.wait(0.5)
    w.shot("40-recent-after-cancel")
    w.press("ok", long=True)
    w.shot("41-recent-menu")
    w.exit_app()


def w_network(w: Walker) -> None:
    """Network status, LAN settings, WiFi scan and join, ping, keys."""
    w.launch()
    w.wait(0.6)
    w.press("down", 3)  # Network
    w.press("ok")
    w.wait(0.3)
    w.shot("42-network")
    w.press("ok")  # LAN
    w.shot("43-lan")
    w.press("right")  # Mode (selected on entry): Static
    w.wait(0.3)
    w.shot("44-lan-static")
    w.press("left")  # back to DHCP, leave unchanged
    w.press("back")
    w.press("down", 2)  # Join WiFi
    w.press("ok")
    w.wait(0.3)
    w.shot("45-wifi-scanning")
    w.wait(1.8)
    w.shot("46-wifi-list")
    w.press("ok")  # Lab-WiFi
    w.type("correct horse", confirm=False)
    w.shot("47-wifi-password")
    w.type("", confirm=True, fresh=False)
    w.wait(0.4)
    w.shot("48-wifi-joining")
    w.wait(2.2)
    w.shot("49-wifi-joined")
    w.press("ok")  # close the result screen (OK, no timeout)
    w.shot("50-network-wifi-joined")
    w.press("down")  # Ping host
    w.press("ok")
    w.wait_keyboard()
    w.press("ok")  # prefilled 8.8.8.8, the cursor on Save
    w.wait(2.3)  # LAN, then WiFi
    w.shot("51-ping")
    w.press("back")
    w.press("back")
    w.wait(0.3)
    w.shot("52-home-lan-wifi-icons")
    w.press("up", 3)  # Quick Connect, as on launch (the README hero's first step)
    w.shot("52b-home-quick-connect")
    w.exit_app()


def w_keys(w: Walker) -> None:
    """SSH keys on the mock (same steps as w_keys in test_render.c): generate with and without a
    passphrase, card, actions, the passphrase asked before the connection with Remember, a
    host on a named key, deleting that key (the host switches to the default key), and a key
    file removed from the SD card outside the app."""
    w.launch()
    w.press("down", 4)  # Keys
    w.press("ok")
    w.wait(0.3)
    w.shot("k01-keys-empty")
    w.press("ok")  # Generate key
    w.shot("k02-key-type")
    w.press("ok")  # Ed25519
    w.shot("k03-key-name")
    w.type("")  # the suggested id_ed25519
    w.shot("k05-key-passphrase")  # one prompt, empty for none (as ssh-keygen)
    w.type("secret")
    w.shot("k06-key-passphrase-again")
    w.type("secret")
    w.wait(0.3)
    w.shot("k07-key-generating")
    w.wait(3.0)
    w.shot("k08-key-card")
    w.press("right")  # Actions
    w.shot("k09-key-actions")
    w.press("ok")  # Show public key
    w.wait(0.3)
    w.shot("k10-public-key")
    w.press("back", 3)  # actions, card, list
    w.shot("k11-keys-list")
    w.press("up")  # Generate key
    w.press("ok")
    w.press("down")  # ECDSA P-256
    w.press("ok")
    w.type("")  # the suggested name
    w.type("")  # empty: no passphrase
    w.wait(1.5)
    w.shot("k12-key-card-ecdsa")
    w.press("back", 2)  # list, Home (on Keys)
    w.press("up", 3)  # Hosts
    w.press("ok")
    w.wait(0.6)
    w.press("ok")  # the host page, on Connect
    w.wait(0.5)
    w.press("down", 5)  # Auth: Password, Default, id_ecdsa, id_ed25519
    w.press("right")
    w.shot("k13-host-auth-default")
    w.press("down", 5)  # Save (no Save password row for key hosts)
    w.press("ok")
    w.wait(0.4)
    w.press("ok")  # the Hosts list
    w.press("back")  # Home, on Hosts
    w.press("down", 3)  # Keys: the cursor is on id_ecdsa, the newest
    w.press("ok")
    w.press("down")  # id_ed25519
    w.press("ok")
    w.press("right")  # Actions: Show, Forget passphrase, Rename, Delete
    w.press("down")
    w.press("ok")
    w.wait(0.3)
    w.shot("k14-passphrase-forgotten")
    w.press("ok")
    w.press("back", 3)  # card, list, Home
    w.press("up", 3)  # Hosts -> nas -> Connect
    w.press("ok")
    w.wait(0.6)
    w.press("ok")  # nas: the host page, on Connect
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(1.2)
    w.shot("k15-passphrase-prompt")
    w.type("wrong")
    w.shot("k16-remember-first-notice")
    w.press("right")  # Remember
    w.wait(0.8)
    w.shot("k17-passphrase-retry")
    w.type("secret")
    w.shot("k18-remember-passphrase")
    w.press("right")
    w.wait(0.4)
    w.shot("k19-unlocking-key")
    w.wait(3.0)
    w.press("right")  # trust the new host key
    w.wait(1.5)
    w.shot("k20-run-menu-key")
    w.press("back")  # disconnect: back on nas's page, on Connect
    w.wait(0.5)
    w.press("down", 5)  # Auth
    w.press("right")  # id_ecdsa (Password, Default, id_ecdsa, id_ed25519)
    w.shot("k21-host-auth-key")
    w.press("down", 5)  # Save
    w.press("ok")
    w.wait(0.4)
    w.press("ok")  # the Hosts list
    w.press("back")  # Home, on Hosts
    w.press("down", 3)  # Keys: the cursor is on id_ed25519
    w.press("ok")
    w.press("up")  # id_ecdsa
    w.press("ok")
    w.press("right")  # Actions: Show, Set as default, Rename, Delete
    w.press("up")  # Delete
    w.press("ok")
    w.wait(0.3)
    w.shot("k22-confirm-delete-key")
    w.press("right")  # Delete
    w.wait(0.3)
    w.shot("k23-key-deleted")
    w.press("ok")  # the Keys list
    w.press("back")  # Home, on Keys
    w.press("up", 3)  # Hosts -> nas
    w.press("ok")
    w.wait(0.6)
    w.press("ok")  # the host page, on Connect
    w.wait(0.5)
    w.press("down", 5)  # Auth
    w.shot("k24-host-auth-switched")
    w.press("back")  # the Hosts list
    w.r.delete("/ext/apps_data/ssh_runner/keys/id_ed25519")
    w.r.delete("/ext/apps_data/ssh_runner/keys/id_ed25519.pub")
    w.press("ok")  # nas
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(0.6)
    w.shot("k25-connect-key-missing")
    w.press("right")  # Edit host
    w.wait(0.4)
    w.shot("k26-edit-host-from-missing")
    w.exit_app()


def w_failure(w: Walker) -> None:
    """Unreachable host: failure dialog; keyboard symbol page."""
    w.launch()
    w.wait(0.6)
    w.press("ok")
    w.shot("56-quick-connect-prefilled")
    w.type("user@10.0.0.99")
    w.wait(1.6)  # typing already takes ~1 s: the mock gives up 3.0 s after the connect
    w.shot("57-connecting-unreachable")  # "1-2 s" (09 shows "0 s")
    w.wait(2.0)
    w.shot("58-connect-failed")
    w.press("left")  # Back
    w.wait(0.3)
    w.press("ok")  # Quick Connect again: Up twice from 'q' = stock text-cursor mode
    w.press("up", 2)
    w.press("left", 3)
    w.shot("59-keyboard-cursor-edit")
    w.exit_app()


def w_link(w: Walker) -> None:
    """Needs the ESP32 wired to pins 13/14/GND: real UART backend (mock off)."""
    w.launch()
    w.wait(4.0)  # HELLO + baud negotiation
    w.shot("m3-01-home-board")
    w.press("down", 5)  # Settings
    w.press("ok")
    w.press("ok")  # About
    w.wait(0.5)
    w.shot("m3-02-about")
    w.press("down", 8)
    w.wait(0.3)
    w.shot("m3-03-about-board")
    w.press("back")
    w.press("back")
    w.press("up", 2)  # Network
    w.press("ok")
    w.wait(1.0)
    w.shot("m3-04-network")
    w.press("back")
    w.wait(0.5)
    w.shot("m3-05-home-after")
    w.exit_app()


def w_states(w: Walker) -> None:
    """Empty states (no data yet) and the keyboard's validation error."""
    w.launch()
    w.wait(0.6)
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(0.3)
    w.shot("60-hosts-empty")
    w.press("back")
    w.press("down")  # Recent (Home keeps its selection)
    w.press("ok")
    w.wait(0.3)
    w.shot("61-recent-empty")
    w.press("back")
    w.press("up", 2)  # Quick Connect
    w.press("ok")
    w.type("nohost")  # no user@: refused with a hint
    w.wait(0.3)
    w.shot("62-keyboard-error")
    w.press("left")  # first key only dismisses the bubble
    w.exit_app()


def w_dialogs(w: Walker) -> None:
    """Confirmation dialogs (safe default) and a report screen."""
    w.launch()
    w.wait(0.6)
    w.press("down")  # Hosts
    w.press("ok")
    w.press("ok")  # nas: the host page
    w.press("up")  # round to Delete, the last row
    w.press("ok")
    w.wait(0.3)
    w.shot("63-confirm-delete-host")
    w.press("back")  # Back = Cancel: nothing deleted
    w.press("back")
    w.press("back")
    w.press("down", 4)  # Settings
    w.press("ok")
    w.press("down")  # Developer
    w.press("ok")
    w.press("down", 3)  # Link test (mock: a perfect link)
    w.press("ok")
    w.wait(1.5)
    w.shot("64-report-linktest")
    w.press("ok")  # close
    w.exit_app()


def w_board(w: Walker) -> None:
    """Board states on the mock (Developer > Mock board): no network, board gone, relinked."""
    w.launch()
    w.wait(0.6)
    linked = w.r.screenshot()[:128]
    _open_developer(w)
    w.press("down", 4)  # Mock board
    w.press("ok")  # the chooser, on the current state (Linked)
    w.press("down")  # No network
    w.shot("65-developer-mock-board")
    w.press("ok")  # back in Developer
    w.wait(0.6)
    w.press("back", 2)
    w.wait(0.5)
    w.shot("66-home-no-network")
    no_net = w.r.screenshot()[:128]
    w.press("ok")  # Settings (Developer remembered)
    w.press("ok")  # Developer; the list starts at the top again
    w.press("down", 4)
    w.press("ok")  # chooser on No network
    w.press("down")  # No board
    w.press("ok")
    w.wait(0.6)
    w.press("back", 2)
    w.wait(0.5)
    w.shot("67-home-board-gone")
    gone = w.r.screenshot()[:128]
    w.press("ok")  # Settings
    w.press("up")  # About
    w.press("ok")
    w.wait(0.3)
    w.press("down", 3)
    w.shot("68-about-board-gone")
    w.press("back")
    w.press("down")  # Developer
    w.press("ok")
    w.press("down", 4)
    w.press("ok")  # chooser on No board
    w.press("up", 2)  # Linked
    w.press("ok")
    w.wait(0.6)
    w.press("back", 2)
    w.wait(0.5)
    w.shot("69-home-relinked")
    back = w.r.screenshot()[:128]
    if len({linked, no_net, gone}) != 3:
        raise WalkFailed("home header does not differ between linked, no network and gone")
    if back != linked:
        raise WalkFailed("home header after relink differs from the linked state")
    w.exit_app()


def w_security(w: Walker) -> None:
    """Settings > Security on the mock (same steps as w_security in test_render.c): the
    status, the warning, the typed word (OK does nothing until it is "confirm"), progress
    through the ESP32's restart, the result. The demo build burns nothing: no board."""
    marker = "/ext/apps_data/ssh_runner/.demo_sec_fail"  # the demo build's mock fails the first try
    if not w.r.stat("/ext/apps_data/ssh_runner"):
        w.r.mkdir("/ext/apps_data/ssh_runner")
    w.r.write(marker, b"")
    w.launch()
    w.wait(0.6)
    w.press("down", 5)  # Settings
    w.press("ok")
    w.press("down", 2)  # Security (after Developer: the walks run with Debug on)
    w.press("ok")
    w.wait(0.5)
    w.shot("s01-security-plain")
    w.press("right")  # Protect
    w.shot("s02-protect-confirm")
    w.press("down", 10)  # to the end of the details
    w.shot("s02b-protect-confirm-details")
    w.press("right")  # Protect
    w.shot("s03-type-confirm")
    w.type("conf")
    w.shot("s04-word-incomplete")  # still the keyboard: no popup
    w.type("irm", fresh=False)
    w.wait(2.0)
    w.shot("s10-protect-failed")  # the failure after the burn: nothing moved
    w.r.delete(marker)
    w.press("ok")  # back on Security, still plaintext
    w.wait(0.5)
    w.press("right")
    w.press("right")
    w.type("confirm")
    w.wait(0.3)
    w.shot("s05-protect-writing-key")
    w.wait(1.3)
    w.shot("s06-protect-encrypting")
    w.wait(1.5)
    w.shot("s07-protect-restarting")
    w.wait(2.5)
    w.shot("s08-secrets-protected")
    w.press("ok")
    w.wait(0.5)
    w.shot("s09-security-protected")
    w.exit_app()


SETTINGS = f"{APP_DATA_DIR}/settings.conf"
COMMANDS = f"{APP_DATA_DIR}/commands.conf"
SETTINGS_SEEDED = b"Filetype: SSH Runner Settings\nVersion: 1\nExamples: seeded\n"


def preseed(r: FlipperRpc) -> None:
    """A fresh data folder starts as one that has had its examples already, so the walks'
    menu positions stay as they were; the examples walk takes the first start itself."""
    if not r.stat(SETTINGS):
        if not r.stat(APP_DATA_DIR):
            r.mkdir(APP_DATA_DIR)
        r.write(SETTINGS, SETTINGS_SEEDED)


def _settings_without_seeded(w: Walker) -> None:
    """settings.conf as it is, minus the line that says the examples were offered."""
    old = w.r.read(SETTINGS) if w.r.stat(SETTINGS) else b"Filetype: SSH Runner Settings\nVersion: 1\n"
    lines = [l for l in old.splitlines(keepends=True) if not l.startswith((b"Examples:", b"Mock_backend:"))]
    w.r.write(SETTINGS, b"".join(lines) + b"Mock_backend: yes\n")  # the demo build's mock


def _run_menu_by_quick_connect(w: Walker) -> None:
    """The mock's Quick Connect to its demo host, up to the run menu."""
    w.press("ok")  # Quick Connect
    w.type("user@192.168.1.10")
    if w.wait_prompt_or_keyboard():
        w.press("right")  # Trust
    w.wait_keyboard()
    w.type("hunter2")
    w.wait(1.5)


def w_examples(w: Walker) -> None:
    """The built-in examples (as test_render.c w_examples_*), the field limit with its
    counter, and what the app does with commands it cannot run: never cut, never dropped."""
    for p in (COMMANDS, f"{SCRIPTS}/sysinfo.sh"):
        if w.r.stat(p):
            w.r.delete(p)
    _settings_without_seeded(w)
    w.launch()  # first start of this build: the examples where nothing is
    w.wait(0.6)
    if not w.r.stat(COMMANDS) or not w.r.stat(f"{SCRIPTS}/sysinfo.sh"):
        raise WalkFailed("the examples were not copied on the first start")
    if b"Examples: seeded" not in w.r.read(SETTINGS):
        raise WalkFailed("the first start was not recorded in settings.conf")
    w.press("down", 5)  # Settings
    w.press("ok")
    w.press("down", 3)  # Restore Examples (after Developer and Security: Debug is on)
    w.shot("x01-settings-restore-examples")
    w.press("ok")
    w.wait(0.5)
    w.shot("x03-examples-all-there")
    w.press("ok")
    w.exit_app()

    # the field limit: Quick Connect holds user@host:port, 102 characters (a fresh start:
    # the home menu's cursor is on Quick Connect)
    w.launch()
    w.wait(0.6)
    w.press("ok")
    w.type("a" * 83, confirm=False)
    w.shot("59c-keyboard-counter")
    w.type("b" * 25, confirm=False, fresh=False)
    w.shot("59d-keyboard-full")
    w.exit_app()

    # a commands.conf line and a host the app cannot run as written: listed, not dropped
    w.r.write(COMMANDS, w.r.read(COMMANDS) + b"Cmd: echo " + b"x" * 130 + b"\n")
    w.r.write(f"{APP_DATA_DIR}/hosts/old.conf",
              b"Filetype: SSH Runner Host\nVersion: 1\nAlias: old\nUser: pi\nHost: 192.168.1.20\n"
              b"Cmd: echo " + b"y" * 130 + b"\n")
    w.r.delete(f"{SCRIPTS}/sysinfo.sh")  # a deleted example stays deleted ...
    w.launch()
    w.wait(0.6)
    if w.r.stat(f"{SCRIPTS}/sysinfo.sh"):
        raise WalkFailed("a deleted example came back by itself")
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(0.8)
    w.shot("x04-hosts-unreadable")  # run on its own (--walk examples): old is the only host
    w.press("ok")
    w.wait(0.5)
    w.shot("x05-host-unreadable-why")
    w.press("ok")
    w.exit_app()

    w.launch()
    w.wait(0.6)
    _run_menu_by_quick_connect(w)
    w.shot("x06-run-menu-examples")
    w.press("down", 9)  # past the nine examples: "! 1 command not shown"
    w.shot("x07-run-menu-not-shown")
    w.press("ok")
    w.wait(0.4)
    w.shot("x08-not-shown-why")
    w.press("ok")
    w.exit_app()

    w.launch()  # ... until Settings > Restore Examples
    w.wait(0.6)
    w.press("down", 5)
    w.press("ok")
    w.press("down", 3)
    w.press("ok")
    w.wait(0.5)
    w.shot("x02-examples-restored")
    if not w.r.stat(f"{SCRIPTS}/sysinfo.sh"):
        raise WalkFailed("Restore Examples did not bring sysinfo.sh back")
    w.press("ok")
    w.exit_app()
    w.r.delete(f"{APP_DATA_DIR}/hosts/old.conf")


def w_low_memory(w: Walker) -> None:
    """The startup check's Low memory dialog, from the demo build: a marker file that only
    the demo build reads forces it (no real-backend screen ever goes to docs/)."""
    marker = "/ext/apps_data/ssh_runner/.demo_low_memory"
    if not w.r.stat("/ext/apps_data/ssh_runner"):
        w.r.mkdir("/ext/apps_data/ssh_runner")
    w.r.write(marker, b"")
    w.r.app_start(FAP)
    w.in_app = True
    w.wait(2.5)
    w.shot("70-low-memory")
    w.press("ok")
    w.wait(0.8)
    w.r.delete(marker)
    w.in_app = w.r.app_locked()
    w.exit_app()


WALKS = {
    "tour": w_tour,
    "states": w_states,
    "quick_connect": w_quick_connect,
    "saved_host": w_saved_host,
    "dialogs": w_dialogs,
    "fleet": w_fleet,
    "network": w_network,
    "keys": w_keys,
    "failure": w_failure,
    "board": w_board,
    "security": w_security,
    "low_memory": w_low_memory,
    "examples": w_examples,
}


def _open_developer(w: Walker) -> None:
    w.press("down", 5)  # Settings
    w.press("ok")
    w.press("down")  # Developer
    w.press("ok")
    w.wait(0.5)


def w_linktest(w: Walker) -> None:
    """1 MiB each way through ECHO, every byte compared (Developer > Link test)."""
    w.launch()
    w.wait(4.0)
    _open_developer(w)
    w.press("down", 3)  # Link test 1 MiB
    w.shot("m3-10-developer")
    w.press("ok")
    w.wait(8.0)  # progress is reported every 128 frames: catch it mid-run, not at 0%
    w.shot("m3-11-linktest-running")
    # Only the % value of the bottom row changes while the test runs; the result screen
    # replaces the list when it ends (~1 min), which changes the top 16 px (pages 0-1).
    top = w.r.screenshot()[:256]
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        time.sleep(5.0)
        if w.r.desktop_locked():
            w.unlock_if_locked()
        if w.r.screenshot()[:256] != top:
            w.wait(0.5)
            w.shot("m3-12-linktest-result")
            w.press("down", 4)  # retransmits, rate and baud are below the fold
            w.shot("m3-13-linktest-result-more")
            break
    else:
        raise WalkFailed("link test did not finish within 4 minutes")
    w.exit_app()


def _esp32_console(port: str):
    """Open the ESP32's USB console without resetting it (DTR and RTS both low)."""
    import serial

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def _esp32_cmd(s, cmd: str, wait: float = 0.6) -> str:
    s.reset_input_buffer()
    s.write((cmd + "\r\n").encode())
    time.sleep(wait)
    return s.read(4096).decode("utf-8", "replace")


def contains_secret(data: bytes, secret: str) -> bool:
    """The secret as a whole word in a file (not inside a longer one): a short test password
    such as 'test' is part of the user name 'tester', which is no leak."""
    import re

    pat = rb"(?<![A-Za-z0-9])" + re.escape(secret.encode()) + rb"(?![A-Za-z0-9])"
    return re.search(pat, data) is not None


def without_busy_sign(frame: bytes) -> bytes:
    """A frame with the output footer's busy sign (6-dot spinner, x 2..6, rows 53..60) blanked:
    it turns while a command runs, so 'the screen changed' means the run ended only without it."""
    f = bytearray(frame)
    for page in (6, 7):
        for x in range(0, 9):
            f[page * 128 + x] = 0
    return bytes(f)


def _esp32_forget(w: Walker, profiles: list[str], legacy_key: bool = False) -> None:
    """Start from a known board: forget the trusted keys and remembered password of these
    profiles (console `forget`), and with legacy_key give it a v0 key for the app to migrate
    (console `legacy create`), before the app starts. The console may reset the board on
    open (DTR on some hosts): wait out a boot first."""
    import os

    s = _esp32_console(os.environ.get("FES_ESP32_PORT", "COM8"))
    try:
        time.sleep(5.0)
        if legacy_key:
            out = _esp32_cmd(s, "legacy create", 3.0)
            if "ESP_OK" not in out:
                raise WalkFailed(f"legacy create failed: {out.strip().splitlines()[-1:]}")
        for p in profiles:
            _esp32_cmd(s, f"forget {p}", 1.0)
            if "no trusted keys" not in _esp32_cmd(s, f"hostkeys {p}", 1.0):
                raise WalkFailed(f"the board still trusts a key for {p}")
    finally:
        s.close()
    w.log(f"  board: forgot {len(profiles)} profiles")


def _watch_header(w: Walker, linked: bytes, timeout: float, log_prefix: str):
    """Sample the status header every 0.25 s; return (seconds to 'gone', seconds to relinked)."""
    t0 = time.monotonic()
    gone_at = back_at = None
    prev = linked
    while time.monotonic() - t0 < timeout:
        top = w.r.screenshot()[:128]
        if top != prev:
            state = "linked" if top == linked else "board gone"
            w.log(f"  {log_prefix} +{time.monotonic() - t0:.2f} s: {state}")
            if state == "board gone" and gone_at is None:
                gone_at = time.monotonic() - t0
                w.shot("m3-21-home-board-gone")
            if state == "linked" and gone_at is not None:
                back_at = time.monotonic() - t0
                break
            prev = top
        time.sleep(0.25)
    return gone_at, back_at


def w_replug(w: Walker) -> None:
    """The board goes silent (wires pulled) and comes back; then a real ESP32 reboot;
    then an app restart while the board is still at the negotiated rate."""
    import os
    import re

    port = os.environ.get("FES_ESP32_PORT", "COM8")
    w.launch()
    w.wait(4.0)
    w.shot("m3-20-home-linked")
    linked = w.r.screenshot()[:128]  # top page: the status header

    # A: the ESP32 ignores the UART for 6 s, exactly what the Flipper sees when the wires
    # come off; it must show "No ESP32 board" and reconnect by itself afterwards.
    s = _esp32_console(port)
    w.log("  " + _esp32_cmd(s, "link mute 6").strip().splitlines()[-2:][0])
    gone, back = _watch_header(w, linked, 15.0, "mute:")
    if gone is None:
        raise WalkFailed("the Flipper never noticed the silent board")
    if back is None:
        raise WalkFailed("the link did not come back within 15 s")
    w.shot("m3-22-home-relinked")
    w.log(f"  mute: board gone after {gone:.1f} s, relinked {back - 6.0:.1f} s after the board answered again")

    # B: a real reboot (100 ms RTS pulse). Recovery is faster than the screen sampling, so
    # the proof is the ESP32's own counters: a fresh boot, then the Flipper's HELLO.
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    s.close()
    time.sleep(6.0)
    s = _esp32_console(port)
    out = _esp32_cmd(s, "link")
    s.close()
    m = re.search(r"link: peer (\w+), baud (\d+), rx (\d+) B, (\d+) frames", out)
    w.log(f"  reboot: ESP32 says {m.group(0) if m else out.strip()[-120:]}")
    if not m or m.group(1) != "yes" or int(m.group(4)) > 30:
        raise WalkFailed("after the ESP32 reboot the Flipper did not re-handshake")
    if w.r.screenshot()[:128] != linked:
        raise WalkFailed("the Flipper does not show the board after the ESP32 reboot")

    # C: app restart; the board is still at the negotiated rate and must accept the new
    # app's HELLO at 115200 once it has noticed the old app is gone (3 s).
    w.exit_app()
    w.launch()
    w.wait(8.0)  # the board must be linked again (it waits 2 s after the last app)
    w.shot("m3-23-app-restart-linked")
    if w.r.screenshot()[:128] != linked:
        raise WalkFailed("the board was not found after an app restart")
    w.exit_app()


M4_SCRIPT = b"""#!/bin/sh
echo "hello from $(hostname)"
uname -m
echo "exit code should be 3"
exit 3
"""


def _local_env() -> dict:
    from pathlib import Path

    p = Path(__file__).resolve().parents[1] / "hil" / "target" / "local.env"
    env = {}
    for line in p.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def _remote_has(container: str, needle: str) -> bool:
    """Is a process whose command line contains `needle` running in the test sshd container
    (SSH_TARGET_CONTAINER, Docker on this PC)? Read-only: `docker exec ... ps`."""
    import subprocess

    out = subprocess.run(["docker", "exec", container, "ps", "-eo", "args"], capture_output=True, text=True).stdout
    return any(needle in line for line in out.splitlines())


def w_lancfg(w: Walker) -> None:
    """Read-only: Network > LAN as the board has it (mode, address, mask, gateway, DNS), to
    record a static setup before a test changes it and to check it after. Nothing is edited;
    the screens show the real network: .cache only."""
    w.launch()
    for i in range(8):  # HELLO, baud switch, network status
        w.wait(1.0)
        w.shot(f"lan-start-{i}", same_ok=True)
    w.press("down", 3)  # Network
    w.press("ok")
    w.wait(0.5)
    w.press("ok")  # LAN
    w.wait(0.5)
    w.shot("lan-1")
    for i in range(2, 7):
        w.press("down")
        w.shot(f"lan-{i}", same_ok=True)
    w.exit_app()


def w_cmdlen(w: Walker) -> None:
    """Real backend: a 127-character command reaches the sshd whole (it writes its own length
    there), and Back stops a streaming ping on the host within a second."""
    import subprocess

    env = _local_env()
    container = env["SSH_TARGET_CONTAINER"]
    target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"
    _esp32_forget(w, [f"{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"])
    w.launch()
    w.wait(8.0)  # HELLO, baud switch, network status
    w.press("ok")  # Quick Connect
    w.type(target)
    if w.wait_prompt_or_keyboard():
        w.press("right")  # Trust
    w.wait_keyboard()
    w.type(env["SSH_TARGET_PASSWORD"])
    w.wait(3.0)
    w.shot("c01-run-menu")
    w.press("down", 2)  # Type command (after the two default global commands)
    w.press("ok")
    w.wait_keyboard()
    cmd = "echo " + "a" * 109 + "|wc -c>/tmp/n"  # 127 characters: 109 a's and a newline = 110
    assert len(cmd) == 127
    w.type(cmd, confirm=False)
    w.shot("c02-command-127")
    w.type("", confirm=True, fresh=False)
    w.wait(3.0)
    w.shot("c03-command-127-done")
    got = subprocess.run(["docker", "exec", container, "cat", "/tmp/n"], capture_output=True, text=True).stdout.strip()
    w.log(f"  127-character command on the host: wc -c = {got} (110 = all of it)")
    if got != "110":
        raise WalkFailed(f"the 127-character command arrived cut: wc -c {got!r}, want 110")
    w.press("back")  # a Quick Connect target: "Save as alias?"
    w.wait(0.5)
    w.press("left")  # No: back to the run menu
    w.wait(0.5)
    w.press("ok")  # Type command again (the menu keeps its place)
    w.wait_keyboard()
    w.type("ping 127.0.0.1")
    w.wait(3.0)
    w.shot("c04-ping-streaming")
    if not _remote_has(container, "ping 127.0.0.1"):
        raise WalkFailed("the ping is not running on the host")
    w.press("back")  # Stop
    t0 = time.monotonic()
    while _remote_has(container, "ping 127.0.0.1"):
        if time.monotonic() - t0 > 3.0:
            raise WalkFailed("Back did not stop the remote ping")
        time.sleep(0.1)
    w.log(f"  Back stopped the remote ping in {time.monotonic() - t0:.2f} s")
    w.shot("c05-ping-stopped")
    w.press("back")
    w.press("back")  # disconnect
    w.wait(1.0)
    w.exit_app()


def w_m4(w: Walker) -> None:
    """The core user story on the real backend (board wired, network up, test sshd from
    local.env): Quick Connect, host key, password, one-liner, save as alias, the profile's
    transport (SSH_TARGET_TRANSPORT: lan or wifi, default wifi), saved command in 4 clicks,
    SD script with its exit code. Screens show real addresses: kept out of git."""
    env = _local_env()
    target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"
    pw = env["SSH_TARGET_PASSWORD"]
    # a known board: the walk's host-key prompts appear only for keys it has not trusted yet
    _esp32_forget(w, [f"{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}", "lab"])
    base = SCRIPTS.rsplit("/", 1)[0]
    if not w.r.stat(base):
        w.r.mkdir(base)
    if not w.r.stat(SCRIPTS):
        w.r.mkdir(SCRIPTS)
    w.r.write(f"{SCRIPTS}/hello.sh", M4_SCRIPT)
    w.launch()
    w.wait(8.0)  # HELLO, baud switch, network status
    w.shot("m4-01-home")
    w.press("ok")  # Quick Connect
    w.type(target)
    w.wait(0.3)
    w.shot("m4-02-connecting")
    if w.wait_prompt_or_keyboard():  # a board that trusts the bench already asks nothing
        w.shot("m4-03-hostkey-new")
        w.press("right")  # Trust
    else:
        w.log("  host key already trusted on the board: no prompt")
    w.wait_keyboard()
    w.shot("m4-04-password")
    w.type(pw)
    w.wait(3.0)
    w.shot("m4-05-run-menu")
    w.press("down", 2)  # Type command
    w.press("ok")
    w.type("uname -a")
    w.wait(3.0)
    w.shot("m4-06-output-uname")
    w.press("back")  # offers Save as alias
    w.wait(0.5)
    w.press("right")  # Save
    w.type("lab", clear_first=True)
    w.wait(2.5)
    w.shot("m4-07-saved")
    w.press("ok")
    w.exit_app()

    # Edit the host: WiFi transport and a saved command
    w.launch()
    w.wait(8.0)  # the board must be linked again (it waits 2 s after the last app)
    w.shot("m4-07b-home-phase2")
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)  # the list must be up before OK
    w.press("ok")  # lab: the host page, on Connect
    w.wait(0.5)
    w.shot("m4-08a-host-page")
    w.press("down")  # Alias
    w.press("down", 6)  # Transport
    transport = env.get("SSH_TARGET_TRANSPORT", "wifi").lower()
    w.press("right", 1 if transport == "lan" else 2)  # Auto > LAN > WiFi
    w.wait(0.3)
    w.shot(f"m4-08-transport-{transport}")
    w.press("down", 2)  # Commands
    w.press("ok")
    w.press("ok")  # + Add command
    w.type("uptime")
    w.press("back")
    w.press("down", 2)  # Save
    w.press("ok")
    w.wait(2.5)
    w.exit_app()

    # Saved command in 4 clicks: Hosts, lab, Connect, $ uptime
    w.launch()
    w.wait(8.0)  # the board must be linked again (it waits 2 s after the last app)
    w.shot("m4-08b-home-phase3")
    w.press("down")
    w.press("ok")  # 1: Hosts
    w.wait(1.0)
    w.press("ok")  # 2: lab
    w.wait(0.5)
    w.press("ok")  # 3: Connect
    if w.wait_prompt_or_keyboard():
        w.shot("m4-09-hostkey-alias")  # keys are pinned per profile: asked once for the alias
        w.press("right")  # Trust
    else:
        w.log("  alias lab already trusted on the board: no prompt")
    w.wait_keyboard()
    w.type(pw)
    w.wait(3.0)
    w.shot("m4-10-run-menu-alias")
    w.press("ok")  # 4: $ uptime
    w.wait(3.0)
    w.shot("m4-11-output-uptime")
    w.press("back")
    w.press("down", 3)  # $ uptime, $ df -h, Type command, Scripts
    w.press("ok")
    w.wait(0.5)
    w.press("ok")  # hello.sh
    w.wait(0.5)
    w.shot("m4-12-script-confirm")
    w.press("right")  # Run
    w.wait(4.0)
    w.shot("m4-13-output-script")
    w.press("back")
    # Back stops a running command: the remote process must be gone, the session kept.
    w.press("up")  # Type command
    w.press("ok")
    w.type("sleep 30")
    w.wait(2.5)
    w.shot("m4-14-output-running")
    container = env.get("SSH_TARGET_CONTAINER", "")
    running = _remote_has(container, "sleep 30") if container else None
    w.press("back")  # stop
    w.wait(2.0)
    w.shot("m4-15-after-stop")
    if container:
        gone = not _remote_has(container, "sleep 30")
        w.log(f"  back to stop: remote sleep running before {running}, gone after {gone}")
        if not running or not gone:
            raise WalkFailed("Back did not stop the remote command")
    w.press("back")
    w.press("back")  # disconnect
    w.wait(1.0)
    w.exit_app()


def _authorize(env: dict, pubs: str) -> None:
    """Append public keys to the test sshd's authorized_keys (a container on this machine)."""
    import subprocess

    path = env.get("SSH_TARGET_AUTHORIZED_KEYS", "/config/.ssh/authorized_keys")
    subprocess.run(["docker", "exec", "-i", env["SSH_TARGET_CONTAINER"], "sh", "-c", f"cat >> '{path}'"],
                   input=pubs, text=True, check=True)


def _walk_keys() -> dict:
    """Test keys for w_hwkeys, made fresh with ssh-keygen into .cache/hil/walk-keys
    (git-ignored) and authorized in the --rm test sshd: an encrypted OpenSSH Ed25519 key
    with its .pub, and an encrypted PEM RSA key without one (the ESP32 returns it)."""
    import secrets
    import shutil
    import subprocess

    env = _local_env()
    d = Path(__file__).resolve().parents[2] / ".cache" / "hil" / "walk-keys"
    d.mkdir(parents=True, exist_ok=True)
    pp = "walk-" + secrets.token_hex(4)
    exe = shutil.which("ssh-keygen")
    keys = {"hil_ed25519": ["-t", "ed25519"], "hil_rsa_pem": ["-t", "rsa", "-b", "3072", "-m", "PEM"]}
    for name, args in keys.items():
        for f in (d / name, d / (name + ".pub")):
            f.unlink(missing_ok=True)
        subprocess.run([exe, "-q", *args, "-N", pp, "-C", name, "-f", str(d / name)], check=True)
    pubs = "\n".join((d / (n + ".pub")).read_text().strip() for n in keys) + "\n"
    _authorize(env, pubs)
    return {"dir": d, "pp": pp, "names": list(keys)}


def w_hwkeys(w: Walker) -> None:
    """SSH keys on the real board and the test sshd (local.env). Copies two test keys into
    keys/ (the SD card is restored afterwards). The first launch with this firmware moves
    the board's v0 key to keys/flipper (migration) and makes it the default. Then: Quick
    Connect on the default key (no password), a host on an encrypted key (passphrase once,
    then remembered), a PEM key without .pub (the ESP32 returns it and the app writes it),
    and a key generated on the board. Screens show real addresses: kept out of git."""
    env = _local_env()
    target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"
    # a v0 key on the board: the app migrates it to keys/flipper, the default key (the SD
    # card's restore takes that away again after the walk, and the migration the board's copy)
    _esp32_forget(w, [f"{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}", "lab"], legacy_key=True)
    k = _walk_keys()
    keys_dir = "/ext/apps_data/ssh_runner/keys"
    if not w.r.stat("/ext/apps_data/ssh_runner"):
        w.r.mkdir("/ext/apps_data/ssh_runner")
    if not w.r.stat(keys_dir):
        w.r.mkdir(keys_dir)
    w.r.write(f"{keys_dir}/hil_ed25519", (k["dir"] / "hil_ed25519").read_bytes())
    w.r.write(f"{keys_dir}/hil_ed25519.pub", (k["dir"] / "hil_ed25519.pub").read_bytes())
    w.r.write(f"{keys_dir}/hil_rsa_pem", (k["dir"] / "hil_rsa_pem").read_bytes())
    w.launch()
    w.wait(8.0)  # HELLO, network status, and the migration of the v0 key
    migrated = bool(w.r.stat(f"{keys_dir}/flipper"))
    w.log(f"  v0 key migrated to keys/flipper: {migrated}")
    if not w.r.stat(f"{keys_dir}/flipper.pub"):
        raise WalkFailed("no default key: keys/flipper.pub missing after the migration")
    # the default key from now on: the test sshd must know it for Quick Connect
    pub = w.r.read(f"{keys_dir}/flipper.pub").decode().strip()
    _authorize(_local_env(), pub + "\n")
    w.press("down", 4)  # Keys
    w.press("ok")
    w.wait(0.6)
    w.shot("hk01-keys-list")
    w.press("down")  # flipper (Generate key, flipper, hil_ed25519, hil_rsa_pem)
    w.press("ok")
    w.wait(0.4)
    w.shot("hk02-key-card-migrated")
    w.press("back", 2)  # list, Home
    # Quick Connect uses the default key: no password at all
    w.press("up", 4)
    w.press("ok")
    w.type(target)
    w.wait(4.0)
    w.shot("hk03-hostkey-new")
    w.press("right")  # Trust
    w.wait(3.0)
    w.shot("hk04-run-menu-default-key")
    w.press("down", 2)  # Type command
    w.press("ok")
    w.type("whoami")
    w.wait(3.0)
    w.shot("hk05-output-whoami")
    w.press("back")  # offers Save as alias
    w.wait(0.5)
    w.press("right")  # Save
    w.type("lab", clear_first=True)
    w.wait(2.5)
    w.press("ok")
    w.press("back")  # disconnect: back on Home
    w.wait(1.0)
    # lab on the encrypted Ed25519 key: Password, Default, flipper, hil_ed25519, hil_rsa_pem
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # the host page, on Connect
    w.wait(0.5)
    w.press("down", 5)  # Auth
    w.press("right", 2)
    w.shot("hk06-host-auth-hil-ed25519")
    w.press("down", 5)  # Save
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # the Hosts list
    w.wait(1.0)
    w.press("ok")  # lab
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(2.0)
    w.shot("hk07-passphrase-prompt")
    w.type(k["pp"])
    w.wait(0.5)
    w.press("right")  # Remember
    w.wait(1.0)
    w.shot("hk08-unlocking-key")
    w.wait(14.0)  # OpenSSH's bcrypt KDF on the ESP32
    w.shot("hk09-hostkey-alias")
    w.press("right")  # keys are pinned per alias
    w.wait(3.0)
    w.shot("hk10-run-menu-encrypted-key")
    w.press("back")  # disconnect: back on lab's page, on Connect
    w.wait(1.0)
    w.press("ok")  # Connect again: the passphrase is remembered
    w.wait(16.0)
    w.shot("hk11-run-menu-remembered", same_ok=True)  # the same run menu: connected again
    w.press("back")  # disconnect: lab's page, on Connect
    w.wait(1.0)
    # lab on the PEM key without .pub
    w.press("down", 5)
    w.press("right")  # hil_rsa_pem
    w.press("down", 5)
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # the Hosts list
    w.wait(1.0)
    w.press("ok")  # lab
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(2.0)
    w.shot("hk12-passphrase-pem")
    w.type(k["pp"])
    w.wait(0.5)
    w.press("right")  # Remember
    w.wait(8.0)
    w.shot("hk13-run-menu-pem-key")
    wrote_pub = bool(w.r.stat(f"{keys_dir}/hil_rsa_pem.pub"))
    w.log(f"  keys/hil_rsa_pem.pub written from the ESP32's answer: {wrote_pub}")
    w.press("back")  # disconnect: lab's page
    w.wait(1.0)
    w.press("back")  # the Hosts list
    w.press("back")  # Home
    # a key generated on the board
    w.press("down", 3)  # Keys
    w.press("ok")
    w.wait(0.5)
    w.press("up")  # the cursor is on flipper (last opened): Generate key is above it
    w.press("ok")
    w.press("ok")  # Ed25519
    w.type("")  # suggested name
    w.press("left")  # Skip
    w.wait(4.0)
    w.shot("hk14-key-card-generated")
    w.exit_app()
    if not migrated:
        raise WalkFailed("the v0 key was not migrated to keys/flipper")
    if not wrote_pub:
        raise WalkFailed("keys/hil_rsa_pem.pub was not written")


def w_heap(w: Walker) -> None:
    """Startup heap check. About shows the free heap at app start; the CLI gives the free heap
    with the app idle. The RPC session itself holds ~10 KB, so the app is also started once
    from the CLI with no RPC session (the normal case) to read desktop and idle there."""
    r = w.r
    r.stop_session()
    r.cli_prompt()

    def cli(cmd: str, wait: float) -> str:
        r.t.write(cmd.encode() + bytes([13]))
        time.sleep(wait)
        out = r.t.read(4096, 0.3).decode("utf-8", "replace")
        return " | ".join(x.strip() for x in out.splitlines() if "heap" in x.lower())

    w.log("  no RPC, desktop: " + cli("free", 0.5))
    cli(f"loader open {FAP}", 4.0)
    w.log("  no RPC, app idle: " + cli("free", 0.5))
    r.start_session()
    w.in_app = r.app_locked()
    w.exit_app()
    w.launch()
    w.wait(1.0)
    w.heap("app idle (RPC session open)")
    _open_settings_about(w)
    w.press("down", 6)
    w.wait(0.3)
    w.shot("heap-about-memory")
    w.exit_app()


def _open_settings_about(w: Walker) -> None:
    w.press("down", 5)  # Settings
    w.press("ok")
    w.press("ok")  # About
    w.wait(0.5)


def w_m5(w: Walker) -> None:
    """On the real backend (test sshd from local.env): a 5 MB output (`yes | head -c 5M`)
    neither crashes nor wedges the app; a remembered password never reaches the SD card
    (every file under apps_data/ssh_runner is read and searched; the restore diff covers the
    rest of the card) and is used on the next connect. Real addresses: kept out of git."""
    env = _local_env()
    target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"
    pw = env["SSH_TARGET_PASSWORD"]
    # earlier runs leave the target and the alias trusted, and its password remembered
    _esp32_forget(w, [f"{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}", "m5"])
    w.launch()
    w.wait(8.0)  # HELLO, baud switch, network status
    w.press("ok")  # Quick Connect
    w.type(target)
    # the board keeps trusted host keys across runs and reflashes (its nvs): the prompt shows
    # only for a host it has not pinned yet
    if w.wait_prompt_or_keyboard():
        w.shot("m5-01-hostkey-new")
        w.press("right")  # Trust
    else:
        w.log("  host key already trusted on the board: no prompt")
    w.wait_keyboard()
    w.type(pw)
    w.settle()
    w.press("down", 2)  # Type command
    w.press("ok")
    flood = env.get("M5_FLOOD", "5M")  # smaller while iterating on the walk itself
    w.type(f"yes | head -c {flood}")
    w.wait(3.0)
    w.shot("m5-02-flood-running")
    w.heap("5 MB output running")
    # Every line is "y", so only the footer changes when it ends ("Running" -> "Exit 0"):
    # poll the screen without the turning busy sign; the time gives the end-to-end rate
    # through the flow control.
    running, t0 = without_busy_sign(w.r.screenshot()), time.monotonic()
    while time.monotonic() - t0 < 900 and without_busy_sign(w.r.screenshot()) == running:
        w.wait(10.0)
        w._guard()
    secs = time.monotonic() - t0 + 3.0
    w.log(f"  {flood} output ended after about {secs:.0f} s")
    w.shot("m5-03-flood-done")
    w.heap("5 MB output done")
    w.press("back")  # Save as alias? (a good first run)
    w.wait(1.0)
    w.shot("m5-03b-after-back")
    w.press("right")  # Save
    w.type("m5", clear_first=True)
    w.wait(2.5)
    w.press("ok")  # back in the run menu, still on Type command (the menu keeps its item)
    w.press("ok")  # the app still answers after 5 MB
    w.type("uname -a")
    w.wait(3.0)
    w.shot("m5-04-after-flood")
    w.press("back")
    w.press("back")  # disconnect
    w.wait(1.0)
    w.exit_app()

    # Remember the password: Hosts > m5 > Save password: Yes > Save, then connect once
    w.launch()
    w.wait(8.0)
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # the host page, on Connect
    w.wait(0.5)
    w.press("down", 6)  # Save password
    w.press("right")  # Yes
    w.wait(0.3)
    w.shot("m5-05-save-password-yes")
    w.press("down", 5)  # Save
    w.press("ok")
    w.wait(2.5)
    w.press("ok")  # close "Saved"
    w.wait(1.0)
    w.press("ok")  # Hosts: m5
    w.wait(0.5)
    w.press("ok")  # Connect
    if w.wait_prompt_or_keyboard():  # keys are pinned per alias, also across runs
        w.shot("m5-06-hostkey-alias")
        w.press("right")  # Trust
    else:
        w.log("  alias m5 already trusted on the board: no prompt")
    w.wait_keyboard()
    w.type(pw)  # sent once with the remember flag; kept only in the ESP32's NVS
    w.settle()
    w.shot("m5-07-run-menu")
    w.press("back")  # disconnect
    w.wait(1.0)
    w.exit_app()

    found, files = [], 0
    for e in w.r.walk(APP_DATA_DIR):
        if e.is_dir:
            continue
        files += 1
        if contains_secret(w.r.read(e.path), pw):
            found.append(e.path)
    w.log(f"  password search: {files} files under {APP_DATA_DIR}, found in {found or 'none'}")
    if found:
        raise WalkFailed(f"remembered password found on the SD card: {found}")

    # Next connect: no password prompt
    w.launch()
    w.wait(8.0)
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # m5
    w.wait(0.5)
    w.press("ok")  # Connect, with the remembered password
    w.wait(5.0)
    if w.keyboard_up(w.settle()):
        raise WalkFailed("the remembered password was not used: password keyboard shown")
    w.shot("m5-08-connected-without-prompt", same_ok=True)  # the run menu again, as in m5-07
    w.press("back")
    w.wait(1.0)
    w.exit_app()


def w_soak(w: Walker) -> None:
    """Soak test on the real backend (test sshd from local.env): a saved host with a remembered
    password, then connect / `$ uptime` / disconnect in a loop for SOAK_MINUTES (default 60),
    the Flipper's free heap read every 10 cycles. Track the ESP32 heap on its console at the
    same time (opened before this walk: opening it resets the board)."""
    env = _local_env()
    target = f"{env['SSH_TARGET_USER']}@{env['SSH_TARGET_HOST']}:{env['SSH_TARGET_PORT']}"
    pw = env["SSH_TARGET_PASSWORD"]
    minutes = float(env.get("SOAK_MINUTES", "60"))
    # setup: Quick Connect, trust, password, save as "soak", Save password: Yes
    w.launch()
    w.wait(8.0)
    w.press("ok")  # Quick Connect
    w.type(target)
    w.wait(4.0)
    w.press("right")  # Trust
    w.wait(1.0)
    w.type(pw)
    w.wait(3.0)
    w.press("ok")  # $ uptime (first item)
    w.wait(3.0)
    w.press("back")  # Save as alias?
    w.wait(1.0)
    w.press("right")  # Save
    w.type("soak", clear_first=True)
    w.wait(2.5)
    w.press("ok")
    w.press("back")  # disconnect
    w.wait(1.0)
    w.exit_app()
    w.launch()
    w.wait(8.0)
    w.press("down")  # Hosts
    w.press("ok")
    w.wait(1.0)
    w.press("ok")  # the host page, on Connect
    w.wait(0.5)
    w.press("down", 6)  # Save password
    w.press("right")  # Yes
    w.press("down", 5)  # Save
    w.press("ok")
    w.wait(2.5)
    w.press("ok")  # close "Saved": back in Hosts on soak
    w.wait(1.0)
    w.press("ok")  # soak
    w.wait(0.5)
    w.press("ok")  # Connect
    w.wait(4.0)
    w.press("right")  # Trust (pinned per alias)
    w.wait(1.0)
    w.type(pw)  # remembered from now on
    w.wait(3.0)
    w.shot("soak-01-run-menu")
    w.press("back")  # disconnect: back on soak's page, on Connect
    w.wait(1.5)

    t0, cycles = time.monotonic(), 0
    while time.monotonic() - t0 < minutes * 60:
        w.press("ok", delay=0.2)  # connect (remembered password)
        w.wait(3.5)
        w.press("ok", delay=0.2)  # $ uptime
        w.wait(2.5)
        w.press("back", delay=0.2)  # output -> run menu
        w.wait(0.3)
        w.press("back", delay=0.2)  # disconnect -> the host page, on Connect
        w.wait(1.2)
        cycles += 1
        if cycles % 10 == 0:
            w.heap(f"soak cycle {cycles}, {int(time.monotonic() - t0)} s")
    w.log(f"  soak: {cycles} cycles in {int(time.monotonic() - t0)} s")
    w.press("ok")  # one more connect for the last screen
    w.wait(3.5)
    w.press("ok")
    w.wait(2.5)
    w.shot("soak-02-last-output")
    w.exit_app()


# Hardware walks run only when named with --walk (their shots stay in .cache/).
HW_WALKS = {"lancfg": w_lancfg, "cmdlen": w_cmdlen, "link": w_link, "linktest": w_linktest, "replug": w_replug, "m4": w_m4, "m5": w_m5, "soak": w_soak, "heap": w_heap,
            "hwkeys": w_hwkeys}


def run_all(
    r: FlipperRpc, shots: Path, only=None, log=print, hw_shots: Path | None = None, real_backend: bool = False
) -> dict:
    """Mock walks save to `shots` (public); hardware walks show real addresses and save to
    `hw_shots` (kept out of git)."""
    results = {}
    for name, fn in (WALKS | HW_WALKS if only else WALKS).items():
        if only and name not in only:
            continue
        log(f"walk {name}")
        w = Walker(r, hw_shots if name in HW_WALKS and hw_shots else shots, log, real_backend=real_backend)
        t0 = time.monotonic()
        try:
            fn(w)
            results[name] = {"ok": True, "shots": w.taken, "seconds": round(time.monotonic() - t0, 1),
                             "auto_locks": w.locks}
        except AppGone as e:
            log(f"walk {name} STOPPED: {e}; no further walks, going to restore")
            try:
                w.shots = Path(__file__).resolve().parents[2] / ".cache" / "fail-shots"
                w.shot(f"FAIL-{name}", same_ok=True)  # read-only: what is on the screen now
            except Exception as e2:  # noqa: BLE001
                log(f"  screenshot after the stop failed: {e2}")
            results[name] = {"ok": False, "error": str(e), "shots": w.taken, "auto_locks": w.locks,
                             "app_gone": True}
            break
        except OSError as e:  # serial.SerialException: the USB link is gone (a crash reboots the Flipper)
            log(f"walk {name} STOPPED: USB link lost ({e}); the Flipper probably rebooted - no further walks")
            results[name] = {"ok": False, "error": str(e), "shots": w.taken, "auto_locks": w.locks,
                             "link_lost": True}
            break
        except Exception as e:  # noqa: BLE001 - record and continue to restore
            log(f"walk {name} FAILED: {e}")
            try:
                w.shots = Path(__file__).resolve().parents[2] / ".cache" / "fail-shots"
                w.shot(f"FAIL-{name}", same_ok=True)
                w.exit_app()
            except Exception as e2:  # noqa: BLE001
                log(f"  cleanup after failure also failed: {e2}")
            results[name] = {"ok": False, "error": str(e), "shots": w.taken, "auto_locks": w.locks}
    return results






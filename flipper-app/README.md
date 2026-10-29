# SSH Runner (Flipper App)

[← Documentation index](../docs/README.md)

Flipper Zero app for the SSH Runner add-on board (ESP32-S3 + W5500 Ethernet + WiFi).
Pick a host, authenticate with a password or an SSH key from the SD card, then run one command
or a script from the SD card and read its output and exit code.

**The app needs the ESP32-S3 board.** Today that is a Waveshare ESP32-S3-ETH running the SSH
Runner firmware, wired to the Flipper's GPIO header as in
[Hardware and Wiring](../docs/HARDWARE.md); the [Quick Start](../docs/QUICKSTART.md) goes from
the parts to the first command. Without the board the app cannot connect (only the demo build
runs without it, see [Without the Board](#without-the-board)).

## Contents

- [Usage](#usage)
- [Files](#files)
- [Without the Board](#without-the-board)
- [Compatibility](#compatibility)
- [Build](#build)

## Usage

- **Quick Connect**: type `user@host[:port]`, trust the host key, enter the password, run.
  After a successful run the app offers to save the host under a name.
- **Hosts**: saved hosts by name. OK opens the host's page: **Connect**, its settings,
  **Delete**. Each host has a user, address, port, authentication (password, the default key or
  a named key), whether to remember the password (stored only on the ESP32), transport (auto,
  LAN or WiFi), host-key policy (prompt, strict or off) and saved commands.
- **Recent**: the last 10 connections; long-press OK to save one as a host or remove it.
- **Run menu**: saved commands, **Type Command**, **Scripts** (`.sh` files from
  `apps_data/ssh_runner/scripts/`, copied there with qFlipper), **Save Last Output**.
- **Network**: LAN and WiFi status, DHCP or static address, WiFi scan and join, ping (through
  every interface, or one from its page).
- **Keys**: the keys in `apps_data/ssh_runner/keys/` (OpenSSH or PEM files, Ed25519, ECDSA or
  RSA; an encrypted key needs its passphrase only once). **Generate Key** has the ESP32 make an
  Ed25519 or ECDSA P-256 key with an optional passphrase and saves it with its `.pub`; each key
  can **Show Public Key**, **Set as Default**, or be deleted.

Back always goes up one level and leaves the app from Home. Passwords are never written to the
SD card.

## Files

`/ext/apps_data/ssh_runner/`: `hosts/<alias>.conf`, `recent.conf`, `commands.conf` (global
commands, editable), `scripts/`, `logs/`, `keys/`, `settings.conf`.

**Examples.** On the first start the app copies its examples where nothing is yet: nine global
commands (`ips`, `ping`, `disk`, `mem`, `os`, `wifi`, `up`, `load`, `date`; read-only, no
`sudo`; `ping` runs until you stop it, to watch the latency) and `scripts/sysinfo.sh` (one screen
of host status). An existing `commands.conf` or script is never touched, and examples you
delete stay deleted. **Settings > Restore Examples** adds back the missing ones, appended to
your `commands.conf` without changing anything in it. A command is at most 127 characters;
a longer one in a file is not cut: the host shows as *(unreadable)* with the reason, and
`commands.conf` lines that cannot run are counted in the run menu.

## Without the Board

**Demo mode** runs the whole UI against a simulated board with example data, for trying the
app or taking screenshots. It needs the demo build, `ssh_runner_demo.fap` (built from source
next to the app, see [Build Outputs](../docs/ARCHITECTURE.md#build-outputs); not part of a
release), with **Settings > Developer > Mock Backend** set to On; the Home header then shows
DEMO. Settings > Developer appears with the Flipper's Settings > System > Debug on. In the
release build the Mock Backend row stays Off.

Some inputs reach the error paths: password `wrong` gives "Permission
denied", a host ending in `.99` or containing `timeout` times out, a host containing `refused`
is refused, one containing `nxdomain` is not found, and a host containing `robot` shows a
different host key on every connection (same-IP fleet).

## Compatibility

- **Official** `1.5.1` and **Unleashed** `unlshd-094` firmware: one source, two builds
  (`dist/official/`, `dist/unleashed/`). The official build (API 88.2) also runs on Unleashed
  (API 88.19): a FAP needs the same API major and a minor no newer than the firmware's. The
  Unleashed build is optional, for Unleashed only.
- On the official firmware the stock list cannot scroll a long value, so the app shows its
  tail; Unleashed scrolls the selected row.
- **Settings > Developer** (Mock Backend, Link Test) appears only with the Flipper's
  Settings > System > Debug on.
- SSH servers: tested with OpenSSH 8.9; RSA keys sign with `rsa-sha2-*` only.

## Build

`ufbt` in this folder (SDK pinned in `SDK_VERSION`), or `tools/check.ps1` / `tools/check.sh`
from the repository root.

## See Also

- [Quick Start](../docs/QUICKSTART.md): from the parts to the first command
- [PC Tool](../cli/README.md): edit hosts, commands, keys and scripts from a computer
- [Changelog](../CHANGELOG.md)

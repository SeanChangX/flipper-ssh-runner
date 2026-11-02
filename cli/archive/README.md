# sshrunner 0.3.0

`sshrunner` is the PC tool of SSH Runner: it edits the hosts, global commands, keys and
scripts of the SSH Runner Flipper Zero app from a computer, over USB (the connection qFlipper
uses) or on the SD card in a card reader, with a backup before every change.

## Install

- Archive (`.tar.gz`, `.zip`): unpack it and put `sshrunner` (`sshrunner.exe` on Windows) on
  your `PATH`. The binary is static: nothing else to install.
- Debian or Ubuntu: `sudo apt install ./sshrunner_0.3.0_<arch>.deb`; Fedora:
  `sudo dnf install ./sshrunner_0.3.0_<arch>.rpm`. Both install `/usr/bin/sshrunner`.
- On Linux, the Flipper's port needs the `dialout` group: `sudo usermod -aG dialout $USER`,
  then log in again.

## Usage

```sh
sshrunner          # the interactive editor
sshrunner --help   # every command, for scripts
```

Close qFlipper and leave the SSH Runner app while `sshrunner` is connected.

## More

- Full guide: https://github.com/SeanChangX/flipper-ssh-runner/blob/v0.3.0/cli/README.md
- Setting up the board and the Flipper app: https://github.com/SeanChangX/flipper-ssh-runner/blob/v0.3.0/docs/QUICKSTART.md
- License: GPL-3.0-only (`LICENSE`); the third-party licenses are in
  `licenses/THIRD_PARTY_NOTICES.txt`.

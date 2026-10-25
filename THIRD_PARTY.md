# Third-Party Components

[← Back to the README](README.md) · [Documentation index](docs/README.md)

Project code is GPL-3.0-only ([LICENSE](LICENSE)). Bundled or linked third-party code keeps its own
license; every one below is compatible with GPL-3.0 (LGPL-2.1 libssh is linked into the
GPL-3.0 firmware image, which LGPL-2.1 section 3 allows).

| Component | Where | License | Notes |
|---|---|---|---|
| Flipper Zero SDK ([Unleashed](https://github.com/DarkFlippers/unleashed-firmware) `unlshd-094`, [official](https://github.com/flipperdevices/flipperzero-firmware) `1.5.1`) | fetched by uFBT, not bundled | GPL-3.0 | The FAP links against the firmware API at runtime |
| [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.6 | Docker image, not bundled | Apache-2.0 | |
| [libssh](https://www.libssh.org/) 0.11.5 via [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) 5.10.0 (Ewan Parker) | `firmware-esp32/components/libssh/` (vendored) | LGPL-2.1, parts BSD-2-Clause | Provenance and local changes in its `VERSION` file; `COPYING` and `BSD` included. Linked statically into the ESP32 firmware; the firmware source is public, so the LGPL relinking requirement is met by building from this repo. |
| mbedTLS (via ESP-IDF) | Docker image, not bundled | Apache-2.0 | crypto backend for libssh |
| lwIP (via ESP-IDF) | Docker image, not bundled | BSD-3-Clause | TCP/IP stack |
| FreeRTOS kernel (via ESP-IDF) | Docker image, not bundled | MIT | |
| Flipper firmware `text_input.c` (Unleashed `unlshd-094`, commit `2f62b3ab`) | basis of `flipper-app/views/kbd.c` | GPL-3.0 | [`text_input.c` at `unlshd-094`](https://github.com/DarkFlippers/unleashed-firmware/blob/unlshd-094/applications/services/gui/modules/text_input.c) |
| UART Terminal `uart_text_input.c` key table (cool4uma) | `flipper-app/views/kbd_layout.c` | repo says MIT; the file derives from the GPL-3.0 firmware module, so it is treated as GPL-3.0 | [UART Terminal](https://github.com/cool4uma/UART_Terminal); two keys realigned, two long-press alternates added |
| Keyboard key images `KeySave_24x11`, `KeySaveSelected_24x11`, `KeyBackspace_16x9`, `KeyBackspaceSelected_16x9` | `flipper-app/images/` | GPL-3.0 (firmware assets) | Unleashed firmware `assets/icons/Keyboard/` at `unlshd-094` (commit `2f62b3ab`) |
| Warning sign `Alert_9x8`, lock `Lock_7x8` | `flipper-app/images/` (drawn by `tools/make_icons.py`, pixel for pixel) | GPL-3.0 (firmware assets) | `assets/icons/StatusBar/Alert_9x8.png` and `assets/icons/SubGhz/Lock_7x8.png`, identical in the official `1.5.1` and Unleashed `unlshd-094` firmware. FAPs cannot use firmware icons, so they are copied. The LAN and WiFi status icons are drawn for this project |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | embedded in `docs/media/cli/*.svg` (by freeze) | OFL-1.1 | The font of the PC tool figures |
| [freeze](https://github.com/charmbracelet/freeze) v0.2.2 (Charm) | fetched by `tools/cli_figures.py`, not bundled | MIT | Draws the PC tool figures |

The release firmware image links ESP-IDF, mbedTLS, lwIP and FreeRTOS; their license texts
ship next to it in the firmware release archive.

## sshrunner (cli/)

The PC tool is statically linked Go. Its modules (all MIT, BSD-3-Clause or Apache-2.0, so
compatible with GPL-3.0) are listed with their versions in `cli/go.sum`; the main ones:

| Component | License |
|---|---|
| [Bubble Tea](https://github.com/charmbracelet/bubbletea), [Bubbles](https://github.com/charmbracelet/bubbles), [Lip Gloss](https://github.com/charmbracelet/lipgloss) (Charm) | MIT |
| [go.bug.st/serial](https://github.com/bugst/go-serial) | BSD-3-Clause |
| [golang.org/x/crypto, x/sys, x/text](https://pkg.go.dev/golang.org/x) (tests and terminal support) | BSD-3-Clause |

Each release archive carries the license of every module linked into the binary
(`third_party/`, collected by `cli/scripts/licenses.sh`). `github.com/mattn/go-localereader`
ships no license file; its README states MIT.

"""tools/release_assets.py: the release's file set, its one SHA256SUMS, the notes."""

import hashlib
import re
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_assets as ra  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
VER = re.search(r'set\(PROJECT_VER "([0-9.]+)"\)', (ROOT / "firmware-esp32/CMakeLists.txt").read_text()).group(1)
EFUSE_OFF = "CONFIG_IDF_TARGET=\"esp32s3\"\n"
FLASH_ARGS = ("--flash_mode dio --flash_freq 80m --flash_size 16MB\n0x0 bootloader/bootloader.bin\n"
              "0x20000 flipper_eth_ssh.bin\n0x8000 partition_table/partition-table.bin\n")
# (board, zip name, image prefix)
ZIPS = [(ra.BOARDS[0], f"flipper-ssh-runner-esp32-waveshare-v{VER}", f"flipper-ssh-runner-waveshare-v{VER}"),
        (ra.BOARDS[1], f"flipper-ssh-runner-esp32-addon-v{VER}", f"flipper-ssh-runner-addon-v{VER}")]
LICENSES = ["LICENSE", "COPYING.libssh", "BSD.libssh", "LICENSE.esp-idf", "LICENSE.lwip", "LICENSE.freertos",
            "LICENSE.mbedtls", "THIRD_PARTY_NOTICES.txt"]


def esp_build(esp: Path, board: ra.Board, sdkconfig: str = EFUSE_OFF, factory: bytes | None = None) -> None:
    esp.mkdir(parents=True)
    app = board.name.encode() * 100
    (esp / "flipper_eth_ssh.bin").write_bytes(app)
    if factory is None:
        factory = b"\xe9" + b"\xff" * (ra.APP_OFFSET - 1) + app
    (esp / "factory.bin").write_bytes(factory)
    (esp / "flash_args").write_text(FLASH_ARGS)
    (esp / "sdkconfig").write_text(sdkconfig + f"{board.kconfig}=y\n")
    for name in ra.IDF_LICENSES:
        (esp / name).write_text(name)


def builds(tmp: Path, sdkconfig: str = EFUSE_OFF, factory: bytes | None = None, swap: bool = False):
    esp, rev = tmp / "esp32", tmp / "esp32-addon"
    esp_build(esp, ra.BOARDS[0], sdkconfig, factory)
    esp_build(rev, ra.BOARDS[1])
    if swap:
        esp, rev = rev, esp
    cli = tmp / "cli"
    (cli / "sshrunner_linux_amd64_v1").mkdir(parents=True)  # GoReleaser's build folder: not an asset
    for name in (f"sshrunner_{VER}_linux_amd64.tar.gz", f"sshrunner_{VER}_windows_arm64.zip",
                 f"sshrunner_{VER}_amd64.deb", "checksums.txt", "artifacts.json"):
        (cli / name).write_bytes(name.encode())
    for fw in ("official", "unleashed"):
        (tmp / f"{fw}.fap").write_bytes(fw.encode())
    return ["--fap-official", str(tmp / "official.fap"), "--fap-unleashed", str(tmp / "unleashed.fap"),
            "--esp32", str(esp), "--esp32-addon", str(rev), "--cli-dist", str(cli)]


def test_the_release_files_and_one_checksum_file(tmp_path):
    out = tmp_path / "out"
    ra.main(["assets", VER, str(out)] + builds(tmp_path))
    names = sorted(p.name for p in out.iterdir())
    assert names == sorted([
        "SHA256SUMS", "ssh_runner-official.fap", "ssh_runner-unleashed.fap",
        f"flipper-ssh-runner-esp32-waveshare-v{VER}.zip", f"flipper-ssh-runner-esp32-addon-v{VER}.zip",
        f"sshrunner_{VER}_linux_amd64.tar.gz", f"sshrunner_{VER}_windows_arm64.zip", f"sshrunner_{VER}_amd64.deb",
    ])
    sums = dict(line.split("  ")[::-1] for line in (out / "SHA256SUMS").read_text().splitlines())
    assert set(sums) == set(names) - {"SHA256SUMS"}
    for name, digest in sums.items():
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest


@pytest.mark.parametrize("board,zip_name,image", ZIPS)
def test_a_firmware_zip_per_board(tmp_path, board, zip_name, image):
    out = tmp_path / "out"
    ra.main(["assets", VER, str(out)] + builds(tmp_path))
    with zipfile.ZipFile(out / f"{zip_name}.zip") as z:
        base = f"{zip_name}/"
        assert sorted(z.namelist()) == sorted(
            [base + "README.txt", base + f"{image}-factory.bin", base + f"{image}-app.bin"]
            + [base + "licenses/" + n for n in LICENSES])
        names = " ".join(z.namelist())
        assert "flash_args" not in names and "sdkconfig" not in names and "THIRD_PARTY.md" not in names
        app = z.read(base + f"{image}-app.bin")
        assert board.name.encode() in app
        assert z.read(base + f"{image}-factory.bin")[ra.APP_OFFSET:] == app
        assert z.read(base + "licenses/COPYING.libssh") == (ROOT / "firmware-esp32/components/libssh/COPYING").read_bytes()
        assert z.read(base + "licenses/LICENSE") == (ROOT / "LICENSE").read_bytes()
        readme = z.read(base + "README.txt").decode()
    assert f"v{VER}" in readme and board.name in readme
    assert f"{image}-factory.bin  at 0x0" in readme and f"{image}-app.bin  at 0x20000" in readme
    assert f"write_flash 0x0 {image}-factory.bin" in readme and f"write_flash 0x20000 {image}-app.bin" in readme
    assert "erases the board's saved WiFi" in readme and "first install only" in readme
    assert "flash_args" not in readme and "development image" not in readme
    assert board.usb in readme and ("3V3 wire" in readme) == (board.tag == "waveshare")
    # links absolute and pinned to the tag
    assert f"{ra.REPO}/blob/v{VER}/docs/QUICKSTART.md" in readme
    assert "](" not in readme
    # every link to a document is a full URL (plain text: no Markdown links to resolve)
    docs = re.findall(r"\S*\.md\S*", readme)
    assert docs and all(d.startswith(f"{ra.REPO}/blob/v{VER}/") for d in docs), docs
    assert len(readme.splitlines()) <= 25


def test_the_notices_are_plain_text_with_absolute_links():
    text = ra.third_party_notices(VER)
    assert not re.search(r"\]\(|^\||^#|`|\*\*", text, flags=re.M)
    assert f"<{ra.REPO}/blob/v{VER}/LICENSE>" in text
    assert "libssh" in text and "LGPL-2.1" in text
    for url in re.findall(r"<([^>]+)>", text):
        assert url.startswith("https://"), url


def test_the_zip_depends_on_its_content_only(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    args = builds(tmp_path)
    ra.main(["assets", VER, str(a)] + args)
    ra.main(["assets", VER, str(b)] + args)
    for _, name, _ in ZIPS:
        assert (a / f"{name}.zip").read_bytes() == (b / f"{name}.zip").read_bytes()


def test_a_firmware_that_burns_efuses_is_refused(tmp_path):
    args = builds(tmp_path, "CONFIG_SECURE_FLASH_ENC_ENABLED=y\n")
    with pytest.raises(SystemExit):
        ra.main(["assets", VER, str(tmp_path / "out")] + args)


def test_the_builds_of_the_two_boards_cannot_be_swapped(tmp_path):
    args = builds(tmp_path, swap=True)
    with pytest.raises(SystemExit, match="not a build for"):
        ra.main(["assets", VER, str(tmp_path / "out")] + args)


def test_a_factory_image_that_is_not_bootloader_through_app_is_refused(tmp_path):
    args = builds(tmp_path, factory=b"\xe9" + b"\xff" * (ra.APP_OFFSET - 1) + b"another app")
    with pytest.raises(SystemExit, match="factory.bin"):
        ra.main(["assets", VER, str(tmp_path / "out")] + args)


def test_a_version_not_in_the_tree_is_refused():
    with pytest.raises(SystemExit):
        ra.main(["check", "99.0.0"])


def test_notes_are_one_line_per_paragraph_and_item():
    md = "Intro that\nwraps here.\n\n### Added\n\n- An item that\n  goes on.\n- Another.\n"
    assert ra.unwrap(md) == "Intro that wraps here.\n\n### Added\n\n- An item that goes on.\n- Another."
    notes = ra.notes(VER)
    assert "## Install" in notes
    assert f"/blob/v{VER}/docs/" in notes


BLOB = f"{ra.REPO}/blob/v9.9.9/"


@pytest.mark.parametrize("md, pinned", [
    ("[Quick Start](docs/QUICKSTART.md)", f"[Quick Start]({BLOB}docs/QUICKSTART.md)"),
    ("[LED](docs/HARDWARE.md#status-led)", f"[LED]({BLOB}docs/HARDWARE.md#status-led)"),
    ("[PC Tool](cli/README.md)", f"[PC Tool]({BLOB}cli/README.md)"),
    ("[design](hardware/README.md)", f"[design]({BLOB}hardware/README.md)"),
    ("[notices](THIRD_PARTY.md)", f"[notices]({BLOB}THIRD_PARTY.md)"),
    ("[changes](./CHANGELOG.md)", f"[changes]({BLOB}CHANGELOG.md)"),
    ("![render](docs/media/pcb-rev0.1-top.png)", f"![render]({BLOB}docs/media/pcb-rev0.1-top.png)"),
    ("[ref]: THIRD_PARTY.md", f"[ref]: {BLOB}THIRD_PARTY.md"),
    ('<img src="docs/media/demo.gif">', f'<img src="{BLOB}docs/media/demo.gif">'),
    ("[site](https://example.com/x)", "[site](https://example.com/x)"),
    ("[mail](mailto:a@example.com)", "[mail](mailto:a@example.com)"),
])
def test_relative_links_are_pinned_to_the_tag_whatever_their_folder(md, pinned):
    out = ra.pin_links(md, "9.9.9")
    assert out == pinned
    ra.check_links(out)


def test_a_link_to_a_missing_file_stops_the_release():
    with pytest.raises(SystemExit, match="not in the repository"):
        ra.pin_links("[gone](hardware/NOPE.md)", "9.9.9")


@pytest.mark.parametrize("md", ["[top](#install)", "[x](http://example.com)", "[ref]: #top",
                                '<a href="docs/x.md">x</a>'])
def test_a_link_that_does_not_resolve_on_the_release_page_is_refused(md):
    with pytest.raises(SystemExit, match="does not resolve"):
        ra.check_links(md)


def test_every_changelog_section_renders_with_only_absolute_links():
    # a new entry linking a folder or file the tooling has not seen fails here, on its PR
    heads = re.findall(r"^## \[([^\]]+)\]", (ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), re.M)
    assert {"0.2.0", "0.2.1", "0.3.0"} <= set(heads)
    for ver in heads:
        ra.check_links(ra.pin_links(ra.section(ver), "9.9.9"))
    for ver in (h for h in heads if h != "Unreleased"):
        ra.notes(ver)  # pins, then refuses a page with a link that does not resolve


def test_notes_keep_the_approved_layout():
    notes = ra.notes(VER)
    # the release shows its own title and date
    assert "Released" not in notes and f"[{VER}]" not in notes
    # every heading at the level of "## Install"
    heads = [line for line in notes.splitlines() if line.startswith("#")]
    assert len(heads) > 1 and all(h.startswith("## ") for h in heads)
    assert "no Erase" not in notes


def test_notes_install_a_factory_image_first_and_the_app_on_updates():
    notes = ra.notes(VER)
    assert f"`flipper-ssh-runner-esp32-waveshare-v{VER}.zip`" in notes
    assert f"`flipper-ssh-runner-esp32-addon-v{VER}.zip`" in notes
    assert "On a first install, erase the flash, then write the factory image (`-factory.bin`) at `0x0`." in notes
    assert "On an update, write only the app image (`-app.bin`) at `0x20000`, without erasing" in notes
    install = notes[notes.index("## Install"):]  # the changes above may name the old layout
    assert "flash_args" not in install and "three parts" not in install

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


def builds(tmp: Path, sdkconfig: str = "CONFIG_IDF_TARGET=\"esp32s3\"\n"):
    esp = tmp / "esp32"
    for part in ra.FW_PARTS + ra.IDF_LICENSES:
        (esp / part).parent.mkdir(parents=True, exist_ok=True)
        (esp / part).write_bytes(part.encode())
    (esp / "sdkconfig").write_text(sdkconfig)
    cli = tmp / "cli"
    (cli / "sshrunner_linux_amd64_v1").mkdir(parents=True)  # GoReleaser's build folder: not an asset
    for name in (f"sshrunner_{VER}_linux_amd64.tar.gz", f"sshrunner_{VER}_windows_arm64.zip",
                 f"sshrunner_{VER}_amd64.deb", "checksums.txt", "artifacts.json"):
        (cli / name).write_bytes(name.encode())
    for fw in ("official", "unleashed"):
        (tmp / f"{fw}.fap").write_bytes(fw.encode())
    return ["--fap-official", str(tmp / "official.fap"), "--fap-unleashed", str(tmp / "unleashed.fap"),
            "--esp32", str(esp), "--cli-dist", str(cli)]


def test_the_release_files_and_one_checksum_file(tmp_path):
    out = tmp_path / "out"
    ra.main(["assets", VER, str(out)] + builds(tmp_path))
    names = sorted(p.name for p in out.iterdir())
    assert names == sorted([
        "SHA256SUMS", "ssh_runner-official.fap", "ssh_runner-unleashed.fap", f"flipper-ssh-runner-esp32-v{VER}.zip",
        f"sshrunner_{VER}_linux_amd64.tar.gz", f"sshrunner_{VER}_windows_arm64.zip", f"sshrunner_{VER}_amd64.deb",
    ])
    sums = dict(line.split("  ")[::-1] for line in (out / "SHA256SUMS").read_text().splitlines())
    assert set(sums) == set(names) - {"SHA256SUMS"}
    for name, digest in sums.items():
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest
    with zipfile.ZipFile(out / f"flipper-ssh-runner-esp32-v{VER}.zip") as z:
        base = f"flipper-ssh-runner-esp32-v{VER}/"
        assert sorted(z.namelist()) == sorted(base + n for n in ra.FW_PARTS + ra.IDF_LICENSES + [
            "COPYING.libssh", "LICENSE", "THIRD_PARTY.md", "README-flash.txt"])
        assert "sdkconfig" not in " ".join(z.namelist())
        assert f"v{VER}" in z.read(base + "README-flash.txt").decode()


def test_the_zip_depends_on_its_content_only(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    args = builds(tmp_path)
    ra.main(["assets", VER, str(a)] + args)
    ra.main(["assets", VER, str(b)] + args)
    name = f"flipper-ssh-runner-esp32-v{VER}.zip"
    assert (a / name).read_bytes() == (b / name).read_bytes()


def test_a_firmware_that_burns_efuses_is_refused(tmp_path):
    args = builds(tmp_path, "CONFIG_SECURE_FLASH_ENC_ENABLED=y\n")
    with pytest.raises(SystemExit):
        ra.main(["assets", VER, str(tmp_path / "out")] + args)


def test_a_version_not_in_the_tree_is_refused():
    with pytest.raises(SystemExit):
        ra.main(["check", "99.0.0"])


def test_notes_are_one_line_per_paragraph_and_item():
    md = "Intro that\nwraps here.\n\n### Added\n\n- An item that\n  goes on.\n- Another.\n"
    assert ra.unwrap(md) == "Intro that wraps here.\n\n### Added\n\n- An item that goes on.\n- Another."
    notes = ra.notes(VER)
    assert notes.startswith("Released ") and "## Install" in notes
    assert f"/blob/v{VER}/docs/" in notes

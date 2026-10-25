"""tools/flash.py refuses builds that would burn eFuses."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import flash  # noqa: E402


def write_cfg(tmp_path, lines):
    (tmp_path / "sdkconfig").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (tmp_path / "flash_args").write_text("", encoding="utf-8")
    return tmp_path


def test_dev_config_is_clean(tmp_path):
    d = write_cfg(tmp_path, ["CONFIG_IDF_TARGET=\"esp32s3\"", "# CONFIG_SECURE_FLASH_ENC_ENABLED is not set"])
    assert flash.efuse_burning_options(d) == []


def test_release_options_detected(tmp_path):
    d = write_cfg(tmp_path, ["CONFIG_SECURE_FLASH_ENC_ENABLED=y", "CONFIG_NVS_ENCRYPTION=y", "CONFIG_SECURE_BOOT=n"])
    assert flash.efuse_burning_options(d) == ["CONFIG_SECURE_FLASH_ENC_ENABLED", "CONFIG_NVS_ENCRYPTION"]


def test_missing_sdkconfig_refused(tmp_path):
    with pytest.raises(SystemExit):
        flash.efuse_burning_options(tmp_path)


def test_flash_refuses_release_build(tmp_path):
    d = write_cfg(tmp_path, ["CONFIG_SECURE_FLASH_ENC_ENABLED=y"])
    args = _args(d)
    with pytest.raises(SystemExit, match="burns eFuses"):
        flash.cmd_flash(args)


def _args(d, efuse_test=False):
    return type("A", (), {"build": str(d), "port": "COM99", "baud": 460800, "efuse_test": efuse_test,
                          "i_understand_this_burns_efuses": False})()


VIRTUAL = ["CONFIG_EFUSE_VIRTUAL=y", "CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH=y", "# CONFIG_NVS_ENCRYPTION is not set"]


def test_efuse_test_build_accepted(tmp_path, monkeypatch):
    """The virtual-eFuse test image passes, and only then is esptool called."""
    d = write_cfg(tmp_path, VIRTUAL)
    assert flash.efuse_test_problems(d) == []
    calls = []
    monkeypatch.setattr(flash.subprocess, "call", lambda *a, **k: calls.append(a) or 0)
    assert flash.cmd_flash(_args(d, efuse_test=True)) == 0
    assert len(calls) == 1


@pytest.mark.parametrize("lines, why", [
    (["# CONFIG_EFUSE_VIRTUAL is not set"], "CONFIG_EFUSE_VIRTUAL is not set"),  # the real-eFuse dev image
    (["CONFIG_EFUSE_VIRTUAL=y"], "CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH is not set"),
    (VIRTUAL + ["CONFIG_NVS_ENCRYPTION=y"], "CONFIG_NVS_ENCRYPTION=y"),
    (VIRTUAL + ["CONFIG_SECURE_FLASH_ENC_ENABLED=y"], "CONFIG_SECURE_FLASH_ENC_ENABLED=y"),
    (VIRTUAL + ["CONFIG_SECURE_BOOT=y"], "CONFIG_SECURE_BOOT=y"),
])
def test_efuse_test_refuses_unsafe_builds(tmp_path, monkeypatch, lines, why):
    d = write_cfg(tmp_path, lines)
    monkeypatch.setattr(flash.subprocess, "call", lambda *a, **k: pytest.fail("esptool must not run"))
    with pytest.raises(SystemExit, match="not a virtual-eFuse test build") as e:
        flash.cmd_flash(_args(d, efuse_test=True))
    assert why in str(e.value)


def test_test_partitions_never_touch_the_boards_nvs(tmp_path):
    parts = flash.test_partitions()  # the real partitions.csv
    assert sorted(n for n, _, _ in parts) == ["efuse_em", "fes_sec", "nvs_test"]
    assert all(off >= 0x320000 for _, off, _ in parts)
    bad = tmp_path / "p.csv"
    bad.write_text("fes_sec, data, nvs, 0x9000, 0x10000,\nnvs_test, data, nvs, 0x330000, 0x10000,\n"
                   "efuse_em, data, efuse, 0x340000, 0x2000,\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="overlaps"):
        flash.test_partitions(bad)


class _Port:
    def __init__(self, device, vid, pid):
        self.device, self.vid, self.pid = device, vid, pid


@pytest.mark.skipif(sys.platform == "win32", reason="by-id links are Linux")
def test_guard_resolves_by_id_links(tmp_path, monkeypatch):
    """A /dev/serial/by-id/... symlink to the Flipper's ttyACM is refused like the tty."""
    from serial.tools import list_ports

    flipper, esp32 = tmp_path / "ttyACM1", tmp_path / "ttyACM0"
    flipper.touch()
    esp32.touch()
    monkeypatch.setattr(
        list_ports, "comports", lambda: [_Port(str(flipper), 0x0483, 0x5740), _Port(str(esp32), 0x303A, 0x1001)]
    )
    by_id = tmp_path / "by-id"
    by_id.mkdir()
    (by_id / "usb-Flipper_Devices_Inc._Flipper-if00").symlink_to(flipper)
    (by_id / "usb-Espressif_USB_JTAG_serial_debug_unit-if00").symlink_to(esp32)
    with pytest.raises(SystemExit, match="Flipper"):
        flash.guard_port(str(by_id / "usb-Flipper_Devices_Inc._Flipper-if00"))
    with pytest.raises(SystemExit, match="Flipper"):
        flash.guard_port(str(flipper))
    flash.guard_port(str(by_id / "usb-Espressif_USB_JTAG_serial_debug_unit-if00"))  # the ESP32: allowed

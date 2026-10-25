"""tools/cli_figures.py: every figure exists and is used, and --check ignores only the time."""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cli_figures  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def test_every_figure_is_drawn():
    for name in cli_figures.FIGURES:
        assert (cli_figures.OUT / f"{name}.svg").is_file(), f"run tools/cli_figures.py: {name}.svg is missing"


def test_docs_use_only_generated_figures():
    used = set()
    for md in [ROOT / "README.md", *ROOT.glob("docs/*.md"), *ROOT.glob("*/README.md")]:
        used |= set(re.findall(r"media/cli/([\w-]+)\.svg", md.read_text(encoding="utf-8")))
    assert used <= set(cli_figures.FIGURES), f"not made by cli_figures.py: {used - set(cli_figures.FIGURES)}"
    assert {p.stem for p in cli_figures.OUT.glob("*")} == set(cli_figures.FIGURES)


def test_check_ignores_the_backup_time_only():
    a = b"<text>sshrunner-20261025-101438  2026-10-25 10:14:38  folder flipper</text>"
    b = b"<text>sshrunner-20270101-000001  2027-01-01 00:00:01  folder flipper</text>"
    assert cli_figures.TIME.sub(b"T", a) == cli_figures.TIME.sub(b"T", b)
    assert cli_figures.TIME.sub(b"T", a) != cli_figures.TIME.sub(b"T", a.replace(b"flipper", b"card"))

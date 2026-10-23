from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AGENT = ROOT / "python-workflow" / "_agent"
sys.path.insert(0, str(AGENT))

import nbs  # noqa: E402


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, *map(str, args)],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )


def test_demo_pipeline(tmp_path: Path) -> None:
    a = tmp_path / "demo_A.nbs"
    b = tmp_path / "demo_B.nbs"

    result = run("python-workflow/midi2nbs.py", "python-workflow/samples/demo.mid", "-o", a, "--force")
    assert result.returncode == 0, result.stdout + result.stderr
    assert a.exists()

    song = nbs.load(str(a))
    notes = [(n.tick, li, n.instrument, n.key) for li, layer in enumerate(song.layers) for n in layer.notes]
    assert len(song.layers) == 16
    assert len(notes) == 38
    assert all(0 <= n[3] <= 87 for n in notes)
    assert len({(tick, li) for tick, li, _, _ in notes}) == len(notes)

    result = run("python-workflow/nbs_postprocess.py", "--in", a, "--out", b)
    assert result.returncode == 0, result.stdout + result.stderr
    assert b.exists()

    song = nbs.load(str(b))
    notes = [n for layer in song.layers for n in layer.notes]
    assert len(song.layers) == 15
    assert len(notes) == 38
    assert all(33 <= n.key <= 57 for n in notes)


def test_help_has_no_legacy_names() -> None:
    result = run("python-workflow/midi2nbs.py", "--help")
    assert result.returncode == 0, result.stderr
    text = result.stdout.lower()
    assert "--no-drum-fallback" in text
    assert "--fold" not in text
    assert "nbl" not in text
    assert "noteblocklib" not in text


def test_quiet_postprocess(tmp_path: Path) -> None:
    a = tmp_path / "demo_A.nbs"
    b = tmp_path / "demo_B.nbs"
    assert run("python-workflow/midi2nbs.py", "python-workflow/samples/demo.mid", "-o", a, "--force").returncode == 0
    result = run("python-workflow/nbs_postprocess.py", "--in", a, "--out", b, "--quiet")
    assert result.returncode == 0, result.stderr
    assert "整理后：" in result.stdout
    assert "改动：" not in result.stdout

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def run_check(script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, script, "--check"],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )


def test_tables_are_generated() -> None:
    result = run_check("tools/gen_tables.py")
    assert result.returncode == 0, result.stdout + result.stderr


def test_pinyin_table_is_generated() -> None:
    pytest.importorskip("pypinyin")
    result = run_check("tools/gen_pinyin_table.py")
    assert result.returncode == 0, result.stdout + result.stderr

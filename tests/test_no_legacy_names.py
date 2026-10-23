from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", "NyaaFlavor-archive", "build", "artifacts", "__pycache__", ".pytest_cache", "tests", "out", "dist"}
TEXT_SUFFIXES = {".md", ".py", ".cpp", ".h", ".ps1", ".txt", ".yml", ".yaml", ".json", ".ini",
                 ".gitignore", ".gitattributes", ".manifest"}
PATTERN = re.compile(r"noteblocklib|NBL_|nbl_|(?<![A-Za-z])nbl(?![A-Za-z])", re.IGNORECASE)


def test_no_legacy_nbl_names() -> None:
    hits: list[str] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT)
        if any(part in SKIP_DIRS for part in relative.parts):
            continue
        if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in {".gitignore", ".gitattributes"}:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if PATTERN.search(line):
                hits.append(f"{relative}:{number}: {line.strip()}")
    assert not hits, "legacy NBL/NoteBlockLib naming remains:\n" + "\n".join(hits)

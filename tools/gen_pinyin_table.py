#!/usr/bin/env python3
"""Generate cpp/src/pinyin_table.cpp from pypinyin.

Usage:
    python -m pip install pypinyin==0.55.0
    python tools/gen_pinyin_table.py          # write cpp/src/pinyin_table.cpp
    python tools/gen_pinyin_table.py --check  # fail if the checked-in file is stale
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "cpp" / "src" / "pinyin_table.cpp"
CJK_START = 0x4E00
CJK_COUNT = 0x9FFF - 0x4E00 + 1
NO_PINYIN = 0xFFFF
CHUNK = 96


def build_text() -> str:
    try:
        from pypinyin import lazy_pinyin
    except ImportError:
        raise SystemExit("需要 pypinyin：python -m pip install pypinyin==0.55.0")

    pool: list[str] = []
    index: dict[str, int] = {}
    offsets: list[int] = []
    raw = ""
    for cp in range(CJK_START, CJK_START + CJK_COUNT):
        syllable = lazy_pinyin(chr(cp))[0]
        if not syllable or not syllable.isascii():
            offsets.append(NO_PINYIN)
            continue
        if syllable not in index:
            index[syllable] = len(raw)
            pool.append(syllable)
            raw += syllable + "\x00"
        offsets.append(index[syllable])

    lines: list[str] = []
    add = lines.append
    add("// 自动生成，勿手改：python tools/gen_pinyin_table.py")
    add("// 数据来源 pypinyin.lazy_pinyin 首音节，覆盖 U+4E00..U+9FFF")
    add('#include "pinyin.h"')
    add("")
    add("namespace nyaa {")
    add("namespace {")
    add("constexpr unsigned kCjkStart = 0x4E00;")
    add(f"constexpr unsigned kCjkCount = {CJK_COUNT};")
    add("constexpr unsigned kNoPinyin = 0xFFFF;")
    add("")
    add("const char kPool[] =")
    chunks = [raw[i:i + CHUNK] for i in range(0, len(raw), CHUNK)]
    for i, chunk in enumerate(chunks):
        escaped = chunk.replace("\x00", "\\000")
        add(f'    "{escaped}"' + (";" if i == len(chunks) - 1 else ""))
    add("")
    add("const unsigned short kOffsets[] = {")
    for i in range(0, len(offsets), 16):
        row = ",".join(str(v) for v in offsets[i:i + 16])
        add(f"    {row},")
    add("};")
    add("}  // namespace")
    add("")
    add("std::string pinyin_first(unsigned cp) {")
    add("    if (cp < kCjkStart || cp >= kCjkStart + kCjkCount) return {};")
    add("    unsigned off = kOffsets[cp - kCjkStart];")
    add("    if (off == kNoPinyin) return {};")
    add("    return std::string(kPool + off);")
    add("}")
    add("")
    add("}  // namespace nyaa")
    add("")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="only verify that pinyin_table.cpp is current")
    args = parser.parse_args(argv)

    generated = build_text()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n") if OUTPUT.exists() else ""
        if current != generated:
            print(f"{OUTPUT.relative_to(ROOT)} is stale; run: python tools/gen_pinyin_table.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0

    OUTPUT.write_text(generated, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""gp -> midi for the NyaaFlavor pipeline (MuseScore + ghost-note pruning).

Why this exists: a Guitar Pro score exported to MIDI by MuseScore keeps every
muted "ghost" strum as a real short note.  Those notes are transcription noise;
the reference arrangements drop them wholesale (measured: 95% of sub-60 ms notes
never appear in the finished .nbs).  This script converts the .gp and then
removes them, so the rest of the pipeline never sees them.

usage
  python gp2midi.py song.gp                  # -> song.mid next to the .gp
  python gp2midi.py song.gp -o out.mid --min-ms 60
  python gp2midi.py song.gp --keep-raw       # keep the un-pruned export

Requires MuseScore 4 (auto-detected, or pass --musescore).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:  # mido 只在 ① gp→midi（洗 ghost）这一步才需要
    import mido
except ImportError:  # pragma: no cover
    mido = None


def require_mido() -> None:
    if mido is None:
        raise SystemExit("这一步需要 mido：pip install mido（或用 cpp/ 里的单文件 exe，无需依赖）")

CANDIDATES = [
    # Windows
    r"C:\Program Files\MuseScore 4\bin\MuseScore4.exe",
    r"C:\Program Files\MuseScore 4\bin\MuseScore3.exe",
    r"C:\Program Files\MuseScore 3\bin\MuseScore3.exe",
    r"C:\Program Files (x86)\MuseScore 3\bin\MuseScore3.exe",
    # macOS
    "/Applications/MuseScore 4.app/Contents/MacOS/mscore",
    "/Applications/MuseScore 3.app/Contents/MacOS/mscore",
    # Linux（发行版包名不统一，PATH 里能搜到就行）
]


def find_musescore(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        sys.exit(f"指定的 MuseScore 不存在：{explicit}")
    found = os.environ.get("MUSESCORE")
    if found and Path(found).exists():
        return Path(found)
    for c in CANDIDATES:
        if Path(c).exists():
            return Path(c)
    for name in ("mscore4", "mscore3", "mscore", "musescore"):
        which = shutil.which(name)
        if which:
            return Path(which)
    sys.exit("未找到 MuseScore 4。请安装 MuseScore 4，或用 --musescore 指定 MuseScore4.exe（也可设置 MUSESCORE）。")


def ms_per_tick(mid) -> float:
    tempo, ppq = 500000, mid.ticks_per_beat
    for tr in mid.tracks:
        for msg in tr:
            if msg.type == "set_tempo":
                tempo = msg.tempo
                break
        break
    return tempo / 1000.0 / ppq


def chromatic_runs(short_pairs, max_gap: int) -> set[int]:
    """Note-onset indices that belong to a fast +/-1 semitone run (a glissando).

    A glissando is written as a burst of very short notes, so a plain duration
    filter would delete it before script A can compress it into a grace note.
    short_pairs: (channel, on_tick, off_tick, pitch, onset_index)
    """
    protected: set[int] = set()
    by_channel: dict[int, list[tuple[int, int, int]]] = {}
    for channel, on, _off, pitch, idx in short_pairs:
        by_channel.setdefault(channel, []).append((on, pitch, idx))
    for chain_notes in by_channel.values():
        chain_notes.sort()
        chain: list[tuple[int, int, int]] = []
        for on, pitch, idx in chain_notes:
            if chain and on - chain[-1][0] <= max_gap and abs(pitch - chain[-1][1]) == 1:
                chain.append((on, pitch, idx))
            else:
                if len(chain) >= 3:
                    protected.update(n[2] for n in chain)
                chain = [(on, pitch, idx)]
        if len(chain) >= 3:
            protected.update(n[2] for n in chain)
    return protected


def prune(src: Path, dst: Path, min_ms: float, keep_gliss: bool = True):
    require_mido()
    mid = mido.MidiFile(str(src))
    thresh = min_ms / ms_per_tick(mid)
    gap = max(2, mid.ticks_per_beat // 12)          # the same rule script A uses
    out = mido.MidiFile(ticks_per_beat=mid.ticks_per_beat)
    kept = dropped = protected_n = 0
    for tr in mid.tracks:
        new = mido.MidiTrack()
        out.tracks.append(new)
        t, events = 0, []
        for msg in tr:
            t += msg.time
            events.append((t, msg))
        open_notes, pairs = {}, []
        for i, (tick, msg) in enumerate(events):
            if msg.type == "note_on" and msg.velocity > 0:
                open_notes.setdefault((msg.channel, msg.note), []).append(i)
            elif msg.type == "note_off" or (msg.type == "note_on" and msg.velocity == 0):
                key = (msg.channel, msg.note)
                if not open_notes.get(key):
                    continue
                on_i = open_notes[key].pop(0)
                pairs.append((i, on_i, events[on_i][0], tick, msg.channel, msg.note))
        short = [(p[4], p[2], p[3], p[5], p[1]) for p in pairs if p[3] - p[2] < thresh]
        protected_ids = chromatic_runs(short, gap) if keep_gliss else set()
        protected_n += len(protected_ids)
        drop_ids: set[int] = set()
        for off_i, on_i, on_t, off_t, _ch, _pitch in pairs:
            if off_t - on_t < thresh:
                if on_i in protected_ids:
                    kept += 1
                    continue
                drop_ids.update((on_i, off_i))
                dropped += 1
            else:
                kept += 1
        last = 0
        for i, (tick, msg) in enumerate(events):
            if i in drop_ids:
                continue
            new.append(msg.copy(time=tick - last))
            last = tick
    out.save(str(dst))
    return kept, dropped, protected_n


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Guitar Pro → MIDI（MuseScore）+ 过滤短促杂音；"
                    "--prune-only 只清理已有 MIDI")
    ap.add_argument("src", help="要转换的 .gp；--prune-only 时是 .mid")
    ap.add_argument("-o", "--out")
    ap.add_argument("--min-ms", type=float, default=60.0)
    ap.add_argument("--musescore")
    ap.add_argument("--keep-raw", action="store_true")
    ap.add_argument("--prune-only", action="store_true",
                    help="treat src as an existing .mid and only remove ghost notes")
    ap.add_argument("--no-protect-gliss", action="store_true",
                    help="also prune the short notes that make up a glissando run")
    args = ap.parse_args(argv)

    src = Path(args.src)
    if not src.exists():
        sys.exit(f"找不到文件：{src}")

    if args.prune_only:
        dst = Path(args.out) if args.out else src.with_name(src.stem + "_noghost.mid")
        kept, dropped, prot = prune(src, dst, args.min_ms, not args.no_protect_gliss)
        print(f"{src.name} → {dst.name}：保留 {kept} 个音，删掉 {dropped} 个鬼音"
              f"（短于 {args.min_ms:.0f} ms），保留 {prot} 个滑音")
        return 0

    dst = Path(args.out) if args.out else src.with_suffix(".mid")
    exe = find_musescore(args.musescore)

    raw = src.with_name(src.stem + ".raw.mid") if args.keep_raw else \
        Path(tempfile.gettempdir()) / (src.stem + ".raw.mid")
    r = subprocess.run([str(exe), "-o", str(raw), str(src)], capture_output=True, text=True)
    if not raw.exists():
        detail = f"\n详情：{r.stderr[:400]}" if r.stderr else ""
        sys.exit(f"MuseScore 转换失败（返回码 {r.returncode}）。"
                 f"请先确认这个谱能在 MuseScore 4 里正常打开。{detail}")

    kept, dropped, prot = prune(raw, dst, args.min_ms, not args.no_protect_gliss)
    print(f"{src.name} → {dst.name}：保留 {kept} 个音，删掉 {dropped} 个鬼音"
          f"（短于 {args.min_ms:.0f} ms），保留 {prot} 个滑音")
    if not args.keep_raw and raw.exists():
        raw.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main())

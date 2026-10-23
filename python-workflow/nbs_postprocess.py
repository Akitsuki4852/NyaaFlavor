"""Script B — turn a template cast (script A) into an Akitsuki-style .nbs.

Pipeline (all steps deterministic, each one switchable off):

  1. FOLD      keys outside 33..57 are moved by octaves into the window.
  2. RHYTHM    inside *rhythm* blocks: when one block puts more than `keep_low`
               guitar notes on a single tick, the notes above the lowest
               `keep_low` become `bit`, written one octave lower (same sounding
               pitch, because guitar sounds 12 semitones below harp/bit).
  3. MERGE     lead blocks that duplicate each other (same pitch classes on the
               same tick) are merged into one line written as `bell` + `bit`;
               the duplicate block is dropped.  This is the "two guitars play the
               same melody an octave apart" case.
  4. BORROW    a lead line that is left over (not merged) and still sits above
               the window switches to `bit` instead of a plain octave fold.
  5. CLASH     `bit` notes that collide with a lead `guitar` note on the same
               (tick, key) are dropped.
  6. DEDUPE    duplicate (tick, key, instrument) triples are collapsed.
  7. LAYOUT    empty layers are kept (the muted separators are part of the
               layout); the generated `bell` layers are inserted right after the
               vocal block.

Layer blocks are read from script A's separator layers (`── NAME ──`); a block
counts as rhythm / lead by the prefix of that name.

usage
  python nbs_postprocess.py --in A.nbs --out B.nbs
  python nbs_postprocess.py --in A.nbs --out B.nbs --compare ref.nbs
  python nbs_postprocess.py --in A.nbs --out B.nbs --align ref.nbs
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

for _p in Path(__file__).resolve().parents:
    if (_p / "_agent" / "nbs.py").exists():
        ROOT = _p
        break
else:
    raise SystemExit("找不到 _agent/nbs.py")
sys.path.insert(0, str(ROOT / "_agent"))
import nbs                                     # noqa: E402

LO, HI = 33, 57
DEFAULTS = dict(
    keep_low=3,          # guitar notes kept at the bottom of a rhythm chord
    bell_offset=-12,     # bell written this far from the merged line
    rhythm_prefix="rhythm",
    lead_prefix="lead",
)

# ---------------------------------------------------------------------------
# Instrument register table — the single source of truth for every "borrow the
# range" decision.  Thirteen pitched instruments (drums and the trumpet set are
# left out: they are never borrowed, they are assigned explicitly).
#
# `offset` is the SOUNDING pitch of written key 33 relative to harp, i.e.
# sounding = key + offset.  The values come from the author's own instrument
# list (each timbre's native F#..F# window), and the five with a `check`
# marker were additionally verified against the finished songs.
#
# `up` / `down` are the preferred timbres when the intended sounding pitch
# falls outside what the current instrument can play at a legal written key
# (33..57).  The key is then computed, not stored: key = sounding - offset[tgt].
# Order matters — the first target that lands inside the window wins.
# ---------------------------------------------------------------------------
INSTRUMENTS = {
    #            offset  native window     up targets                        down targets
    "harp":     dict(offset=0,   span=(33, 57), up=["bell", "flute"],        down=["guitar"]),
    "dbass":    dict(offset=-24, span=(9, 33),  up=["guitar"],               down=[]),
    "guitar":   dict(offset=-12, span=(21, 45), up=["bit"],                  down=[]),
    "flute":    dict(offset=12,  span=(45, 69), up=["bell"],                 down=["harp", "bell"]),
    "bell":     dict(offset=24,  span=(57, 81), up=["xylophone"],            down=["harp", "flute"]),
    "icechime": dict(offset=24,  span=(57, 81), up=[],                       down=["harp", "bell"]),
    "xylophone": dict(offset=24, span=(57, 81), up=[],                       down=["harp", "bit"]),
    "iron_xyl": dict(offset=0,   span=(33, 57), up=["cow_bell","xylophone"], down=[]),
    "cow_bell": dict(offset=12,  span=(45, 69), up=["xylophone"],            down=["harp", "iron_xyl"]),
    "didgeridoo": dict(offset=-24, span=(9, 33), up=["guitar"],              down=[]),
    "bit":      dict(offset=0,   span=(33, 57), up=["xylophone", "bell"],    down=["guitar"]),
    "banjo":    dict(offset=0,   span=(33, 57), up=["bell"],                 down=["guitar"]),
    "pling":    dict(offset=0,   span=(33, 57), up=["flute", "bell"],        down=["guitar"]),
}


def borrow(inst: str, sounding: int, prefer=None) -> tuple[str, int] | None:
    """Best (instrument, written key) for `sounding`, or None if nothing fits.

    先试 `prefer`（按曲目/音色给的偏好，如电钢的 flute → bell），
    再试直接目标，最后跳一层（guitar → bit → xylophone）：bit 自己写不下的时候，
    xylophone 的可用写域比 bit 低两个八度，正好接住「sounding > 57」的高音。
    """
    info = INSTRUMENTS.get(inst)
    if info is None:
        return None
    chain: list[str] = []
    for t in (prefer or ()):
        if t != inst and t in INSTRUMENTS and t not in chain:
            chain.append(t)
    for t in info["up"] + info["down"]:
        if t != inst and t not in chain:
            chain.append(t)
    for t in list(chain):                       # 第二跳
        for t2 in INSTRUMENTS[t]["up"] + INSTRUMENTS[t]["down"]:
            if t2 != inst and t2 not in chain:
                chain.append(t2)
    for t in chain:
        k = sounding - INSTRUMENTS[t]["offset"]
        if LO <= k <= HI:
            return t, k
    # 没有任何音色能在窗口里精确表达这个发声（比如发声 83：bell 要 key 59）。
    # 退而求其次：所有音色都试一遍，取「折八度之后误差最小」的那个 —— 音级不变，
    # 只会在八度上差，最坏 12 个半音；总比原地折成 47（差 36 个半音）强。
    best: tuple[int, str, int] | None = None
    for t in [inst] + chain:
        if t not in INSTRUMENTS:
            continue
        k = fold(sounding - INSTRUMENTS[t]["offset"])
        err = abs(k + INSTRUMENTS[t]["offset"] - sounding)
        if best is None or err < best[0]:
            best = (err, t, k)
    return (best[1], best[2]) if best else None


def fold(key: int) -> int:
    while key < LO:
        key += 12
    while key > HI:
        key -= 12
    return key


DRUM_BLOCK = re.compile(
    r"^(?:DRUMS\b|GM\d+$|HH-[CO]$|CRASH$|SPLASH$|SNARE$|TOM-[LMH]$|BD$|STICK$)")


def is_drum_block(name: str) -> bool:
    """鼓组里的层（套鼓槽位 / GM 兜底槽位 / DRUMS 组标题）。"""
    return bool(DRUM_BLOCK.match((name or "").strip()))


def block_names(song) -> dict:
    """layer index -> block name, taken from the `── NAME ──` separator layers."""
    out, cur = {}, "misc"
    for li, layer in enumerate(song.layers):
        name = (layer.name or "").strip()
        if name.startswith(("──", "--")):
            cur = name.strip("─- ").split("[")[0].strip()
        out[li] = cur
    return out


def block_programs(song) -> dict:
    """layer index -> GM program（A 用 `p4` / `p5` 标记电钢琴，其它块为 None）。"""
    import re

    out, cur = {}, None
    for li, layer in enumerate(song.layers):
        name = (layer.name or "").strip()
        if name.startswith(("──", "--")):
            m = re.search(r"\bp(\d{1,3})\b", name)
            cur = int(m.group(1)) if m else None
        out[li] = cur
    return out


def best_shift(notes, ref_notes, max_shift=64):
    """Tick shift that maximises the (tick, pitch-class) overlap with a reference.

    两种行格式都接受：`(tick, key, inst)` 与 `[tick, layer, inst, key]`。
    """
    def tick_key(r):
        return r[0], (r[3] if len(r) == 4 else r[1])

    ref = collections.defaultdict(set)
    for row in ref_notes:
        tick, key = tick_key(row)
        ref[tick].add(key % 12)
    best = (0, -1)
    for s in range(-max_shift, max_shift + 1):
        hit = 0
        for row in notes:
            tick, key = tick_key(row)
            if key % 12 in ref.get(tick + s, ()):
                hit += 1
        if hit > best[1]:
            best = (s, hit)
    return best[0]


def main(argv=None):
    # Windows 控制台默认可能是 cp950/cp936，中文会直接抛 UnicodeEncodeError
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass

    ap = argparse.ArgumentParser(description="template cast -> Akitsuki style")
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", dest="dst", required=True)
    ap.add_argument("--compare", help="reference .nbs to score against")
    ap.add_argument("--align", help="reference .nbs used to auto-compute the tick shift")
    ap.add_argument("--shift", type=int, default=0, help="manual tick shift (overrides --align)")
    ap.add_argument("--keep-low", type=int, default=DEFAULTS["keep_low"])
    ap.add_argument("--bell-offset", type=int, default=DEFAULTS["bell_offset"])
    ap.add_argument("--no-fold", action="store_true")
    ap.add_argument("--no-rhythm", action="store_true")
    ap.add_argument("--no-merge", action="store_true")
    ap.add_argument("--no-borrow", action="store_true")
    ap.add_argument("--borrow-table", dest="borrow_table", action="store_true", default=True,
                    help="route out-of-window notes through INSTRUMENTS instead of folding (default)")
    ap.add_argument("--no-borrow-table", dest="borrow_table", action="store_false",
                    help="disable the borrow table (fall back to plain octave folding)")
    ap.add_argument("--no-clash", action="store_true")
    ap.add_argument("--no-dedupe", action="store_true")
    ap.add_argument("--drop-empty", action="store_true")
    ap.add_argument("--strings", choices=["cp1252", "utf8"], default="cp1252",
                    help="cp1252 = ASCII/pinyin metadata (NBS audio export can read it); "
                         "utf8 = keep Chinese, NBS export will fail")
    ap.add_argument("--no-tidy", action="store_true",
                    help="keep empty non-separator layers and adjacent separators")
    ap.add_argument("-q", "--quiet", action="store_true", help="only print the output file summary")
    args = ap.parse_args(argv)

    song = nbs.load(args.src)
    blk = block_names(song)
    prog = block_programs(song)
    stats = collections.Counter()

    # notes are plain mutable rows: [tick, layer, instrument, key]
    notes = [[n.tick, li, n.instrument, n.key]
             for li, layer in enumerate(song.layers) for n in layer.notes]

    # ---- 1. fold ------------------------------------------------------------
    folded = {}
    if not args.no_fold:
        for row in notes:
            if LO <= row[3] <= HI:
                continue
            if is_drum_block(blk[row[1]]):
                # 鼓的 key 只表示"哪个鼓音"，折八度不改音色；也不进 folded，
                # 免得 4b 把它当越界旋律音去借音色（GM81 chime 被改成 harp 就是这么来的）
                row[3] = fold(row[3])
                stats["鼓块只折八度"] += 1
                continue
            folded[id(row)] = ("high" if row[3] > HI else "low", row[3], row[2])
            row[3] = fold(row[3])
            stats["八度折叠"] += 1

    # ---- 2. rhythm guitar stacks -> bit ------------------------------------
    if not args.no_rhythm:
        per_block_tick = collections.defaultdict(list)
        for row in notes:
            if blk[row[1]].lower().startswith(DEFAULTS["rhythm_prefix"]) and \
                    nbs.iname(row[2]) == "guitar":
                per_block_tick[(blk[row[1]], row[0])].append(row)
        for rows in per_block_tick.values():
            if len(rows) <= args.keep_low:
                continue
            rows.sort(key=lambda r: r[3])
            for row in rows[args.keep_low:]:
                row[2] = nbs.INSTR_ID["bit"]
                row[3] = fold(row[3] - 12)
                stats["节奏吉他转 bit"] += 1
            # guitar left under the window goes up an octave; a duplicate that
            # this creates is removed by the dedupe step at the end
            for row in rows[:args.keep_low]:
                if row[3] < LO:
                    row[3] += 12
                    stats["低音 guitar 升八度"] += 1

    # ---- 3. merge duplicated lead lines ------------------------------------
    if not args.no_merge:
        # a lead block that repeats a pitch class on the same tick (octave
        # doubling inside one block) keeps only the lowest written key
        keep = {}
        for row in notes:
            if blk[row[1]].lower().startswith(DEFAULTS["lead_prefix"]) and \
                    nbs.iname(row[2]) == "guitar":
                k = (row[0], blk[row[1]], row[3] % 12)
                if k not in keep or row[3] < keep[k][3]:
                    keep[k] = row
        keep_ids = {id(r) for r in keep.values()}
        dropped = [r for r in notes
                   if blk[r[1]].lower().startswith(DEFAULTS["lead_prefix"])
                   and nbs.iname(r[2]) == "guitar" and id(r) not in keep_ids]
        stats["八度重复"] = len(dropped)
        notes = [r for r in notes if id(r) not in {id(x) for x in dropped}]

        by_tick = collections.defaultdict(lambda: collections.defaultdict(list))
        for row in notes:
            if blk[row[1]].lower().startswith(DEFAULTS["lead_prefix"]) and \
                    nbs.iname(row[2]) == "guitar":
                by_tick[row[0]][blk[row[1]]].append(row)
        bell_rows = []
        for tick, per_block in by_tick.items():
            if len(per_block) < 2:
                continue
            sig = {b: frozenset(r[3] % 12 for r in rows) for b, rows in per_block.items()}
            groups = collections.defaultdict(list)
            for b, s in sig.items():
                groups[s].append(b)
            for s, blocks_ in groups.items():
                if len(blocks_) < 2:
                    continue
                blocks_.sort(key=lambda b: min(r[3] for r in per_block[b]))
                keep_b, dups = blocks_[0], blocks_[1:]
                for b in dups:
                    for r in per_block[b]:
                        r[2] = None                       # mark for removal
                for r in per_block[keep_b]:
                    k = r[3]
                    r[2] = nbs.INSTR_ID["bit"]
                    bk = k + args.bell_offset
                    if bk < LO:
                        bell_rows.append([tick, -1, nbs.INSTR_ID["harp"], fold(bk + 24)])
                        stats["bell 越界改 harp"] += 1
                    else:
                        bell_rows.append([tick, -1, nbs.INSTR_ID["bell"], fold(bk)])
                        stats["重复主音合并"] += 1
        stats["重复块丢弃"] += sum(1 for r in notes if r[2] is None)
        notes = [r for r in notes if r[2] is not None]
        notes.extend(bell_rows)

    # ---- 4. leftover lead lines above the window borrow bit ----------------
    # 高音 lead：guitar 的写域只到 57（发声 45）。发声 46..57 用 bit 正好接住；
    # 发声 58 以上 bit 也写不下（key = 发声 > 57），再借一层到 xylophone（key −24）。
    if not args.no_borrow:
        for row in notes:
            info = folded.get(id(row))
            if not info or info[0] != "high" or row[1] < 0:
                continue
            if not blk[row[1]].lower().startswith(DEFAULTS["lead_prefix"]):
                continue
            if nbs.iname(row[2]) != "guitar":
                continue
            sounding = info[1] + INSTRUMENTS["guitar"]["offset"]   # 原始写法的发声
            hit = borrow("guitar", sounding)
            if hit:
                row[2] = nbs.INSTR_ID[hit[0]]
                row[3] = hit[1]
                stats[f"主音高音改 {hit[0]}"] += 1
            else:
                stats["主音高音无音色可用"] += 1

    # ---- 4b. borrow by the instrument register table (default on) -----------
    if args.borrow_table:
        for row in notes:
            info = folded.get(id(row))
            if not info or row[1] < 0:
                continue
            _dir, orig, orig_inst = info
            if is_drum_block(blk[row[1]]):
                continue            # drums keep their own timbre
            if row[2] != orig_inst:
                continue            # an earlier step already re-timbered it
            src = nbs.iname(row[2])
            reg = INSTRUMENTS.get(src)
            if reg is None:      # drums / trumpet have no register row
                continue
            # 电钢琴（p4/p5）越界时先借 flute，flute 也写不下才借 bell
            prefer = ("flute", "bell") if (src == "harp" and prog.get(row[1]) in (4, 5)) else None
            hit = borrow(src, orig + reg["offset"], prefer)
            if not hit:
                continue
            tgt, key = hit
            row[2] = nbs.INSTR_ID[tgt]
            row[3] = fold(key)
            stats[f"借音色 {src}→{tgt}"] += 1

    # ---- 5. drop bit that collides with a lead guitar note ------------------
    if not args.no_clash:
        lead_keys = collections.defaultdict(set)
        for row in notes:
            if row[1] >= 0 and blk[row[1]].lower().startswith(DEFAULTS["lead_prefix"]) \
                    and nbs.iname(row[2]) == "guitar":
                lead_keys[row[0]].add(row[3])
        kept = []
        for row in notes:
            if nbs.iname(row[2]) == "bit" and row[3] in lead_keys.get(row[0], ()):
                stats["与主音撞音删除"] += 1
                continue
            kept.append(row)
        notes = kept

    # ---- 6. dedupe ----------------------------------------------------------
    if not args.no_dedupe:
        seen, kept = set(), []
        for row in notes:
            sig = (row[0], row[3], row[2])
            if sig in seen:
                continue
            seen.add(sig)
            kept.append(row)
        stats["去重删除"] = len(notes) - len(kept)
        notes = kept

    # ---- shift --------------------------------------------------------------
    shift = args.shift
    if not shift and args.align:
        ref = nbs.load(args.align)
        ref_rows = [(n.tick, n.key, nbs.iname(n.instrument))
                    for layer in ref.layers for n in layer.notes]
        shift = best_shift(notes, ref_rows)
        stats["对齐偏移"] = shift
    if shift:
        for row in notes:
            row[0] = max(0, row[0] + shift)

    # ---- 7. rebuild layers --------------------------------------------------
    per_layer, bell_notes = collections.defaultdict(list), []
    for tick, li, inst, key in notes:
        note = nbs.Note(tick=tick, layer=0, instrument=inst, key=key)
        if li < 0:
            bell_notes.append(note)
        else:
            per_layer[li].append(note)

    layers = []
    for li, layer in enumerate(song.layers):
        got = per_layer.get(li, [])
        if args.drop_empty and not got:
            continue
        layers.append(nbs.Layer(id=len(layers), name=layer.name, lock=layer.lock,
                                volume=layer.volume, panning=layer.panning, notes=got))

    if bell_notes:
        per_tick = collections.Counter(n.tick for n in bell_notes)
        width = max(per_tick.values())
        bins = [[] for _ in range(width)]
        seen = collections.Counter()
        for n in sorted(bell_notes, key=lambda x: x.tick):
            bins[seen[n.tick] % width].append(n)
            seen[n.tick] += 1
        bell_layers = [nbs.Layer(id=0, name=f"Bell{i + 1}", notes=ns)
                       for i, ns in enumerate(bins)]
        insert_at = 0
        for i, layer in enumerate(layers):
            if layer.lock != 1 and "vocal" in (layer.name or "").lower():
                insert_at = i + 1
        # keep one silent (separator) layer between the vocal block and the
        # generated bell layers, the same way the other blocks are separated
        silent = nbs.Layer(id=0, name="-- Bell --", lock=1, notes=[])
        layers = layers[:insert_at] + [silent] + bell_layers + layers[insert_at:]

    # ---- 7. tidy: drop empty regular layers, fold adjacent separators ---------
    # A separator is an empty locked layer (the `── NAME ──` markers).  Empty
    # layers that are NOT separators are leftovers and can go; two separators
    # in a row mean there is nothing between them, so keep only the first.
    if not args.no_tidy:
        def is_separator(layer):
            return bool(layer.lock) or (layer.name or "").strip().startswith(("──", "--"))

        kept_layers = []
        before = len(layers)
        for layer in layers:
            empty = not layer.notes
            if empty and not is_separator(layer):
                stats["删空轨"] += 1
                continue
            if empty and kept_layers and not kept_layers[-1].notes \
                    and is_separator(kept_layers[-1]):
                # a run of separators: keep the LAST one, it is the header of
                # the block that actually follows
                kept_layers[-1] = layer
                stats["合并分隔轨"] += 1
                continue
            kept_layers.append(layer)
        layers = kept_layers
        stats["层数"] = f"{before}→{len(layers)}"

    for i, layer in enumerate(layers):
        layer.id = i
        for n in layer.notes:
            n.layer = i

    song.layers = layers
    Path(args.dst).parent.mkdir(parents=True, exist_ok=True)
    nbs.save(song, args.dst, cp1252_safe=(args.strings == "cp1252"))

    # ---- report -------------------------------------------------------------
    inst = collections.Counter(nbs.iname(n.instrument) for L in layers for n in L.notes)
    keys = [n.key for L in layers for n in L.notes]
    if args.quiet:
        print(f"整理后：{len(keys)} 个音符 / {len(layers)} 条音轨")
    else:
        if stats:
            print("  改动：")
            parts = [f"{k} {v}" for k, v in stats.items()]
            lines: list[str] = []
            current = ""
            for part in parts:
                candidate = f"{current} · {part}" if current else part
                if current and len(candidate) > 72:
                    lines.append(current)
                    current = part
                else:
                    current = candidate
            if current:
                lines.append(current)
            for line in lines:
                print("    " + line)
        else:
            print("  改动：无")
        if keys:
            print(f"  产出：{len(keys)} 个音 / {len(layers)} 层 / key {min(keys)}–{max(keys)}")
        else:
            print(f"  产出：0 个音 / {len(layers)} 层")
        counts = " · ".join(f"{name} {n}" for name, n in inst.most_common(10))
        if len(inst) > 10:
            counts += " · …"
        print("  音色：" + counts)

    if args.compare:
        ref = nbs.load(args.compare)
        mine = {(n.tick, n.key, nbs.iname(n.instrument)) for L in layers for n in L.notes}
        theirs = {(n.tick, n.key, nbs.iname(n.instrument)) for L in ref.layers for n in L.notes}

        def score(a, b):
            inter = len(a & b)
            rec = inter / len(b) if b else 0.0
            prec = inter / len(a) if a else 0.0
            return rec, prec, (2 * rec * prec / (rec + prec) if rec + prec else 0.0)

        rec, prec, f1 = score(mine, theirs)
        print(f"  与参考谱 {Path(args.compare).name} 对比（全部音）：命中率 {100*rec:.1f}% / "
              f"准确率 {100*prec:.1f}% / F1 {100*f1:.1f}%")
        # 鼓是确定性的（MIDI → 槽位表），参考里一抄就 100%，会盖住旋律/音色的真实差异，
        # 所以再打一行去掉鼓的，比较时以这一行为准。
        drums = {"sdrum", "bdrum", "clicks"}
        rec2, prec2, f1_2 = score({x for x in mine if x[2] not in drums},
                                  {x for x in theirs if x[2] not in drums})
        print(f"  与参考谱对比（不含鼓）：命中率 {100*rec2:.1f}% / 准确率 "
              f"{100*prec2:.1f}% / F1 {100*f1_2:.1f}%")


if __name__ == "__main__":
    main()

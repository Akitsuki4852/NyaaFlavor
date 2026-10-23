"""
NBS (NoteBlockStudio) binary format reader/writer  --  verified implementation.

Format (from https://opennbs.gitbook.io/open-note-block-studio/nbs-format):

  u16 song_length_or_0
      If 0  -> "new" (v1+) format, next byte is the version.
                        (The historical ASCII magic "NBS" was replaced by 0x0000.)
  u8  version
  u8  vanilla_instrument_count          (20 in modern OpenNBS, 16 in older files)
  u16 song_length                       (v>=3)
  u16 layer_count
  string song_name, song_author, original_author, description
  u16 tempo (ticks per second * 100)
  u8  auto_save, u8 auto_save_duration, u8 time_signature
  5 x i32 stats   (minutes spent, left clicks, right clicks, blocks added, blocks removed)
  string song_origin  (MIDI filename / path)
  u8 loop, u8 max_loop_count, u16 loop_start        (v>=4)
  --- notes ---
  repeat:  u16 jump_ticks ; then repeat { u16 jump_layers ; u8 instrument, key,
                                             [v>=4: velocity, panning+100, i16 pitch] }
                                       until jump_layers == 0
           until jump_ticks == 0
  --- layers ---
  layer_count x { string name, [v>=4: u8 lock], u8 volume, [v>=2: u8 panning+100] }
  --- custom instruments ---
  u8 count x { string name, string sound_file, u8 pitch, u8 press_key }

Note: **both** the tick jump and the layer index are written as *deltas*.
"""
from __future__ import annotations
import io, struct, os
from dataclasses import dataclass, field
from typing import List, Optional
from collections import Counter

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# OpenNBS v5+ instrument list (20 entries; 0..15 are the classic 16)
INSTRUMENTS = [
    "harp",          # 0  piano / harp (any block)
    "dbass",         # 1  bass (wood)
    "bdrum",         # 2  bass drum (stone)
    "sdrum",         # 3  snare (sand)
    "clicks",        # 4  click / hat (glass)
    "guitar",        # 5  guitar (wool)
    "flute",         # 6  flute (clay)
    "bell",          # 7  bell (gold)
    "icechime",      # 8  chime (packed ice)
    "xylophone",     # 9  xylophone (bone)
    "iron_xyl",      # 10 iron xylophone (iron)
    "cow_bell",      # 11 cow bell (soul sand)
    "didgeridoo",    # 12 didgeridoo (pumpkin)
    "bit",           # 13 bit / square (emerald)
    "banjo",         # 14 banjo (hay)
    "pling",         # 15 pling (glowstone)
    "trumpet",       # 16 (1.19+)
    "trumpet_exposed",
    "trumpet_weathered",
    "trumpet_oxidized",
]

INSTR_CN = {
    "harp": "钢琴", "dbass": "贝斯", "bdrum": "底鼓", "sdrum": "军鼓",
    "clicks": "击", "guitar": "吉他", "flute": "笛", "bell": "钟琴",
    "icechime": "风铃", "xylophone": "木琴", "iron_xyl": "铁琴",
    "cow_bell": "牛铃", "didgeridoo": "管", "bit": "方波",
    "banjo": "班卓", "pling": "琵琶",
}

INSTR_ID = {name: i for i, name in enumerate(INSTRUMENTS)}

# Minecraft note block playable range: F#3 .. F#5  ->  NBS key 0..24 (key 0 = F#3 = MIDI 54)
KEY_MIN, KEY_MAX = 0, 24


def iname(i: int) -> str:
    return INSTRUMENTS[i] if 0 <= i < len(INSTRUMENTS) else f"custom{i}"


def keyname(key: int) -> str:
    """MC pitch name of an NBS key.

    OpenNBS's own convention: key 0 == F#3 == MIDI 54, so key k == MIDI 54+k.
    (So key 33..57 == MIDI 87..111 == D#6..D#8 in OpenNBS's *display* frame.)

    This project instead writes the same 25 keys at 33..57 and describes that
    window as "F#..F#" = F#3..F#5, i.e. it reads key 33 as F#3 (key = midi - 21).
    Both are the same 25 keys; only the octave label differs.  `keyname` follows
    OpenNBS; use `user_keyname` for the key 33 == F#3 frame.
    """
    m = 54 + key
    return f"{NOTE_NAMES[m % 12]}{m // 12 - 1}"


def user_keyname(key: int) -> str:
    """Pitch name in the project frame, where key 33 == F#3 (midi = key + 21)."""
    m = key + 21
    return f"{NOTE_NAMES[m % 12]}{m // 12 - 1}"


def midi_of_key(key: int) -> int:
    """NBS key 0 == F#3 == MIDI 54."""
    return 54 + key


def key_of_midi(midi: int) -> int:
    return midi - 54


@dataclass
class Note:
    tick: int
    layer: int
    instrument: int
    key: int
    velocity: int = 100
    panning: int = 100   # NBS raw value; 100 == centre
    pitch: int = 0       # centi-semitones


@dataclass
class Layer:
    id: int = 0
    name: str = ""
    lock: int = 0        # 1 == muted
    volume: int = 100
    panning: int = 100
    notes: List[Note] = field(default_factory=list)

    @property
    def muted(self) -> bool:
        return self.lock == 1


@dataclass
class Song:
    version: int = 5
    vanilla_instrument_count: int = 16
    song_length: int = 0
    tempo: int = 1000
    name: str = ""
    author: str = ""
    orig_author: str = ""
    description: str = ""
    time_signature: int = 4
    midi_filename: str = ""
    loop: int = 0
    loop_start: int = 0
    custom_instruments: list = field(default_factory=list)
    layers: List[Layer] = field(default_factory=list)
    path: str = ""
    has_magic: bool = False

    @property
    def tps(self) -> float:
        return self.tempo / 100.0

    def tick_to_sec(self, tick: float) -> float:
        return tick / self.tps

    def sec_to_tick(self, sec: float) -> float:
        return sec * self.tps

    def all_notes(self):
        for li, L in enumerate(self.layers):
            for n in L.notes:
                yield li, L, n

    def duration_ticks(self) -> int:
        m = 0
        for _, _, n in self.all_notes():
            m = max(m, n.tick)
        return max(m, self.song_length)

    def tempo_map(self):
        """Return [(tick, tps)] from custom "Tempo Changer" notes (instrument 16+),
        falling back to the single header tempo."""
        pts = [(0, self.tps)]
        for li, L, n in self.all_notes():
            if n.instrument >= self.vanilla_instrument_count:
                pass
        # OpenNBS tempo changers use a custom instrument whose *name* is "Tempo Changer"
        names = {ci["name"]: ci for ci in self.custom_instruments}
        id2name = {}
        for k, ci in enumerate(self.custom_instruments):
            id2name[self.vanilla_instrument_count + k] = ci["name"]
        ev = []
        for li, L, n in self.all_notes():
            nm = id2name.get(n.instrument, "")
            if "Tempo" in nm or "tempo" in nm:
                ev.append((n.tick, n.pitch / 100.0 if n.pitch else 0.0))
        unsupported = bool(ev)
        return pts, unsupported

    def instrument_of_layer(self, li: int) -> Optional[str]:
        c = Counter(n.instrument for n in self.layers[li].notes)
        if not c:
            return None
        return iname(c.most_common(1)[0][0])


# ---------------------------------------------------------------- reading

def _dec(b: bytes) -> str:
    """OpenNBS writes UTF-8; very old files/editors used cp1252. Try both."""
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("cp1252", errors="replace")


def ascii_safe(s: str) -> str:
    """Best-effort ASCII for the NBS reader.

    NBS 4's own audio-export script decodes every string as cp1252 and raises on
    bytes it does not know (0x81/0x8d/0x8f/0x90/0x9d), so a UTF-8 Chinese title
    makes export fail.  Chinese is therefore romanised, everything else outside
    cp1252 becomes '?'.
    """
    try:
        s.encode("cp1252")
        return s
    except UnicodeEncodeError:
        pass
    try:
        from pypinyin import lazy_pinyin
    except Exception:
        lazy_pinyin = None
    out = []
    for ch in s:
        try:
            ch.encode("cp1252")
            out.append(ch)
            continue
        except UnicodeEncodeError:
            pass
        if "\u4e00" <= ch <= "\u9fff" and lazy_pinyin is not None:
            out.append(lazy_pinyin(ch)[0])
        elif ch in "\u2014\u2013\u2500":
            out.append("-")
        elif ch in "\uff08\uff09":
            out.append("(" if ch == "\uff08" else ")")
        elif ch in "\u00b7\u30fb":
            out.append("-")
        else:
            out.append("?")
    return "".join(out)


def _enc(s: str, cp1252_safe: bool = False) -> bytes:
    if cp1252_safe:
        return ascii_safe(s).encode("cp1252", "replace")
    try:
        return s.encode("utf-8")
    except Exception:
        return s.encode("cp1252", "replace")


def _rs(raw: bytes, o: int):
    n = struct.unpack("<i", raw[o:o + 4])[0]
    if n < 0 or n > 1 << 20:
        raise ValueError(f"bad string length {n} at {o}")
    return _dec(raw[o + 4:o + 4 + n]), o + 4 + n


def _probe_header(raw: bytes, o: int):
    """Return (score, info) if a plausible NBS header starts at byte o.

    Layout: u16 marker(0) | u8 version | u8 vanilla_instrument_count |
            u16 song_length (v>=3) | u16 layer_count | 4 strings | ...
    """
    try:
        p = o
        first = struct.unpack("<H", raw[p:p + 2])[0]; p += 2
        if first != 0:
            return None                      # only the modern 0x0000 format
        ver = raw[p]; p += 1
        vi = raw[p]; p += 1
        if ver not in (1, 2, 3, 4, 5, 6) or not (1 <= vi <= 64):
            return None
        song_length = struct.unpack("<H", raw[p:p + 2])[0] if ver >= 3 else first
        if ver >= 3:
            p += 2
        nlay = struct.unpack("<H", raw[p:p + 2])[0]; p += 2
        if not (0 < nlay <= 4096):
            return None
        names = []
        for _ in range(4):
            s, p = _rs(raw, p)
            names.append(s)
        tempo = struct.unpack("<H", raw[p:p + 2])[0]
        if not (50 <= tempo <= 20000):
            return None
        p += 2
        p += 3                                   # auto_save, duration, timesig
        ts = raw[p - 1]
        p += 20                                  # 5 x i32 stats
        origin, p = _rs(raw, p)
        score = 0
        if ver >= 4 and p + 4 > len(raw):
            return None
        if names[0]:
            score += 4
        if names[1]:
            score += 4
        if 1 <= ts <= 16:
            score += 2
        if vi in (10, 16, 20):
            score += 2
        if origin:
            score += 1
        return score, dict(offset=o, ver=ver, vi=vi, song_length=song_length,
                           nlay=nlay, names=names, tempo=tempo)
    except Exception:
        return None


def detect_header(raw: bytes):
    cands = [c for c in (_probe_header(raw, 0), _probe_header(raw, 2),
                         _probe_header(raw, 3)) if c]
    if not cands:
        return None
    cands.sort(key=lambda x: -x[0])
    return cands[0][1]


def load(path: str) -> Song:
    with open(path, "rb") as fh:
        raw = fh.read()
    info = detect_header(raw)
    if info is None:
        raise ValueError(f"not an NBS file: {path}")
    s = Song(path=path)
    s.version = info["ver"]
    s.vanilla_instrument_count = info["vi"]
    s.has_magic = (raw[:3] == b"NBS")
    o = info["offset"] + 2 + 1 + 1                # past marker + ver + vi
    if s.version >= 3:
        s.song_length = struct.unpack("<H", raw[o:o + 2])[0]; o += 2
    nlay = struct.unpack("<H", raw[o:o + 2])[0]; o += 2
    s.name, o = _rs(raw, o)
    s.author, o = _rs(raw, o)
    s.orig_author, o = _rs(raw, o)
    s.description, o = _rs(raw, o)
    s.tempo = struct.unpack("<H", raw[o:o + 2])[0]; o += 2
    o += 3
    s.time_signature = raw[o - 1]
    o += 20
    s.midi_filename, o = _rs(raw, o)
    if s.version >= 4:
        s.loop = raw[o]; o += 1
        o += 1
        s.loop_start = struct.unpack("<H", raw[o:o + 2])[0]; o += 2

    # ---- notes (tick and layer are both delta encoded) ----
    s.layers = [Layer(id=i) for i in range(nlay)]
    p = o
    tick = -1
    while True:
        if p + 2 > len(raw):
            break
        jt = struct.unpack("<h", raw[p:p + 2])[0]; p += 2
        if jt == 0:
            break
        tick += jt
        layer = -1
        while True:
            if p + 2 > len(raw):
                p = len(raw); break
            jl = struct.unpack("<h", raw[p:p + 2])[0]; p += 2
            if jl == 0:
                break
            layer += jl
            inst = raw[p]; key = raw[p + 1]; p += 2
            vel, pan, fp = 100, 100, 0
            if s.version >= 4:
                vel = raw[p]; pan = raw[p + 1]
                fp = struct.unpack("<h", raw[p + 2:p + 4])[0]
                p += 4
            if 0 <= layer < nlay:
                s.layers[layer].notes.append(
                    Note(tick, layer, inst, key, vel, pan, fp))
        if p >= len(raw):
            break

    # ---- layers ----
    try:
        for L in s.layers:
            L.name, p = _rs(raw, p)
            if s.version >= 4:
                L.lock = raw[p]; p += 1
            L.volume = raw[p]; p += 1
            if s.version >= 2:
                L.panning = raw[p]; p += 1
    except Exception:
        pass
    # ---- custom instruments ----
    try:
        nci = raw[p]; p += 1
        for _ in range(nci):
            nm, p = _rs(raw, p)
            fn, p = _rs(raw, p)
            pit = raw[p]; pk = raw[p + 1]; p += 2
            s.custom_instruments.append(
                {"name": nm, "file": fn, "pitch": pit, "press_key": pk})
    except Exception:
        pass
    return s


# ---------------------------------------------------------------- writing

def save(s: Song, path: str, version: Optional[int] = None, cp1252_safe: bool = False):
    v = version if version is not None else s.version
    f = io.BytesIO()
    W = lambda fmt, val: f.write(struct.pack(fmt, val))
    # NBS strings are UTF-8 (OpenNBS itself is Java/UTF-8).  Encoding as cp1252
    # silently turned every Chinese song / layer name into "???".
    SW = lambda txt: (W("<i", len(_enc(txt, cp1252_safe))),
                      f.write(_enc(txt, cp1252_safe)))

    W("<H", 0)                     # modern-format marker (was "NBS")
    W("<B", v)
    W("<B", s.vanilla_instrument_count)
    dur = max(s.song_length, s.duration_ticks() + 1)
    if v >= 3:
        W("<H", min(dur, 65535))
    W("<H", len(s.layers))
    for t in (s.name, s.author, s.orig_author, s.description):
        SW(t)
    W("<H", s.tempo)
    W("<B", 0); W("<B", 10); W("<B", s.time_signature)
    for _ in range(5):
        W("<i", 0)
    SW(s.midi_filename)
    if v >= 4:
        W("<B", s.loop); W("<B", 0); W("<H", s.loop_start)

    by_tick = {}
    for li, L in enumerate(s.layers):
        if L.lock == 1:
            continue                      # muted layers carry no notes
        for n in L.notes:
            by_tick.setdefault(n.tick, []).append(n)
    last_tick = -1
    for t in sorted(by_tick):
        jt = t - last_tick
        if not (0 < jt <= 32767):
            raise ValueError(f"tick order broken at {t}")
        W("<h", jt)
        last_layer = -1
        for n in sorted(by_tick[t], key=lambda x: x.layer):
            jl = n.layer - last_layer
            if not (0 < jl <= 32767):
                raise ValueError(f"layer order broken at tick {t}")
            W("<h", jl)
            W("<B", n.instrument); W("<B", n.key)
            if v >= 4:
                W("<B", n.velocity); W("<B", n.panning)
                W("<h", n.pitch)
            last_layer = n.layer
        W("<h", 0)
        last_tick = t
    W("<h", 0)
    for L in s.layers:
        SW(L.name)
        if v >= 4:
            W("<B", L.lock)
        W("<B", L.volume)
        if v >= 2:
            W("<B", L.panning)
    W("<B", len(s.custom_instruments))
    for ci in s.custom_instruments:
        SW(ci["name"]); SW(ci["file"])
        W("<B", ci["pitch"]); W("<B", ci["press_key"])
    with open(path, "wb") as fh:
        fh.write(f.getvalue())


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        s = load(path)
        print(f"=== {os.path.basename(path)}  v{s.version} tempo={s.tps}tps "
              f"len={s.duration_ticks()}t ({s.tick_to_sec(s.duration_ticks()):.1f}s) "
              f"layers={len(s.layers)}")
        print(f"    name={s.name!r} author={s.author!r} orig={s.orig_author!r}")
        for i, L in enumerate(s.layers):
            c = Counter(n.instrument for n in L.notes)
            desc = ", ".join(f"{iname(k)}:{v}" for k, v in c.most_common())
            ks = [n.key for n in L.notes]
            kr = f"{min(ks)}-{max(ks)}" if ks else "-"
            print(f"    [{i:2d}] lock={L.lock} vol={L.volume:3d} pan={L.panning:3d} "
                  f"n={len(L.notes):5d} key={kr:>6} name={L.name!r} {desc}")

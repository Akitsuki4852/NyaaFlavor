#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
midi2nbs.py —— MIDI(.mid) → OpenNBS(.nbs) 转换器（Notebot 用）

硬性要求（本脚本就是照这几条写的）
----------------------------------
1. **每个 MIDI 音轨独占一个「层块」**，层块之间插一条 **静音空层**（lock=1），
   所以任何两个音轨在 NBS 里都不会混进同一层。
2. **鼓（GM 通道 10）按「套鼓槽位」并层**：同一槽位的多个 GM 音高并进同一个层块，
   再按槽位的固定 key 写（`--print-maps` 打印当前生效的表，`STYLE.md` §A6 是同一份）：
       底鼓 35/36        → bdrum  @ C4    (key 39)
       边击·拍手 37/39   → clicks @ G#3   (key 35)
       军鼓 38/40        → sdrum  @ A#3   (key 37)
       通鼓 41/45        → bdrum  @ D4    (key 41)
            43/47        → bdrum  @ E4    (key 43)
            48/50        → bdrum  @ A4    (key 48)
       闭镲·踩镲 42/44   → sdrum  @ E5    (key 55)
       骑镲·中国镲·镲帽·铃鼓 51–54 → 并进 HH-C（同上）
       开镲 46           → sdrum  @ C5    (key 51)
       水镲 55           → sdrum  @ A4    (key 48)
       吊镲 49/57        → sdrum  @ C5    (key 51)，一次触发写 3 连击（t / t+1 / t+2）
   表外的 GM 打击乐默认按 u3002 / OpenNBS 表单独成一条 `GM<n>` 层（`--no-drum-fallback` 退回军鼓）；
   **每个槽位用独立层块，不复用别的层的音**。
3. 同一 MIDI 音轨里若有多个 program，每个 program 也各自成块（不同音色不混）。
4. 音高：`key = MIDI − 21 + 12 × octaveModifier`
   （`key 33 = F#3 = 185 Hz`，已用 OpenNBS 渲染核对；默认表里的
   octaveModifier 是**乐器自身的八度差**——吉他天然低一个八度、贝斯低两个八度、
   笛子高一个八度、钟琴高两个八度——补偿后每个声部的**实际发声音高 = MIDI 写的音高**）。
   **不做八度折叠**（不往可播放窗口 33..57 里压）；出界音原样保留，交给 ③ 处理。
   想得到纯 `key = MIDI − 21` 的裸映射，加 `--no-octave-modifier`。

数据结构说明
------------
NBS 的 `layer` 就是“音轨”。格式上一个 `(tick, layer)` 只能有一个音
（layer 增量 0 是“本 tick 结束”的哨兵），所以一个和弦必须摊到多条 layer 上。
本脚本的做法是：**每个层块内的 layer 数 = 该块的最大同时发声音数**，
同一 tick 的音按音高从低到高依次放进 0..n-1 层——低音永远在靠上的层，
换和弦时层号不会乱跳。

用法
----
    python midi2nbs.py input.mid
    python midi2nbs.py input.mid -o out.nbs --grid 8 --title 迷星叫 --author 某人
    python midi2nbs.py input.mid --dry-run          # 只统计不落盘
    python midi2nbs.py input.mid --print-maps       # 打印生效的映射表

依赖：仓库里的 `_agent/smf.py`（容错 SMF 解析器）与 `_agent/nbs.py`（回读校验）。
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import unicodedata
from bisect import bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# 0. 复用仓库里已经验证过的两个库
# ---------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent
for _cand in (HERE / "_agent", HERE.parent / "_agent"):
    if (_cand / "smf.py").exists():
        sys.path.insert(0, str(_cand))
        break
else:  # pragma: no cover
    raise SystemExit("找不到 _agent/smf.py（本脚本复用仓库里的 MIDI 解析器）")

import smf  # noqa: E402
import nbs  # noqa: E402  （只在最后回读校验时用）

# 可播放窗口：25 个音，F#..F#，key = MIDI − 21
KEY_LO, KEY_HI = 33, 57
MIDI_BASE = 21               # key = MIDI − 21  <=>  key 33 == MIDI 54 == F#3
KEY_MIN_BYTE, KEY_MAX_BYTE = 0, 87   # NBS 里 key 是 u8，且 OpenNBS 只认 0..87

# 名次（层序）：人声 → 主音 → 节奏 → 贝斯 → 鼓 → 其他
ROLE_VOCAL, ROLE_LEAD, ROLE_RHYTHM, ROLE_BASS, ROLE_DRUM, ROLE_OTHER = range(6)

# ---------------------------------------------------------------------------
# 1. 默认映射表（两套，来源都写在注释里）
# ---------------------------------------------------------------------------
# GM → Note Block 映射。数据来源：u3002 的 MIT 映射表（OpenNBS 的
# scripts/midi_instruments/midi_instruments.gml 内置同一张表，OpenNBS 也是 MIT）。
# 第二个数是 octaveModifier（音色自身八度差，默认启用；--no-octave-modifier 关闭）。
# GM_ 前缀表示 General MIDI；表本身来自 u3002 / OpenNBS。
# 相对 u3002/OpenNBS 只有一处本地调整：GM29 Overdriven Guitar 保留 Guitar +1。
# 程序号 → (音色, octaveModifier)
GM_INSTRUMENT_MAPPINGS: Dict[int, Tuple[str, int]] = {
    0: ('harp', 0), 1: ('pling', 0), 2: ('pling', 0), 3: ('pling', 0), 4: ('harp', 0), 5: ('harp', 0),
    6: ('guitar', 1), 7: ('banjo', 0), 8: ('bell', -2), 9: ('bell', -2), 10: ('bell', -2),
    11: ('iron_xylophone', 0), 12: ('iron_xylophone', 0), 13: ('xylophone', -2), 14: ('bell', -2),
    15: ('guitar', 1), 16: ('flute', -1), 17: ('iron_xylophone', 0), 18: ('flute', -1), 19: ('flute', -1),
    20: ('flute', -1), 21: ('flute', -1), 22: ('trumpet', 0), 23: ('flute', -1), 24: ('guitar', 1),
    25: ('guitar', 1), 26: ('harp', 0), 27: ('guitar', 1), 28: ('bass', 2), 29: ('guitar', 1),
    30: ('guitar', 1), 31: ('guitar', 3), 32: ('bass', 2), 33: ('bass', 2), 34: ('bass', 2),
    35: ('bass', 2), 36: ('guitar', 1), 37: ('guitar', 1), 38: ('bass', 2), 39: ('pling', 0),
    40: ('flute', -1), 41: ('flute', -1), 42: ('flute', -1), 43: ('didgeridoo', 2), 44: ('flute', -1),
    45: ('bass', 2), 46: ('harp', 0), 47: ('snare', 0), 48: ('flute', -1), 49: ('flute', -1),
    50: ('flute', -1), 51: ('flute', -1), 52: ('flute', -1), 53: ('flute', -1), 54: ('flute', -1),
    55: ('snare', 0), 56: ('trumpet', 0), 57: ('trumpet_exposed', 0), 58: ('trumpet_oxidized', 2),
    59: ('trumpet_weathered', 1), 60: ('trumpet_exposed', 0), 61: ('trumpet_exposed', 0),
    62: ('trumpet_exposed', 0), 63: ('trumpet', 0), 64: ('flute', -1), 65: ('trumpet', 0),
    66: ('trumpet', 0), 67: ('trumpet_exposed', 0), 68: ('flute', -1), 69: ('flute', -1),
    70: ('flute', -1), 71: ('flute', -1), 72: ('flute', -1), 73: ('flute', -1), 74: ('flute', -1),
    75: ('flute', -1), 76: ('flute', -1), 77: ('flute', -1), 78: ('flute', -1), 79: ('flute', -1),
    80: ('bit', 0), 81: ('flute', -1), 82: ('flute', -1), 83: ('flute', -1), 84: ('guitar', 1),
    85: ('flute', -1), 86: ('flute', -1), 87: ('bass', 2), 88: ('bell', -2), 89: ('flute', -1),
    90: ('flute', -1), 91: ('flute', -1), 92: ('flute', -1), 93: ('flute', -1), 94: ('flute', -1),
    95: ('chime', -2), 96: ('chime', -2), 97: ('flute', -1), 98: ('chime', -2), 99: ('guitar', 1),
    100: ('pling', 0), 101: ('flute', -1), 102: ('flute', -1), 103: ('guitar', 1), 104: ('banjo', 0),
    105: ('banjo', 0), 106: ('banjo', 0), 107: ('guitar', 1), 108: ('iron_xylophone', 0),
    109: ('flute', -1), 110: ('flute', -1), 111: ('flute', -1), 112: ('chime', -2),
    113: ('cow_bell', -1), 114: ('iron_xylophone', 0), 115: ('xylophone', -2), 116: ('bass_drum', 0),
    117: ('bass_drum', 0), 118: ('bass_drum', 0), 119: ('snare', 0), 120: ('hat', 1),
    121: ('flute', -1), 122: ('chime', -2), 123: ('flute', 1), 124: ('bell', 2), 125: ('bass_drum', 0),
    126: ('snare', 0), 127: ('snare', 0),
}

# 同一张表的打击乐部分：GM 音高 → (音色, NBS key)。只列 24..88（GM 实际用到的区间）。
GM_PERCUSSION_MAPPINGS: Dict[int, Tuple[str, int]] = {
    24: ('snare', 12), 25: ('snare', 5), 26: ('hat', 18), 27: ('snare', 2), 28: ('snare', 9),
    29: ('hat', 6), 30: ('hat', 2), 31: ('hat', 13), 32: ('hat', 9), 33: ('hat', 15), 34: ('chime', 18),
    35: ('bass_drum', 4), 36: ('bass_drum', 8), 37: ('hat', 8), 38: ('snare', 15), 39: ('snare', 19),
    40: ('snare', 16), 41: ('bass_drum', 6), 42: ('hat', 21), 43: ('bass_drum', 9), 44: ('hat', 23),
    45: ('bass_drum', 14), 46: ('snare', 22), 47: ('bass_drum', 17), 48: ('bass_drum', 20),
    49: ('snare', 20), 50: ('bass_drum', 23), 51: ('snare', 17), 52: ('snare', 14), 53: ('bell', 17),
    54: ('snare', 23), 55: ('snare', 18), 56: ('cow_bell', 6), 57: ('snare', 21), 58: ('hat', 17),
    59: ('snare', 24), 60: ('cow_bell', 16), 61: ('cow_bell', 9), 62: ('hat', -3), 63: ('cow_bell', -1),
    64: ('cow_bell', -9), 65: ('cow_bell', 5), 66: ('cow_bell', -4), 67: ('xylophone', 12),
    68: ('xylophone', 5), 69: ('hat', 35), 70: ('hat', 32), 71: ('flute', 34), 72: ('flute', 33),
    73: ('hat', 19), 74: ('hat', 20), 75: ('xylophone', 19), 76: ('xylophone', 7), 77: ('xylophone', 1),
    78: ('didgeridoo', 22), 79: ('didgeridoo', 13), 80: ('bell', 19), 81: ('chime', 19),
    82: ('hat', 36), 83: ('bell', 21), 84: ('chime', 17), 85: ('xylophone', 15), 86: ('bass_drum', 12),
    87: ('bass_drum', 7), 88: ('snare', 10),
}

# 映射里的音色名 → NBS 音色 id 名（nbs.INSTRUMENTS 里的写法）
GM_TO_NBS = {
    'harp': 'harp', 'bass': 'dbass', 'bass_drum': 'bdrum', 'snare': 'sdrum',
    'hat': 'clicks', 'guitar': 'guitar', 'flute': 'flute', 'bell': 'bell',
    'chime': 'icechime', 'xylophone': 'xylophone', 'iron_xylophone': 'iron_xyl',
    'cow_bell': 'cow_bell', 'didgeridoo': 'didgeridoo', 'bit': 'bit',
    'banjo': 'banjo', 'pling': 'pling', 'trumpet': 'trumpet',
    'trumpet_exposed': 'trumpet_exposed', 'trumpet_weathered': 'trumpet_weathered',
    'trumpet_oxidized': 'trumpet_oxidized',
}

# ---------------------------------------------------------------------------
# 4. 鼓组：套鼓槽位
#
# 生成结果里的鼓组改成了一套"套鼓"：同一件鼓只占一条 layer，
# 不按 GM 音高一条一条铺；把几件"小镲"并进闭镲、把两条吊镲并成一条。
#   * KIT 的**列表顺序就是 layer 从上到下的顺序**；
#   * times/step 决定一次触发写几个音：
#       times=1                  → 每个触发 1 个音
#       times=3, step=1          → t, t+1, t+2 三连击（吊镲）
#       times=3, step=0          → 同一 tick 叠 3 颗（靠"密度即音量"加响）
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class DrumSlot:
    slot: str            # 槽位 id（也是 layer 名）
    instrument: str      # NBS 音色
    key: int             # NBS key
    times: int = 1       # 每次触发写几个音
    step: int = 0        # 0 = 同一 tick 叠；n = 每隔 n tick 放一个


KIT: Tuple[DrumSlot, ...] = (
    DrumSlot('HH-C',   'sdrum',  55),                   # 闭镲（骑镲/中国镲/镲帽/铃鼓都并进来）
    DrumSlot('CRASH',  'sdrum',  51, times=3, step=1),  # 吊镲：3 连击
    DrumSlot('HH-O',   'sdrum',  51),                   # 开镲
    DrumSlot('SPLASH', 'sdrum',  48),                   # 水镲
    DrumSlot('SNARE',  'sdrum',  37),                   # 军鼓
    DrumSlot('TOM-L',  'bdrum',  41),                   # 低嗵
    DrumSlot('TOM-M',  'bdrum',  43),                   # 中嗵
    DrumSlot('TOM-H',  'bdrum',  48),                   # 高嗵
    DrumSlot('BD',     'bdrum',  39),                   # 底鼓
    DrumSlot('STICK',  'clicks', 35),                   # 边击 / 拍手
)

SLOT_BY_ID: Dict[str, DrumSlot] = {s.slot: s for s in KIT}

# GM 打击乐 → 槽位
GM_TO_SLOT: Dict[int, str] = {
    35: 'BD', 36: 'BD',
    37: 'STICK', 39: 'STICK',
    38: 'SNARE', 40: 'SNARE',
    41: 'TOM-L', 45: 'TOM-L',          # 落地嗵 / 低嗵
    43: 'TOM-M', 47: 'TOM-M',          # 高落地嗵 / 中低嗵
    48: 'TOM-H', 50: 'TOM-H',          # 中高嗵 / 高嗵
    42: 'HH-C', 44: 'HH-C',            # 闭镲 / 踩镲
    51: 'HH-C', 52: 'HH-C', 53: 'HH-C', 54: 'HH-C',   # 骑镲 / 中国镲 / 镲帽 / 铃鼓
    46: 'HH-O',                        # 开镲
    49: 'CRASH', 57: 'CRASH',          # 两条吊镲并成一条
    55: 'SPLASH',                      # 水镲
}

# GM 打击乐的名字，只用来做提示/报错
GM_DRUM_NAMES = {
    35: 'Acoustic Bass Drum', 36: 'Bass Drum 1', 37: 'Side Stick', 38: 'Acoustic Snare',
    39: 'Hand Clap', 40: 'Electric Snare', 41: 'Low Floor Tom', 42: 'Closed Hi-Hat',
    43: 'High Floor Tom', 44: 'Pedal Hi-Hat', 45: 'Low Tom', 46: 'Open Hi-Hat',
    47: 'Low-Mid Tom', 48: 'Hi-Mid Tom', 49: 'Crash Cymbal 1', 50: 'High Tom',
    51: 'Ride Cymbal 1', 52: 'Chinese Cymbal', 53: 'Ride Bell', 54: 'Tambourine',
    55: 'Splash Cymbal', 56: 'Cowbell', 57: 'Crash Cymbal 2', 58: 'Vibraslap',
    59: 'Ride Cymbal 2', 60: 'Hi Bongo', 61: 'Low Bongo', 62: 'Mute Hi Conga',
    63: 'Open Hi Conga', 64: 'Low Conga', 65: 'High Timbale', 66: 'Low Timbale',
    67: 'High Agogo', 68: 'Low Agogo', 69: 'Cabasa', 70: 'Maracas', 71: 'Short Whistle',
    72: 'Long Whistle', 73: 'Short Guiro', 74: 'Long Guiro', 75: 'Claves', 76: 'Hi Wood Block',
    77: 'Low Wood Block', 78: 'Mute Cuica', 79: 'Open Cuica', 80: 'Mute Triangle',
    81: 'Open Triangle', 82: 'Shaker', 83: 'Jingle Bell', 84: 'Belltree', 85: 'Castanets',
    86: 'Mute Surdo', 87: 'Open Surdo', 88: 'Applause',
}

# ---------------------------------------------------------------------------
# 2. 小工具
# ---------------------------------------------------------------------------

def in_window(key: int) -> bool:
    return KEY_LO <= key <= KEY_HI


def clamp_byte(key: int) -> int:
    return max(KEY_MIN_BYTE, min(KEY_MAX_BYTE, int(key)))


def midi_name(p: int) -> str:
    names = nbs.NOTE_NAMES
    return f"{names[p % 12]}{p // 12 - 1}"


def key_name(k: int) -> str:
    """key 用作者参照系命名（key = MIDI − 21）。"""
    return midi_name(k + MIDI_BASE)


@dataclass
class Part:
    """一个层块的原料。"""
    title: str
    role: int
    order: int                       # 同一 role 内的稳定排序
    instrument: str
    kind: str                        # 'pitched' | 'drum'
    notes: List[Tuple[int, int]]     # (midi_tick, midi_pitch) —— 铺层前才换成 NBS tick
    stack: int = 1                   # 每次触发叠几颗（鼓用）
    meta: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# 2.5 调性（给 glissando 用）
# ---------------------------------------------------------------------------

PC_NAMES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
PITCH_CLASS_NAME = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
PITCH_CLASS_FLAT = ['C', 'D♭', 'D', 'E♭', 'E', 'F', 'G♭', 'G', 'A♭', 'A', 'B♭', 'B']


def disp_width(text: str) -> int:
    """字符串的显示宽度：中日韩全角字符占 2 列。用于对齐表格。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1 for ch in text)


def ljust(text: str, width: int) -> str:
    return text + ' ' * max(0, width - disp_width(text))


def rjust(text: str, width: int) -> str:
    return ' ' * max(0, width - disp_width(text)) + text

# Krumhansl-Kessler 调性轮廓（无调号时用来估调）
_KK_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
_KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]
_MAJOR_STEPS = (0, 2, 4, 5, 7, 9, 11)
_MINOR_STEPS = (0, 2, 3, 5, 7, 8, 10)


def parse_key_name(text: str) -> Tuple[int, str]:
    """'Ab' / 'C#m' / 'F minor' -> (根音 pitch class, 'major'|'minor')"""
    s = text.strip().replace('♭', 'b').replace('♯', '#')
    mode = 'minor' if ('m' in s.lower().replace('major', '')) else 'major'
    letters = ''.join(ch for ch in s if ch.isalpha() and ch.lower() in 'abcdefg')
    if not letters:
        raise ValueError(f'看不懂的调名：{text}')
    root = letters[0].upper()
    pc = PC_NAMES[root]
    rest = s[s.index(letters[0]) + 1:]
    if rest.startswith('#'): pc += 1
    elif rest.startswith('b'): pc -= 1
    return pc % 12, mode


def scale_pcs(root: int, mode: str) -> set:
    steps = _MINOR_STEPS if mode == 'minor' else _MAJOR_STEPS
    return {(root + s) % 12 for s in steps}


def key_from_signature(sf: int, mi: int) -> Tuple[int, str]:
    """MIDI 调号 → (root pitch class, mode)。sf = 升号数（负数=降号），mi = 0 大调 / 1 小调。"""
    major_root = (7 * sf) % 12
    if mi:
        return (major_root + 9) % 12, 'minor'
    return major_root, 'major'


def read_key_signature(path: Path) -> Optional[Tuple[int, int]]:
    """从 SMF 里捞调号事件（FF 59 02 sf mi）。找不到返回 None。"""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if data[:4] != b'MThd':
        return None
    hlen = struct.unpack('>I', data[4:8])[0]
    off = 8 + hlen
    found = []
    while off + 8 <= len(data) and data[off:off + 4] == b'MTrk':
        ln = struct.unpack('>I', data[off + 4:off + 8])[0]
        body = data[off + 8:off + 8 + ln]
        off += 8 + ln
        i = 0
        while True:
            j = body.find(b'\xff\x59\x02', i)
            if j < 0:
                break
            sf = body[j + 3]
            mi = body[j + 4]
            sf = sf - 256 if sf > 127 else sf
            if -7 <= sf <= 7 and mi in (0, 1):
                found.append((sf, mi))
            i = j + 1
    if not found:
        return None
    # 取出现次数最多的
    return max(set(found), key=found.count)


def estimate_key(pitches: Sequence[int]) -> Tuple[int, str]:
    """没有调号时，用 Krumhansl 轮廓 + 音级直方图估一个调。"""
    hist = [0.0] * 12
    for p in pitches:
        hist[p % 12] += 1.0
    total = sum(hist) or 1.0
    hist = [h / total for h in hist]
    best = (0.0, 0, 'major')
    for root in range(12):
        for mode, prof in (('major', _KK_MAJOR), ('minor', _KK_MINOR)):
            rot = [prof[(i - root) % 12] for i in range(12)]
            m = sum(rot) / 12
            num = sum((hist[i] - 1 / 12) * (rot[i] - m) for i in range(12))
            den = (sum((hist[i] - 1 / 12) ** 2 for i in range(12)) *
                   sum((rot[i] - m) ** 2 for i in range(12))) ** 0.5 or 1.0
            score = num / den
            if score > best[0]:
                best = (score, root, mode)
    return best[1], best[2]


def resolve_key(path: Path, pitches: Sequence[int], override: Optional[str]):
    """返回 (root, mode, 来源说明, 显示名)。"""
    if override:
        root, mode = parse_key_name(override)
        return root, mode, '手写 --key', override
    sig = read_key_signature(path)
    if sig is not None:
        root, mode = key_from_signature(*sig)
        names = PITCH_CLASS_FLAT if sig[0] < 0 else PITCH_CLASS_NAME
        return root, mode, f'MIDI 调号 (sf={sig[0]}, {"minor" if sig[1] else "major"})', \
            f'{names[root]} {mode}'
    root, mode = estimate_key(pitches)
    return root, mode, '按音级直方图估的', f'{PITCH_CLASS_NAME[root]} {mode}'


def diatonic_neighbor(pitch: int, direction: int, pcs: set) -> int:
    """给定音高，沿 direction（+1 上行 / −1 下行）找**调内的前一个音**。

    C 大调里 G(67) 的"前一个调内音"是 F(65)，不是 F#(66)——
    这正是把 glissando 的整条半音阶换成"装饰音"时要的东西。
    """
    d = 1 if direction >= 0 else -1
    p = pitch - d
    for _ in range(12):
        if (p % 12) in pcs:
            return p
        p -= d
    return pitch - d


# ---------------------------------------------------------------------------
# 2.6 glissando（那条"划线"）：整条半音阶 → 一个调内装饰音
# ---------------------------------------------------------------------------

def find_chromatic_runs(notes: Sequence[Tuple[int, int]], max_gap: int,
                        max_step: int = 1, min_len: int = 2) -> List[List[Tuple[int, int]]]:
    """找"快速、单向、半音级进"的跑句（GP 里的 glissando 就长这样）。

    用"接龙"的方式而不是简单扫一遍：真实 MIDI 里同一条 gliss 常常
    上下八度叠着写（F#3/G3/G#3 + F#4/G4/G#4），逐音扫描会被交叉的音打断，
    接龙则能让每个声部各成一条链。
    """
    ns = sorted(notes)
    chains: List[List[Tuple[int, int]]] = []
    active: List[List[Tuple[int, int]]] = []
    for t, p in ns:
        active = [ch for ch in active if t - ch[-1][0] <= max_gap]
        best, best_gap = None, None
        for ch in active:
            lt, lp = ch[-1]
            g = t - lt
            if g <= 0 or g > max_gap:
                continue
            d = p - lp
            if abs(d) != max_step:
                continue
            if len(ch) >= 2 and (ch[-1][1] - ch[-2][1]) * d <= 0:
                continue                       # 方向必须一致
            if best_gap is None or g < best_gap:
                best, best_gap = ch, g
        if best is not None:
            best.append((t, p))
        else:
            ch = [(t, p)]
            chains.append(ch)
            active.append(ch)
    return [c for c in chains if len(c) >= min_len]


def apply_glissando(notes: Sequence[Tuple[int, int]], pcs: set, max_gap: int,
                    tick_step: int, min_len: int = 2) -> Tuple[List[Tuple[int, int]], int]:
    """把每条 glissando 压成一个"调内装饰音 + 落点音"。

    """
    if len(notes) < min_len:
        return list(notes), 0
    runs = find_chromatic_runs(notes, max_gap, 1, min_len)
    if not runs:
        return list(notes), 0
    ns = sorted(notes)
    drop = set()
    add: List[Tuple[int, int]] = []
    for run in runs:
        t_end, end_p = run[-1]
        up = end_p > run[0][1]
        # 落点音：紧跟着的那个音；同一 tick 有多个时取**离跑句末尾最近**的那个（别的声部不算）
        cands = [x for x in ns if 0 < x[0] - t_end <= max_gap]
        if cands:
            t_min = min(x[0] for x in cands)
            tie = [x for x in cands if x[0] == t_min]
            dest_t, dest_p = min(tie, key=lambda x: abs(x[1] - end_p))
            join = True
            direction = 1 if dest_p > end_p else (-1 if dest_p < end_p else (1 if up else -1))
        else:
            dest_t, dest_p = t_end, end_p
            join = False
            direction = 1 if up else -1
        grace = diatonic_neighbor(dest_p, direction, pcs)
        # 落点音若就在跑句末尾（没有"下一个音"），要留着；其余半音全丢
        for x in run:
            if join or x != run[-1]:
                drop.add(x)
        add.append((max(0, dest_t - tick_step), grace))
    out = [x for x in ns if x not in drop] + add
    out.sort()
    return out, len(runs)


# ---------------------------------------------------------------------------
# 3. 读 MIDI：时间、音轨、通道、音色
# ---------------------------------------------------------------------------

def build_tempo_map(tracks) -> List[Tuple[int, int]]:
    ev = []
    for tr in tracks:
        for t, e in tr:
            if e[0] == 'tempo':
                ev.append((t, e[2]))
    ev.sort()
    if not ev:
        return [(0, 500000)]
    # 同一 tick 只留最后一个
    out: List[Tuple[int, int]] = []
    for t, us in ev:
        if out and out[-1][0] == t:
            out[-1] = (t, us)
        else:
            out.append((t, us))
    if out[0][0] > 0:
        out.insert(0, (0, out[0][1]))
    return out


class TimeMap:
    def __init__(self, tempos: Sequence[Tuple[int, int]], ppq: int):
        self.tempos = list(tempos)
        self.ppq = ppq
        self._ticks = [t for t, _ in self.tempos]
        # 每个速度事件处的累计秒数
        self._sec = [0.0]
        sec = 0.0
        last_t, last_us = self.tempos[0]
        for t, us in self.tempos[1:]:
            sec += (t - last_t) / ppq * (last_us / 1e6)
            self._sec.append(sec)
            last_t, last_us = t, us

    def seconds(self, tick: int) -> float:
        i = bisect_right(self._ticks, tick) - 1
        i = max(0, min(i, len(self.tempos) - 1))
        base_t, base_us = self.tempos[i]
        return self._sec[i] + (tick - base_t) / self.ppq * (base_us / 1e6)


@dataclass(frozen=True)
class TempoStep:
    """一个阶梯：MIDI tick 区间 + 平均 BPM + 每拍写几个 NBS tick。"""
    midi_t0: int
    midi_t1: int
    bpm: float
    ticks_per_beat: int
    nbs_t0: int = 0


def tempo_segments(tempos, ppq: int, end_tick: int):
    """tempo map → [(midi_t0, midi_t1, bpm, 秒数)]。"""
    out = []
    for i, (t, us) in enumerate(tempos):
        t1 = tempos[i + 1][0] if i + 1 < len(tempos) else max(end_tick, t)
        if t1 <= t:
            continue
        out.append((t, t1, 60_000_000.0 / us, (t1 - t) / ppq * (us / 1e6)))
    return out


def dominant_bpm(segs):
    """主速度 = 按时长加权的众数，再取 ±2% 内各段的时长加权均值。"""
    hist = defaultdict(float)
    for _, _, bpm, secs in segs:
        hist[round(bpm * 2) / 2] += secs
    if not hist:
        return None
    peak = max(hist, key=hist.get)
    near = [(bpm, secs) for _, _, bpm, secs in segs if abs(bpm - peak) <= peak * 0.02]
    total = sum(secs for _, secs in near)
    return sum(bpm * secs for bpm, secs in near) / total if total else float(peak)


def build_tempo_steps(segs, ppq: int, grid: int, ref_bpm: float, step_beats: float = 4.0):
    """把速度曲线折成阶梯：每 `step_beats` 拍取一次时长加权平均 BPM，
    再换成整数 ticks_per_beat。渐快/渐慢于是变成几档 12 → 11 → 10 → 9 → 8。"""
    if not segs:
        return []
    end_tick = segs[-1][1]
    step = max(1, int(round(step_beats * ppq)))
    buckets = []
    t = 0
    while t < end_tick:
        t1 = min(end_tick, t + step)
        acc_bpm = acc_sec = 0.0
        for s0, s1, bpm, _ in segs:
            lo, hi = max(t, s0), min(t1, s1)
            if hi <= lo:
                continue
            sec = (hi - lo) / ppq * 60.0 / bpm
            acc_sec += sec
            acc_bpm += bpm * sec
        if acc_sec <= 0:
            t = t1
            continue
        bpm_avg = acc_bpm / acc_sec
        m = max(1, min(8 * grid, int(round(grid * ref_bpm / bpm_avg))))
        if buckets and buckets[-1][3] == m and buckets[-1][1] == t:
            buckets[-1] = (buckets[-1][0], t1, bpm_avg, m)
        else:
            buckets.append((t, t1, bpm_avg, m))
        t = t1

    # 累积 NBS 起点；段边界对齐到整拍，保证主体继续踩在 grid 上
    out, pos = [], 0
    for i, (t0, t1, bpm, m) in enumerate(buckets):
        out.append(TempoStep(t0, t1, bpm, m, pos))
        pos += max(0, int(round((t1 - t0) * m / ppq)))
        pos = int(round(pos / grid)) * grid
    return out


def map_to_steps(steps, midi_tick: int, ppq: int) -> int:
    i = bisect_right([s.midi_t0 for s in steps], midi_tick) - 1
    st = steps[max(0, min(i, len(steps) - 1))]
    return st.nbs_t0 + int(round((midi_tick - st.midi_t0) * st.ticks_per_beat / ppq))


def track_name(tr, idx: int) -> str:
    for _, e in tr:
        if e[0] == 'name' and e[2]:
            return str(e[2]).strip()
    return f"Track {idx + 1}"


def program_at(tr) -> Dict[int, List[Tuple[int, int]]]:
    """channel -> [(tick, program)]，按 tick 升序。"""
    out: Dict[int, List[Tuple[int, int]]] = defaultdict(list)
    for t, e in tr:
        if e[0] == 'prog':
            out[e[1]].append((t, int(e[2])))
    for v in out.values():
        v.sort()
    return out


def program_for(prog: Dict[int, List[Tuple[int, int]]], ch: int, tick: int) -> int:
    lst = prog.get(ch)
    if not lst:
        return 0
    i = bisect_right([t for t, _ in lst], tick) - 1
    return lst[i][1] if i >= 0 else 0


# ---------------------------------------------------------------------------
# 2.7 人声两声部拆分
#
# 人声在 MIDI 里只有一条轨，和声（第二声部）跟主旋律混在一起。两种拆法：
#
# A. **有 Guitar Pro 谱（.gp/.gpx）时按谱拆**（推荐、准确）
#    GP8 的 gpif 里每个音符都带 voice 编号：voice 1 = 主旋律（本曲是双音，
#    所以占 2 行），voice 2 = 绿色那条第二声部。我们对回 MIDI 的 (tick, 音高)
#    就能逐音标上 voice。
#
# B. 只有 MIDI 时按"和弦 + 多余的音"猜：
#    每个音点取至多 2 个离上一个和弦最近的音当第一声部，多出来的算第二声部。
#
# 拆完再按**乐句**决定第二声部放上还是放下（不逐音判断）：
#   一句里只要有一个音落在第一声部的休止里（或跨过休止），整句就放下面的 harp 块；
#   整句都跟第一声部同时响、且在上方，才放上面的 harp 块。
# ---------------------------------------------------------------------------

GP_NOTE_VALUES = {
    'Whole': 4.0, 'Half': 2.0, 'Quarter': 1.0, 'Eighth': 0.5,
    '16th': 0.25, 'Sixteenth': 0.25, '32nd': 0.125, 'ThirtySecond': 0.125,
    '64th': 0.0625, 'SixtyFourth': 0.0625, '128th': 0.03125,
}


def read_gp_voice_map(gp_path: Path, track_name: str,
                      ppq: int) -> Dict[Tuple[int, int], List[int]]:
    """从 GP8 的 .gpif 里读出某条轨每个音的 voice 编号。

    返回 {(MIDI tick, MIDI 音高): [voice, ...]}（同一个 tick 同音高可能既有第一声部
    又有第二声部——GP 里是两条独立声部，MIDI 里会变成两个 note-on）。
    voice 1 = 主旋律，2.. = 第二/第三声部。
    读不了（不是 GP8 zip / 没有这条轨）就返回空字典，调用方回退到纯 MIDI 拆法。
    """
    import xml.etree.ElementTree as ET
    import zipfile
    try:
        with zipfile.ZipFile(str(gp_path)) as z:
            root = ET.fromstring(z.read('Content/score.gpif').decode('utf-8'))
    except Exception:
        return {}
    names = [t.findtext('Name') for t in root.find('Tracks')]
    if track_name not in names:
        return {}
    tr_idx = names.index(track_name)

    durs: Dict[str, float] = {}
    for r in root.find('Rhythms'):
        d = GP_NOTE_VALUES.get(r.findtext('NoteValue') or 'Quarter', 1.0)
        dot = r.find('AugmentationDot')
        if dot is not None:
            d *= (2 - 0.5 ** int(dot.get('count', '1')))
        tup = r.find('Tuplet')
        if tup is not None:
            n = float(tup.findtext('Num') or 3)
            de = float(tup.findtext('Den') or 2)
            d *= de / n
        durs[r.get('id')] = d

    bars = {b.get('id'): b for b in root.find('Bars')}
    voices = {v.get('id'): v for v in root.find('Voices')}
    beats = {b.get('id'): b for b in root.find('Beats')}
    notes = {n.get('id'): n for n in root.find('Notes')}

    def note_midi(n):
        for prop in n.findall('Properties/Property'):
            if prop.get('name') == 'Midi':
                try:
                    return int(float(prop.findtext('Number')))
                except (TypeError, ValueError):
                    return None
        return None

    out: Dict[Tuple[int, int], List[int]] = {}
    pos = 0.0
    for mbar in root.find('MasterBars'):
        a, b = (mbar.findtext('Time') or '4/4').split('/')
        bar_len = 4.0 * float(a) / float(b)
        ids = (mbar.findtext('Bars') or '').split()
        if tr_idx < len(ids):
            bar = bars.get(ids[tr_idx])
            vids = (bar.findtext('Voices') or '').split() if bar is not None else []
            for vi, vid in enumerate(vids):
                if vid in ('', '-1') or vid not in voices:
                    continue
                start = pos
                for bid in (voices[vid].findtext('Beats') or '').split():
                    beat = beats.get(bid)
                    if beat is None:
                        continue
                    rh = beat.find('Rhythm')
                    dur = durs.get(rh.get('ref') if rh is not None else '', 1.0)
                    for nid in (beat.findtext('Notes') or '').split():
                        p = note_midi(notes[nid]) if nid in notes else None
                        if p is not None:
                            out.setdefault((int(round(start * ppq)), p), []).append(vi + 1)
                    start += dur
        pos += bar_len
    return out


def split_vocal_instruments(notes: Sequence[Tuple[int, int]],
                            voice_map: Dict[Tuple[int, int], List[int]]
                            ) -> Tuple[List[Tuple[int, int]], List[Tuple[int, int]]]:
    """返回 (第一声部, 第二声部)。第一声部保留双音（本曲占 2 行）。"""
    v1: List[Tuple[int, int]] = []
    v2: List[Tuple[int, int]] = []
    if voice_map:
        # 有谱：按 (tick, 音高) 对 voice；同一位置有两个音时就按 GP 的 voice 各取一个
        pool = {k: list(v) for k, v in voice_map.items()}
        for t, p in sorted(notes):
            vs = pool.get((t, p))
            v = 1
            if vs:
                v = vs.pop(0)
            (v2 if v >= 2 else v1).append((t, p))
        return v1, v2

    # 没谱：每个音点取至多 2 个"离上一个和弦最近"的音当第一声部，多的算第二声部
    by_tick: Dict[int, List[int]] = defaultdict(list)
    for t, p in notes:
        by_tick[t].append(p)
    prev: List[int] = []
    for tick in sorted(by_tick):
        ps = sorted(set(by_tick[tick]))
        if len(ps) <= 2:
            v1.extend((tick, q) for q in ps)
            prev = ps
            continue
        keep: List[int] = []
        for q in sorted(ps, key=lambda q: (min((abs(q - r) for r in prev), default=0), q)):
            if len(keep) < 2:
                keep.append(q)
        keep.sort()
        for q in ps:
            (v1 if q in keep else v2).append((tick, q))
        prev = keep
    return v1, v2


def split_v2_phrases(v2: Sequence[Tuple[int, int]], v1_ticks: set,
                     gap: int) -> Tuple[List[Tuple[int, int]], List[Tuple[int, int]]]:
    """把第二声部按乐句切开：落在第一声部休止里的句子 → 下方；其余 → 上方。"""
    if not v2:
        return [], []
    ns = sorted(v2)
    phrases: List[List[Tuple[int, int]]] = [[ns[0]]]
    for x in ns[1:]:
        if x[0] - phrases[-1][-1][0] <= gap:
            phrases[-1].append(x)
        else:
            phrases.append([x])
    below: List[Tuple[int, int]] = []
    above: List[Tuple[int, int]] = []
    for ph in phrases:
        in_rest = any(t not in v1_ticks for t, _ in ph)
        (below if in_rest else above).extend(ph)
    return above, below


# 声部 → 块名前缀。**块名一律用这个前缀**（而不是原轨名），这样
# nbs_postprocess.py 的 rhythm/lead 规则不依赖语言，也不依赖用户怎么命名。
# 电钢琴：GM program 4/5。越界时 B 会走 [flute, bell] 而不是默认的 [bell, ...]。
EP_PROGRAMS = frozenset({4, 5})

ROLE_TAG = {
    ROLE_VOCAL: 'VOCAL', ROLE_LEAD: 'LEAD', ROLE_RHYTHM: 'RHYTHM',
    ROLE_BASS: 'BASS', ROLE_DRUM: 'DRUMS', ROLE_OTHER: 'OTHER',
}

# 关键词按顺序匹配：先认人声，再认主音（"主音吉他"必须落在 lead，不能落到 rhythm）
ROLE_KEYWORDS = (
    (ROLE_VOCAL, ('vocal', 'vox', 'voice', 'choir', 'ボーカル', '人声', '主唱', '唱', '歌')),
    (ROLE_LEAD, ('lead', 'solo', 'melody', 'リード', 'メロディ', '主音', '主旋律', '旋律', '独奏')),
    (ROLE_RHYTHM, ('rhythm', 'chord', 'riff', 'gtr', 'guitar', 'コード', '吉他', '和弦', '伴奏')),
    (ROLE_BASS, ('bass', 'ベース', 'ベース', '贝斯', '贝司', '低音')),
    (ROLE_DRUM, ('drum', 'perc', 'ドラム', '鼓', '打击')),
)


def role_of(name: str) -> int:
    """按轨名判断声部。中英文 + 日文罗马字都认；不认识返回 ROLE_OTHER。"""
    n = (name or '').lower()
    for role, words in ROLE_KEYWORDS:
        if any(w in n for w in words):
            return role
    return ROLE_OTHER


def infer_role_by_content(median_pitch: float, mean_poly: float,
                          n_pitched_tracks: int) -> int:
    """轨名认不出来时的内容兜底（只在多轨乐队型文件里猜，独奏谱不猜）。

    * 音区低 + 基本单音 → 贝斯
    * 平均同时发声 ≥ 1.8（和弦型）→ 节奏
    * 音区偏高 + 单音 → 主音
    * 其余保持 OTHER（不套用任何风格规则，宁可不做也别乱改）
    """
    if n_pitched_tracks < 3:
        return ROLE_OTHER
    if median_pitch <= 47 and mean_poly <= 1.6:
        return ROLE_BASS
    if mean_poly >= 1.8:
        return ROLE_RHYTHM
    if median_pitch >= 56:
        return ROLE_LEAD
    return ROLE_OTHER


# ---------------------------------------------------------------------------
# 4. 鼓映射
# ---------------------------------------------------------------------------

def drum_slot_for(gm: int, drum_fallback: bool = False) -> Optional[DrumSlot]:
    """GM 打击乐 → 套鼓槽位。表外的音高返回 None（调用方决定怎么兜底）。"""
    sid = GM_TO_SLOT.get(gm)
    if sid:
        return SLOT_BY_ID[sid]
    if drum_fallback and gm in GM_PERCUSSION_MAPPINGS:
        inst, key = GM_PERCUSSION_MAPPINGS[gm]
        return DrumSlot(f'GM{gm}', GM_TO_NBS[inst], key)
    return None


def kit_sort_index(slot_id: str) -> Tuple[int, int]:
    """层序 = KIT 列表顺序；表外槽位排在最后。"""
    for i, s in enumerate(KIT):
        if s.slot == slot_id:
            return (0, i)
    return (1, int(''.join(ch for ch in slot_id if ch.isdigit()) or 999))


# ---------------------------------------------------------------------------
# 5. 主转换
# ---------------------------------------------------------------------------

@dataclass
class Options:
    grid: int = 8                 # 每拍的 NBS tick 数（8 = 32 分音符）
    octave_modifier: bool = True  # 默认映射里的 octaveModifier（乐器自身的八度差）
    keep_order: bool = False      # 不做人声→主音→节奏→贝斯→鼓的重排
    drum_fallback: bool = True   # 表外打击乐（沙锤/三角铁…）走 u3002 / OpenNBS 表
    separators: bool = True
    time_mode: str = 'grid'       # grid=按乐谱位置量化（推荐）| real=按真实秒数
    tempo_align: bool = False     # 主速度对齐 + 阶梯变速（多速谱用）
    gliss: str = 'diatonic'       # diatonic=整条半音阶换成调内装饰音 | keep=原样保留
    key: Optional[str] = None     # 手写调名，如 'Ab' / 'Fm'
    gp: Optional[str] = None      # 手写 Guitar Pro 谱路径（拆人声声部用）
    auto_gp: bool = True          # 自动找同目录同名的 .gp/.gpx


def convert(midi_path: Path, opt: Options) -> Tuple[dict, List[Part], dict]:
    fmt, ntrks, ppq, tracks = smf.parse_midi(str(midi_path))
    # 每拍 tick 数（4/8/16 = 二分精度；6/12/24 = 三连音档，24 同时容得下 1/8、1/16 和三连音）
    grid = opt.grid
    tempos = build_tempo_map(tracks)
    tm = TimeMap(tempos, ppq)

    # 有没有 Guitar Pro 谱可以照着拆人声声部？（同目录同名的 .gp/.gpx）
    gp_path: Optional[Path] = None
    if opt.gp:
        gp_path = Path(opt.gp)
        if not gp_path.exists():
            gp_path = None
    elif opt.auto_gp:
        for ext in ('.gp', '.gpx'):
            cand = midi_path.with_suffix(ext)
            if cand.exists():
                gp_path = cand
                break

    # 基准速度：默认用第一个 tempo 事件；开 --tempo-align 时改用「主速度」，
    # 其余速度段用阶梯伸缩（每个阶梯一个整数 ticks_per_beat）。
    tempo_steps: List[TempoStep] = []
    first_us = tempos[0][1]
    bpm = 60_000_000.0 / first_us
    if opt.tempo_align:
        last_midi_tick = max((t for tr in tracks for t, _ in tr), default=0)
        segs = tempo_segments(tempos, ppq, last_midi_tick)
        if len({round(b, 2) for _, _, b, _ in segs}) > 1:
            ref = dominant_bpm(segs)
            if ref:
                bpm = ref
                tempo_steps = build_tempo_steps(segs, ppq, grid, ref)
    tps = bpm * grid / 60.0
    nbs_tempo = max(1, min(65535, int(round(tps * 100))))
    tps = nbs_tempo / 100.0       # 以写入文件的实际值为准

    def tick_of(midi_tick: int) -> int:
        """MIDI tick → NBS tick。

        grid（默认）：按**乐谱位置**量化，tick = midi_tick × grid / PPQ。
            这样小节线、和弦天然对齐，也不会因为 tempo 四舍五入而累积漂移。
        real：按真实秒数量化，适合中途变速/自由速度的素材。
        """
        if tempo_steps:
            return map_to_steps(tempo_steps, midi_tick, ppq)
        if opt.time_mode == 'real':
            return int(round(tm.seconds(midi_tick) * tps))
        return int(round(midi_tick * grid / ppq))

    # --- 轨名 → 声部；轨名认不出来时按内容兜底（音区中位数 + 平均同时发声）---
    track_stat: Dict[int, Tuple[float, float]] = {}
    for ti, tr in enumerate(tracks):
        ons = [(t, e) for t, e in tr if e[0] == 'on' and e[1] != 9]
        if not ons:
            continue
        pitches = sorted(e[2] for _, e in ons)
        mid = len(pitches) // 2
        median = float(pitches[mid]) if len(pitches) % 2 else (pitches[mid - 1] + pitches[mid]) / 2.0
        per_tick = Counter(t for t, _ in ons)
        track_stat[ti] = (median, len(pitches) / max(1, len(per_tick)))
    n_pitched_tracks = len(track_stat)

    # --- 每个音轨按 (程序) 或 (套鼓槽位) 聚合（先存 MIDI tick，gliss 处理完再换算）
    pitched: Dict[Tuple[int, int], Part] = {}
    drums: Dict[str, Part] = {}
    stats = Counter()
    unknown_drums = set()
    role_counter = Counter()
    last_sec = 0.0

    for ti, tr in enumerate(tracks):
        name = track_name(tr, ti)
        prog = program_at(tr)
        role = role_of(name)
        role_src = 'name'
        if role == ROLE_OTHER and ti in track_stat:
            median, mean_poly = track_stat[ti]
            role = infer_role_by_content(median, mean_poly, n_pitched_tracks)
            if role != ROLE_OTHER:
                role_src = 'content'
        for t, e in tr:
            if e[0] != 'on':
                continue
            kind, ch, pitch, vel = e
            sec = tm.seconds(t)
            last_sec = max(last_sec, sec)
            if ch == 9:                      # GM 打击乐通道
                stats['drum_notes'] += 1
                slot = drum_slot_for(pitch, opt.drum_fallback)
                if slot is None:             # 表外音高：按军鼓兜底
                    unknown_drums.add(pitch)
                    slot = SLOT_BY_ID['SNARE']
                p = drums.get(slot.slot)
                if p is None:
                    p = Part(title=slot.slot, role=ROLE_DRUM, order=kit_sort_index(slot.slot),
                             instrument=slot.instrument, kind='drum', notes=[],
                             stack=slot.times,
                             meta=dict(slot=slot.slot, key=slot.key, step=slot.step,
                                       times=slot.times, track=name, sources=set(),
                                       group='drums'))
                    drums[slot.slot] = p
                p.notes.append((t, pitch))
                p.meta['sources'].add(pitch)
            else:
                pnum = program_for(prog, ch, t)
                instrument = GM_TO_NBS[GM_INSTRUMENT_MAPPINGS[pnum][0]]
                mod = GM_INSTRUMENT_MAPPINGS[pnum][1] if opt.octave_modifier else 0
                key = (ti, pnum)
                p = pitched.get(key)
                if p is None:
                    role_counter[role] += 1
                    tag = f"{ROLE_TAG[role]}{role_counter[role]}"
                    p = Part(title=name, role=role, order=(ti, pnum),
                             instrument=instrument, kind='pitched', notes=[],
                             meta=dict(program=pnum, track=name, octave=mod,
                                       tag=tag, role_src=role_src))
                    pitched[key] = p
                p.notes.append((t, pitch))
                stats['pitched_notes'] += 1

    # --- 人声轨：拆成 上和声(harp) / 第一声部(pling) / 下和声(harp) --------
    vocal_splits = Counter()
    gp_used = False
    vocal_track_no = 0
    for key in list(pitched.keys()):
        part = pitched[key]
        if part.role != ROLE_VOCAL:
            continue
        ti, pnum = key
        voice_map = read_gp_voice_map(gp_path, part.title, ppq) if gp_path else {}
        if voice_map:
            gp_used = True
        v1, v2 = split_vocal_instruments(part.notes, voice_map)
        v1_ticks = {t for t, _ in v1}
        above, below = split_v2_phrases(v2, v1_ticks, gap=2 * ppq)
        del pitched[key]
        base_name = part.title
        vocal_track_no += 1
        vtag = f"{ROLE_TAG[ROLE_VOCAL]}{vocal_track_no}"
        for k, (suffix, inst, ns, tsuffix) in enumerate((
                ('上和声', 'harp', above, 'H'),
                ('第一声部', 'pling', v1, ''),
                ('下和声', 'harp', below, 'L'),
        )):
            if not ns:
                continue
            pitched[(ti, 1000 + k)] = Part(
                title=f"{base_name} {suffix}", role=ROLE_VOCAL, order=(-1, ti * 10 + k),
                instrument=inst, kind='pitched', notes=sorted(ns),
                meta=dict(program=pnum, track=base_name, octave=0,
                          desc=True,            # 块内"上高下低"
                          tag=vtag + tsuffix, group='vocal', plain_name=base_name,
                          role_src=part.meta.get('role_src', 'name')))
        vocal_splits['v1'] += len(v1)
        vocal_splits['above'] += len(above)
        vocal_splits['below'] += len(below)
    stats['gp'] = 1 if gp_used else 0

    # --- 定调 + glissando 处理 -------------------------------------------
    all_pitches = [p for part in pitched.values() for _, p in part.notes]
    root, mode, key_src, key_display = resolve_key(midi_path, all_pitches, opt.key)
    pcs = scale_pcs(root, mode)
    gliss_gap = max(2, ppq // 12)        # ≤ 1/12 拍才算 gliss 的一步（16 分音符不会误伤）
    tick_step = max(1, ppq // grid)  # 一个 NBS tick 等于多少 MIDI tick
    gliss_runs = 0
    gliss_dropped = 0
    for part in pitched.values():
        before = len(part.notes)
        if opt.gliss == 'diatonic':
            part.notes, n = apply_glissando(part.notes, pcs, gliss_gap, tick_step)
            gliss_runs += n
            gliss_dropped += before - len(part.notes)
        part.notes.sort()

    # --- 全部换算成 NBS tick ----------------------------------------------
    for part in list(pitched.values()) + list(drums.values()):
        part.notes = [(max(0, tick_of(t)), p) for t, p in part.notes]

    parts: List[Part] = list(pitched.values())
    if opt.keep_order:
        parts.sort(key=lambda p: p.order)
    else:                                  # 人声 → 主音 → 节奏 → 贝斯 → 鼓 → 其他
        parts.sort(key=lambda p: (p.role, p.order[0], p.order[1]))
    parts += sorted(drums.values(), key=lambda p: kit_sort_index(p.title))

    header = dict(
        name=midi_path.stem,
        author='',
        orig_author='',
        description='Generated by NyaaFlavor (MIDI -> Note Block Studio)',
        tempo=nbs_tempo,
        time_signature=4,
        midi_filename=midi_path.name,
        version=6,
        vanilla=20,
    )
    info = dict(fmt=fmt, ntrks=len(tracks), ppq=ppq, bpm=bpm, tps=tps,
                grid=grid, tempo=nbs_tempo, tempos=tempos,
                tempo_align=[dict(bpm=round(st.bpm, 2), ticks_per_beat=st.ticks_per_beat,
                                  beats=round((st.midi_t1 - st.midi_t0) / ppq, 2),
                                  nbs_t0=st.nbs_t0) for st in tempo_steps],
                duration_sec=last_sec, stats=stats,
                unknown_drums=sorted(unknown_drums),
                key=dict(root=root, mode=mode, source=key_src, name=key_display,
                         pcs=sorted(pcs)),
                vocal=dict(v1=vocal_splits['v1'], above=vocal_splits['above'],
                           below=vocal_splits['below'], gp=bool(stats['gp']),
                           blocks=sum(1 for p in pitched.values()
                                      if p.meta.get('group') == 'vocal')),
                gp_path=str(gp_path) if gp_path else '',
                gliss=dict(runs=gliss_runs, dropped=gliss_dropped,
                           min_gap=gliss_gap, mode=opt.gliss))
    return header, parts, info


@dataclass
class LayerDef:
    name: str
    lock: int = 0
    volume: int = 100
    panning: int = 100
    block: str = ''


def layout(parts: List[Part], opt: Options) -> Tuple[List[LayerDef], List[Tuple[int, int, int, int, int, int]], dict]:
    """把 Part 列表铺成 layer + notes。返回 (layers, notes, report)。

    每个 Part 变成一个「层块」。旋律块前面插一条静音空层（lock=1）做分隔；
    **鼓组是一整片连续的层，块与块之间不插静音层**（只在鼓组最前面放一条组标题）。
    """
    layers: List[LayerDef] = []
    notes_out: List[Tuple[int, int, int, int, int, int]] = []   # tick, layer, inst, key, vel, pan
    report = dict(blocks=[], dropped=Counter(), clamped=0, outside_window=0,
                  retrigger=0, retrigger_roll=0, notes_in=0, notes_out=0, poly_max=0)
    roll_layers: set = set()          # 吊镲三连击所在的层（那种 1 tick 连击是刻意的）

    def add_separator(title: str):
        layers.append(LayerDef(name=f'-- {title} --', lock=1))

    pitched_parts = [p for p in parts if p.kind != 'drum']
    drum_parts = [p for p in parts if p.kind == 'drum']
    vocal_parts = [p for p in pitched_parts if p.meta.get('group') == 'vocal']
    prev_group = object()

    if opt.separators:
        add_separator('MIDI2NBS')

    for part in pitched_parts + drum_parts:
        is_drum = part.kind == 'drum'
        if is_drum:
            inst = part.instrument
            dkey = clamp_byte(part.meta['key'])
            times = max(1, part.meta.get('times', 1))
            step = max(0, part.meta.get('step', 0))
            tag = part.meta.get('slot', part.title)         # 例：BD / CRASH / HH-O
            hits = sorted({tick for tick, _ in part.notes})
            # 一次触发写 times 个音：step=0 同一 tick 叠，step>0 每隔 step 个 tick 放一个
            events = [(tick + j * step, j) for tick in hits for j in range(times)]
            # 贪心分配 layer：同一 tick 撞车才多开一层（三连击/单发都只占 1 层）
            used: Dict[int, set] = {}
            placed = []
            for tick, j in sorted(events):
                li = 0
                while tick in used.get(li, ()):
                    li += 1
                used.setdefault(li, set()).add(tick)
                placed.append((tick, li))
            n_layers = max(used) + 1 if used else 0
            block_notes = [(tick, li, nbs.INSTR_ID[inst], dkey) for tick, li in placed]
            title = f"{tag} {key_name(dkey)}"
            if times > 1:
                title += f" ×{times}" + (f"(隔{step}tick)" if step else "")
        else:
            tag = part.meta.get('tag') or part.title
            desc = bool(part.meta.get('desc'))
            by_tick: Dict[int, List[int]] = defaultdict(list)
            for tick, pitch in part.notes:
                if tick < 0:
                    continue
                k = pitch - MIDI_BASE + 12 * part.meta.get('octave', 0)
                if not (KEY_MIN_BYTE <= k <= KEY_MAX_BYTE):
                    report['clamped'] += 1
                    k = clamp_byte(k)
                if not in_window(k):
                    report['outside_window'] += 1
                by_tick[tick].append(k)
            n_layers = max((len(set(v)) for v in by_tick.values()), default=0)
            # 块名 = 声部前缀（nbs_postprocess 靠它认 rhythm/lead），原轨名放在箭头后面
            # （NBS 字符串按 cp1252 写，非 ASCII 的轨名只会变成 "? ??"，干脆不写；
            #   原轨名在 --report 的 JSON 里仍完整保留）
            title = f"{tag} [{part.instrument}]"
            # 电钢琴（GM 4/5）要特殊照顾：越界时先借 flute 再借 bell，而不是直接 bell。
            # 这里用 program 号做机器可读标记（"p4"/"p5"），B 靠它认，不受轨名语言影响。
            prog = part.meta.get('program')
            if prog in EP_PROGRAMS:
                title += f" p{prog}"
            src = part.meta.get('track') or ''
            if src and src != tag and src.isascii():
                title += f" ← {src}"
            block_notes = [(tick, i, nbs.INSTR_ID[part.instrument], k)
                           for tick in sorted(by_tick)
                           for i, k in enumerate(sorted(set(by_tick[tick]), reverse=desc))]

        report['notes_in'] += len(part.notes)
        if not block_notes:
            continue

        if opt.separators:
            group = part.meta.get('group')
            if group and group == prev_group:
                pass                        # 组内（人声三块 / 鼓组）不插静音层
            elif group == 'drums':
                add_separator(f'DRUMS ({len(drum_parts)} slots: cymbal > snare > tom > kick > stick)')
            elif group == 'vocal':
                names = [p.meta.get('tag', '') for p in vocal_parts]
                add_separator(f'VOCAL ({len(vocal_parts)} blocks: {" / ".join(names)})')
            else:
                add_separator(title)
            prev_group = group
        base = len(layers)
        for i in range(n_layers):
            suffix = '' if n_layers == 1 else f' {i + 1}/{n_layers}'
            layers.append(LayerDef(name=f"{tag}{suffix}"))
        if is_drum and part.meta.get('step', 0) > 0:
            roll_layers.update(range(base, base + n_layers))

        # 防御性检查：同一 (tick, layer) 不允许两个音（NBS 格式硬限制）
        seen = set()
        dropped = 0
        for tick, li, iid, k in block_notes:
            sig = (tick, base + li)
            if sig in seen:
                dropped += 1
                continue
            seen.add(sig)
            notes_out.append((tick, base + li, iid, k, 100, 100))
        if dropped:
            report['dropped'][title] += dropped

        ks = [k for _, _, _, k in block_notes]
        report['blocks'].append(dict(
            title=title, kind=part.kind, layers=n_layers, base=base,
            instrument=part.instrument,
            key=part.meta.get('key') if is_drum else None,
            key_lo=min(ks), key_hi=max(ks),
            notes=len({(t, li, iid, k) for t, li, iid, k in block_notes}),
            source=part.meta.get('track', ''),
        ))

    if opt.separators:
        add_separator('END')

    report['notes_out'] = len(notes_out)
    # 同层同音高重击间隔（吊镲三连击那种 1 tick 连击是设计好的，单独计）
    last = {}
    for tick, li, iid, k, _, _ in sorted(notes_out):
        key = (li, iid, k)
        prev = last.get(key)
        if prev is not None and tick - prev < 2:
            if li in roll_layers:
                report['retrigger_roll'] += 1
            else:
                report['retrigger'] += 1
        last[key] = tick
    # 2 tick 滑窗最大同时发声
    ticks = sorted(set(t for t, *_ in notes_out))
    per_tick = Counter(t for t, *_ in notes_out)
    mx = 0
    for t in ticks:
        mx = max(mx, per_tick[t] + per_tick.get(t + 1, 0))
    report['poly_max'] = mx
    return layers, notes_out, report


# ---------------------------------------------------------------------------
# 6. 写 .nbs（UTF-8 字符串，严格递增的 tick/layer 增量）
# ---------------------------------------------------------------------------

def write_nbs(path: Path, header: dict, layers: List[LayerDef],
              notes: Sequence[Tuple[int, int, int, int, int, int]]) -> None:
    if len(layers) > 65535:
        raise ValueError(f"层数过多：{len(layers)}")
    by_tick: Dict[int, List[Tuple[int, int, int, int, int, int]]] = defaultdict(list)
    for n in notes:
        by_tick[n[0]].append(n)
    max_tick = max(by_tick) if by_tick else 0
    song_length = min(65535, max(1, max_tick + 1))

    buf = bytearray()
    W = lambda fmt, *v: buf.extend(struct.pack(fmt, *v))

    def S(txt: str) -> None:
        b = nbs.ascii_safe(txt or '').encode('cp1252', 'replace')   # NBS export reads cp1252
        buf.extend(struct.pack('<i', len(b)))
        buf.extend(b)

    W('<H', 0)                       # 新版标记（旧 ASCII "NBS" 已被两个 0 取代）
    W('<B', header.get('version', 6))
    W('<B', header.get('vanilla', 20))
    W('<H', song_length)
    W('<H', len(layers))
    for k in ('name', 'author', 'orig_author', 'description'):
        S(header.get(k, ''))
    W('<H', header['tempo'])
    W('<B', 0)                       # auto_save
    W('<B', 10)                      # auto_save_duration
    W('<B', header.get('time_signature', 4))
    for _ in range(5):
        W('<i', 0)                   # 5 个统计字段
    S(header.get('midi_filename', ''))
    W('<B', 0)                       # loop
    W('<B', 0)                       # max_loop_count
    W('<H', 0)                       # loop_start

    last_tick = -1
    for tick in sorted(by_tick):
        dt = tick - last_tick
        if not (0 < dt <= 32767):
            raise ValueError(f"tick 增量非法：{tick} - {last_tick}")
        W('<h', dt)
        last_tick = tick
        last_layer = -1
        for n in sorted(by_tick[tick], key=lambda x: x[1]):
            li = n[1]
            dl = li - last_layer
            if dl <= 0:
                raise ValueError(f"layer 增量非法：tick {tick} layer {li}")
            W('<h', dl)
            W('<B', n[2]); W('<B', n[3])
            W('<B', n[4]); W('<B', n[5])
            W('<h', 0)
            last_layer = li
        W('<h', 0)
    W('<h', 0)

    for L in layers:
        S(L.name)
        W('<B', L.lock)
        W('<B', L.volume)
        W('<B', L.panning)
    W('<B', 0)                       # 无自定义音色
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(buf))


# ---------------------------------------------------------------------------
# 7. 回读校验
# ---------------------------------------------------------------------------

def verify(path: Path, expect_notes: int, layer_count: int) -> List[str]:
    problems = []
    s = nbs.load(str(path))
    got = sum(len(L.notes) for L in s.layers)
    if got != expect_notes:
        problems.append(f"音符数不符：写 {expect_notes}，读回 {got}")
    if len(s.layers) != layer_count:
        problems.append(f"层数不符：写 {layer_count}，读回 {len(s.layers)}")
    seen = set()
    for li, L, n in s.all_notes():
        if (n.tick, li) in seen:
            problems.append(f"(tick,layer) 重复：{n.tick},{li}")
            break
        seen.add((n.tick, li))
    ks = [n.key for _, _, n in s.all_notes()]
    if ks and (min(ks) < 0 or max(ks) > 87):
        problems.append(f"key 越界：{min(ks)}..{max(ks)}")
    return problems


# ---------------------------------------------------------------------------
# 8. CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    # Windows 控制台默认可能是 cp950/cp936，中文会直接抛 UnicodeEncodeError
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass

    ap = argparse.ArgumentParser(
        description='MIDI → NBS：每轨独立层块（静音层分隔），鼓按套鼓槽位并层。',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='例：\n'
               '  python midi2nbs.py in.mid\n'
               '  python midi2nbs.py in.mid -o out.nbs --title 迷星叫 --author 某人\n'
               '  python midi2nbs.py in.mid --dry-run     # 只统计不落盘\n'
               '  python midi2nbs.py in.mid --print-maps  # 打印生效的音色/鼓槽位表')
    ap.add_argument('midi', type=Path, nargs='?', default=None, help='输入 .mid')
    ap.add_argument('-o', '--output', type=Path, default=None,
                    help='输出 .nbs（默认与输入同目录同名）')
    ap.add_argument('--title', default=None, help='曲名（默认用文件名）')
    ap.add_argument('--author', default='', help='NBS 作者字段')
    ap.add_argument('--orig-author', default='', help='原曲作者字段')
    ap.add_argument('--grid', type=int, default=8, choices=[4, 6, 8, 12, 16, 24],
                    help='每拍几条 NBS tick（默认 8）。4/8/16 是二分精度；'
                         '6/12/24 = 8 的 3/4、3/2、3 倍，给三连音用（24 同时容得下 1/8、1/16）')
    ap.add_argument('--time', choices=['grid', 'real'], default='grid',
                    help='时间换算：grid=按乐谱位置（默认）| real=按真实秒数')
    ap.add_argument('--tempo-align', action='store_true',
                    help='变速曲：按「主速度」定 tempo，前奏/尾奏/渐快用阶梯伸缩对齐到 tick')
    ap.add_argument('--gliss', choices=['diatonic', 'keep'], default='diatonic',
                    help='glissando（那条半音阶跑句）：diatonic=换成调内装饰音（默认）| keep=原样保留')
    ap.add_argument('--key', default=None,
                    help="手写调名（如 Ab / C#m），覆盖 MIDI 调号与自动估调")
    ap.add_argument('--gp', default=None,
                    help='Guitar Pro 谱（.gp/.gpx），用它精确拆人声第一/第二声部')
    ap.add_argument('--no-gp', action='store_true',
                    help='不自动找同目录同名的 .gp/.gpx')
    ap.add_argument('--no-octave-modifier', action='store_true',
                    help='不套用默认映射里的 octaveModifier（等价于 key 一律 = MIDI−21）')
    ap.add_argument('--keep-order', action='store_true', help='按 MIDI 音轨原顺序排版')
    ap.add_argument('--no-separators', action='store_true', help='不插静音分隔层')
    ap.add_argument('--no-drum-fallback', dest='drum_fallback', action='store_false',
                    help='关掉表外打击乐的 u3002 / OpenNBS 映射，退回军鼓')
    ap.add_argument('--force', action='store_true', help='允许覆盖已存在的输出')
    ap.add_argument('--dry-run', action='store_true', help='只统计，不写文件')
    ap.add_argument('--print-maps', action='store_true', help='打印生效的映射表后退出')
    ap.add_argument('--report', type=Path, default=None, help='额外写一份 JSON 报告')
    args = ap.parse_args(argv)

    if args.print_maps:
        print('=== 默认音色映射（u3002 / OpenNBS 表；oct 列是音色自身八度偏移，默认启用）')
        for p in range(128):
            inst, mod = GM_INSTRUMENT_MAPPINGS[p]
            print(f'  {p:3d} -> {GM_TO_NBS[inst]:16s} oct={mod:+d}')
        print('=== 套鼓槽位（layer 从上到下就是这个顺序）')
        for s in KIT:
            extra = f' ×{s.times}' + (f'（隔 {s.step} tick）' if s.step else '（同 tick 叠）') if s.times > 1 else ''
            srcs = [f'GM{g}({GM_DRUM_NAMES.get(g, "?")})' for g, sid in GM_TO_SLOT.items() if sid == s.slot]
            print(f'  {s.slot:<7s} {s.instrument:<8s} @ {s.key:2d} ({key_name(s.key):>3s}){extra}')
            print(f'          ← {", ".join(srcs) if srcs else "（表外音高，默认按 u3002 / OpenNBS 表单独成层）"}')
        return 0

    midi_path: Path = args.midi
    if midi_path is None:
        ap.print_help()
        print('\n缺少输入 MIDI；只想看映射表请加 --print-maps', file=sys.stderr)
        return 2
    if not midi_path.exists():
        print(f'找不到输入：{midi_path}', file=sys.stderr)
        return 2
    out = args.output or midi_path.with_suffix('.nbs')
    if out.exists() and not args.force and not args.dry_run:
        print(f'输出已存在：{out}（加 --force 覆盖）', file=sys.stderr)
        return 2

    opt = Options(grid=args.grid,
                  octave_modifier=not args.no_octave_modifier,
                  keep_order=args.keep_order,
                  drum_fallback=args.drum_fallback,
                  separators=not args.no_separators, time_mode=args.time,
                  gliss=args.gliss, key=args.key, tempo_align=args.tempo_align,
                  gp=args.gp, auto_gp=not args.no_gp)

    header, parts, info = convert(midi_path, opt)
    header['name'] = args.title or midi_path.stem
    header['author'] = args.author
    header['orig_author'] = args.orig_author

    layers, notes, rep = layout(parts, opt)

    print(f'=== {midi_path.name} → {out.name}')
    print(f'  MIDI   : 格式 {info["fmt"]}, {info["ntrks"]} 轨, '
          f'每拍 {info["ppq"]} tick, {info["bpm"]:.2f} BPM, '
          f'{len(info["tempos"])} 个速度事件')
    print(f'  时间轴 : tempo={info["tempo"]} ({info["tps"]:.2f} tick/秒), '
          f'1 拍 = {info["grid"]} tick'
          + ('（三连音档：1/3 拍 = %d tick）' % (info['grid'] // 3)
             if info['grid'] in (6, 12, 24) else '')
          + f', 量化误差 < {0.5 / info["tps"] * 1000:.1f} ms')
    k = info['key']
    print(f'  调性   : {k["name"]}（{k["source"]}，调内音级 '
          f'{", ".join(PITCH_CLASS_NAME[x] for x in k["pcs"])}）')
    g = info['gliss']
    if g['mode'] == 'diatonic':
        print(f'  gliss  : {g["runs"]} 条半音阶跑句 → 调内装饰音'
              f'（净减 {g["dropped"]} 个音；步长阈值 {g["min_gap"]} MIDI tick）')
    else:
        print('  gliss  : 原样保留（--gliss keep）')
    vs = info['vocal']
    if vs['blocks']:
        src = '按 Guitar Pro 谱的 voice 拆' if vs['gp'] else '只有 MIDI，按和弦+多余音猜'
        print(f'  人声   : {src}；第一声部 {vs["v1"]} 音 / 上和声 {vs["above"]} / '
              f'下和声 {vs["below"]}')
    print(f'  产出   : {len(layers)} 层（含静音分隔层）, {len(notes)} 音')
    print()
    blocks = rep['blocks']
    # 列宽按内容算，保证中文块名不会把右边的列顶歪
    title_w = max([disp_width('层块')] + [disp_width(b['title']) for b in blocks])
    inst_w = max([disp_width('音色')] + [disp_width(b['instrument']) for b in blocks])
    key_w = max([disp_width('key 范围')] +
                [disp_width(f'{b["key_lo"]}..{b["key_hi"]}') for b in blocks])
    print(f'  {ljust("层块", title_w)} {ljust("音色", inst_w)} {rjust("key 范围", key_w)} '
          f'{rjust("层数", 4)} {rjust("音符", 6)}')
    for b in blocks:
        kk = f'{b["key_lo"]}..{b["key_hi"]}'
        print(f'  {ljust(b["title"], title_w)} {ljust(b["instrument"], inst_w)} {rjust(kk, key_w)} '
              f'{rjust(str(b["layers"]), 4)} {rjust(str(b["notes"]), 6)}')
    print()
    if rep['dropped']:
        print(f'  被丢弃的重复音：{dict(rep["dropped"])}')
    if rep['clamped']:
        print(f'  ⚠ 超出 0..87 被截到边界的音：{rep["clamped"]}')
    if rep['outside_window']:
        pct = 100.0 * rep['outside_window'] / max(1, len(notes))
        print(f'  ⚠ 落在可播放窗口 33..57 之外的音：{rep["outside_window"]} ({pct:.1f}%) '
              f'—— 交给第 ③ 步处理')
    if info.get('unknown_drums'):
        print(f'  ⚠ 默认鼓表里没有的 GM 打击乐音高：{info["unknown_drums"]}（已按军鼓兜底）')
    if info.get('tempo_align'):
        steps = info['tempo_align']
        seg = ' → '.join(f'{s["bpm"]:.0f}×{s["ticks_per_beat"]}' for s in steps)
        print(f'  · 变速曲：基准 {info["bpm"]:.2f} BPM，{len(steps)} 档阶梯对齐'
              f'（每档 = 该段 BPM × 每拍 tick 数）：{seg}')
    elif len({u for _, u in info['tempos']}) > 1:
        print(f'  · MIDI 含 {len(info["tempos"])} 个速度事件，NBS 只能定速：'
              f'按首段 {info["bpm"]:.2f} BPM 写（要严格保时长用 --time real）')
    print(f'  相邻不到 2 tick 的同音高重复触发：{rep["retrigger"]}'
          + (f'（另有 {rep["retrigger_roll"]} 处是吊镲三连击，刻意如此）'
             if rep.get('retrigger_roll') else ''))
    print(f'  最大同时发声：{rep["poly_max"]} 个'
          + ('  ⚠ 超过 40，Notebot 可能漏音' if rep['poly_max'] > 40 else ''))

    if notes and max(n[0] for n in notes) >= 65535:
        print('  ⚠ 长度超过 NBS 的 65535 tick 上限，尾部会被截断'
              '（可降到 --grid 12 或 8）')

    if args.dry_run:
        return 0

    write_nbs(out, header, layers, notes)
    problems = verify(out, len(notes), len(layers))
    if problems:
        print('\n校验失败：', file=sys.stderr)
        for p in problems:
            print('  - ' + p, file=sys.stderr)
        return 1
    s = nbs.load(str(out))
    dur = s.duration_ticks()
    print(f'\n已写入 {out}')
    print(f'  校验通过：{sum(len(L.notes) for L in s.layers)} 音 / {len(s.layers)} 层 / '
          f'{dur} tick = {s.tick_to_sec(dur):.1f} 秒（{s.tempo / 100:.2f} tick/秒）')

    if args.report:
        payload = dict(midi=str(midi_path), output=str(out), tempo=info['tempo'],
                       bpm=info['bpm'], grid=info['grid'], layers=len(layers),
                       notes=len(notes), key=info['key'], gliss=info['gliss'],
                       vocal=info['vocal'], gp=info.get('gp_path', ''),
                       blocks=rep['blocks'],
                       warnings={k: rep[k] for k in ('clamped', 'outside_window',
                                                     'retrigger', 'poly_max')})
        args.report.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                               encoding='utf-8')
        print(f'  报告：{args.report}')
    return 0


if __name__ == '__main__':
    sys.exit(main())









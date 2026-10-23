"""Lenient SMF parser + MIDI<->NBS pitch offset detection."""
import struct, sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbs
from nbs import NOTE_NAMES, keyname


def nm(x): return f"{NOTE_NAMES[x % 12]}{x // 12 - 1}"


def read_varlen(d, o):
    v = 0
    while True:
        b = d[o]; o += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, o


def parse_midi(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MThd", "not SMF"
    hlen = struct.unpack(">I", d[4:8])[0]
    fmt, ntrks, div = struct.unpack(">HHH", d[8:14])
    o = 8 + hlen
    tracks = []
    while o < len(d) - 8 and d[o:o + 4] == b"MTrk":
        ln = struct.unpack(">I", d[o + 4:o + 8])[0]
        body = d[o + 8:o + 8 + ln]
        o += 8 + ln
        tracks.append(parse_track(body))
    return fmt, ntrks, div, tracks


def parse_track(b):
    """Return list of (abs_tick, event_dict). Tolerates malformed files."""
    o = 0
    t = 0
    status = None
    out = []
    n = len(b)
    while o < n:
        try:
            dt, o = read_varlen(b, o)
        except IndexError:
            break
        t += dt
        if o >= n:
            break
        c = b[o]
        if c & 0x80:
            status = c; o += 1
        elif status is None:
            o += 1; continue
        hi = status & 0xF0
        if hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
            if o + 1 >= n: break
            a = b[o] & 0x7F; bb = b[o + 1] & 0x7F; o += 2
            if hi == 0x90 and bb > 0:
                out.append((t, ("on", status & 0x0F, a, bb)))
            elif hi == 0x80 or (hi == 0x90 and bb == 0):
                out.append((t, ("off", status & 0x0F, a, 0)))
        elif hi in (0xC0, 0xD0):
            if o >= n: break
            a = b[o] & 0x7F; o += 1
            out.append((t, ("prog", status & 0x0F, a, 0)))
        elif hi == 0xF0:
            if c == 0xFF:
                if o >= n: break
                mt = b[o]; o += 1
                ln, o = read_varlen(b, o)
                data = b[o:o + ln]; o += ln
                if mt == 0x03:
                    out.append((t, ("name", 0, data.decode("utf-8", "replace"), 0)))
                elif mt == 0x51:
                    out.append((t, ("tempo", 0, int.from_bytes(data[:3], "big"), 0)))
            elif c in (0xF0, 0xF7):
                ln, o = read_varlen(b, o)
                o += ln
            else:
                o += 1
        else:
            o += 1
    return out


def notes_of(tracks):
    res = []
    for ti, tr in enumerate(tracks):
        names = [e[2] for _, e in tr if e[0] == "name"]
        ns = [(t, e[2]) for t, e in tr if e[0] == "on"]
        res.append((ti, names, ns))
    return res


if __name__ == "__main__":
    # 调试用：python smf.py song.mid [更多 .mid ...]
    if len(sys.argv) < 2:
        raise SystemExit("用法: python smf.py <song.mid> [更多 .mid]  # 打印每个 MIDI 的轨/音域")
    for path in sys.argv[1:]:
        print("=" * 80)
        print(path)
        try:
            fmt, ntr, div, tracks = parse_midi(path)
        except Exception as e:
            print("  解析失败:", e)
            continue
        print(f"  fmt={fmt} ntrks={ntr} div={div}")
        allm = []
        for ti, names, ns in notes_of(tracks):
            if ns:
                lo = min(x[1] for x in ns); hi = max(x[1] for x in ns)
                print(f"   trk{ti} {names} n={len(ns)} {lo}-{hi} ({nm(lo)}-{nm(hi)})")
                allm += [x[1] for x in ns]
        if allm:
            print(f"   音域（原始 MIDI 音高）{min(allm)}-{max(allm)} ({nm(min(allm))}-{nm(max(allm))})")

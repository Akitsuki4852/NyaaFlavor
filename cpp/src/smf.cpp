#include "smf.h"

#include <cstdio>

#include "common.h"
#include "nbs.h"

namespace nyaa {

namespace {

std::string slurp(const std::string& path) {
    return read_file(path);
}

uint32_t be_u32(const std::string& d, size_t o) {
    return ((uint32_t)(uint8_t)d[o] << 24) | ((uint32_t)(uint8_t)d[o + 1] << 16) |
           ((uint32_t)(uint8_t)d[o + 2] << 8) | (uint32_t)(uint8_t)d[o + 3];
}

uint16_t be_u16(const std::string& d, size_t o) {
    return (uint16_t)(((uint8_t)d[o] << 8) | (uint8_t)d[o + 1]);
}

// Python 的 data.decode('utf-8','replace')
std::string decode_replace(const std::string& b) { return dec_text(b); }

// read_varlen：与 Python 行为一致（越界 → 抛 IndexError 的效果）
struct VarlenEnd {};

int varlen(const std::string& d, size_t& o) {
    int v = 0;
    for (;;) {
        if (o >= d.size()) throw VarlenEnd{};
        const uint8_t b = static_cast<uint8_t>(d[o]);
        ++o;
        v = (v << 7) | (b & 127);
        if (!(b & 128)) return v;
    }
}

}  // namespace

bool read_varlen(const std::string& d, size_t& o, int& out) {
    try {
        out = varlen(d, o);
        return true;
    } catch (const VarlenEnd&) {
        return false;
    }
}

MidiFile parse_midi(const std::string& path) {
    const std::string d = slurp(path);
    if (d.size() < 14 || d.compare(0, 4, "MThd") != 0)
        throw std::runtime_error("不是有效的 MIDI 文件（找不到 MThd 文件头）：" + path);
    const size_t hlen = be_u32(d, 4);
    MidiFile mf;
    mf.fmt = be_u16(d, 8);
    mf.ntrks = be_u16(d, 10);
    mf.ppq = be_u16(d, 12);
    if (mf.ppq == 0 || (mf.ppq & 0x8000)) throw std::runtime_error("仅支持 PPQ 时间轴的 MIDI 文件");

    size_t o = 8 + hlen;
    while (o + 8 <= d.size() && d.compare(o, 4, "MTrk") == 0) {
        const size_t ln = be_u32(d, o + 4);
        const std::string body = d.substr(o + 8, ln);
        o += 8 + ln;
        mf.tracks.push_back(parse_track(body));
    }
    return mf;
}

MidiTrack parse_track(const std::string& b) {
    std::vector<MidiEvent> out;
    size_t o = 0;
    const size_t n = b.size();
    int t = 0;
    int status = -1;

    while (o < n) {
        int dt = 0;
        if (!read_varlen(b, o, dt)) break;  // IndexError → break
        t += dt;
        if (o >= n) break;
        const uint8_t c = static_cast<uint8_t>(b[o]);
        if (c & 128) {
            status = c;
            ++o;
        } else if (status < 0) {
            ++o;
            continue;
        }
        const int hi = status & 240;

        if (hi == 128 || hi == 144 || hi == 160 || hi == 176 || hi == 224) {
            if (o + 1 >= n) break;
            const int a = (uint8_t)b[o] & 127;
            const int bb = (uint8_t)b[o + 1] & 127;
            o += 2;
            if (hi == 144 && bb > 0) {
                out.push_back({t, EvKind::On, status & 15, a, bb, {}});
            } else if (hi == 128 || (hi == 144 && bb == 0)) {
                out.push_back({t, EvKind::Off, status & 15, a, 0, {}});
            }
        } else if (hi == 192 || hi == 208) {
            if (o >= n) break;
            const int a = (uint8_t)b[o] & 127;
            ++o;
            out.push_back({t, EvKind::Prog, status & 15, a, 0, {}});
        } else if (hi == 240) {
            if (c == 255) {
                if (o >= n) break;
                const int mt = (uint8_t)b[o];
                ++o;
                int ln = 0;
                if (!read_varlen(b, o, ln)) break;
                const std::string data = b.substr(o, static_cast<size_t>(ln));
                o += static_cast<size_t>(ln);
                if (mt == 3) {
                    out.push_back({t, EvKind::Name, 0, 0, 0, decode_replace(data)});
                } else if (mt == 81) {
                    int us = 0;
                    for (size_t i = 0; i < data.size() && i < 3; ++i) us = (us << 8) | (uint8_t)data[i];
                    out.push_back({t, EvKind::Tempo, 0, 0, us, {}});
                }
            } else if (c == 240 || c == 247) {
                int ln = 0;
                if (!read_varlen(b, o, ln)) break;
                o += static_cast<size_t>(ln);
            } else {
                ++o;
            }
        } else {
            ++o;
        }
    }
    return out;
}

std::string midi_note_name(int p) {
    static const char* const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(N[pymod(p, 12)]) + std::to_string(floordiv(p, 12) - 1);
}

bool read_key_signature(const std::string& path, int& sf, int& mi) {
    std::string data;
    try {
        data = slurp(path);
    } catch (...) {
        return false;
    }
    if (data.size() < 8 || data.compare(0, 4, "MThd") != 0) return false;
    const size_t hlen = be_u32(data, 4);
    size_t off = 8 + hlen;
    std::vector<std::pair<int, int>> found;
    while (off + 8 <= data.size() && data.compare(off, 4, "MTrk") == 0) {
        const size_t ln = be_u32(data, off + 4);
        const std::string body = data.substr(off + 8, ln);
        off += 8 + ln;
        size_t i = 0;
        for (;;) {
            const size_t j = body.find("\xff\x59\x02", i);
            if (j == std::string::npos) break;
            if (j + 5 > body.size()) break;
            int s = (uint8_t)body[j + 3];
            const int m = (uint8_t)body[j + 4];
            if (s > 127) s -= 256;
            if (s >= -7 && s <= 7 && (m == 0 || m == 1)) found.emplace_back(s, m);
            i = j + 1;
        }
    }
    if (found.empty()) return false;
    // max(set(found), key=found.count) —— 计数最多者；并列时取集合迭代顺序（先出现的小值）
    int best_i = 0, best_c = -1;
    for (size_t k = 0; k < found.size(); ++k) {
        int c = 0;
        for (const auto& x : found)
            if (x == found[k]) ++c;
        if (c > best_c) {
            best_c = c;
            best_i = static_cast<int>(k);
        }
    }
    sf = found[best_i].first;
    mi = found[best_i].second;
    return true;
}

}  // namespace nyaa

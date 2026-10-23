#include "nbs.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "common.h"
#include "pinyin.h"

namespace nyaa {

const char* const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

const char* const INSTRUMENTS[20] = {"harp",     "dbass",    "bdrum", "sdrum",  "clicks",
                                     "guitar",   "flute",    "bell",  "icechime", "xylophone",
                                     "iron_xyl", "cow_bell", "didgeridoo", "bit", "banjo",
                                     "pling",    "trumpet",  "trumpet_exposed", "trumpet_weathered",
                                     "trumpet_oxidized"};

const char* const INSTR_CN[16] = {"钢琴", "贝斯", "底鼓", "军鼓", "击",     "吉他", "笛", "钟琴",
                                  "风铃", "木琴", "铁琴", "牛铃", "管",   "方波", "班卓", "琵琶"};

int Song::duration_ticks() const {
    int m = 0;
    for (const auto& L : layers)
        for (const auto& n : L.notes) m = std::max(m, n.tick);
    return std::max(m, song_length);
}

std::optional<std::string> Song::instrument_of_layer(int li) const {
    if (li < 0 || li >= static_cast<int>(layers.size())) return std::nullopt;
    std::map<int, int> count;
    for (const auto& n : layers[li].notes) ++count[n.instrument];
    if (count.empty()) return std::nullopt;
    int best = count.begin()->first;
    int best_n = -1;
    for (const auto& kv : count) {
        if (kv.second > best_n) {  // 平手取先出现（instrument 升序）
            best = kv.first;
            best_n = kv.second;
        }
    }
    return iname(best);
}

std::string iname(int i) {
    if (i >= 0 && i < 20) return INSTRUMENTS[i];
    return "custom" + std::to_string(i);
}

int instr_id(const std::string& name) {
    for (int i = 0; i < 20; ++i)
        if (name == INSTRUMENTS[i]) return i;
    throw std::runtime_error("未知音色: " + name);
}

std::string keyname(int key) {
    const int m = 54 + key;
    return std::string(NOTE_NAMES[pymod(m, 12)]) + std::to_string(floordiv(m, 12) - 1);
}

std::string user_keyname(int key) {
    const int m = key + 21;
    return std::string(NOTE_NAMES[pymod(m, 12)]) + std::to_string(floordiv(m, 12) - 1);
}

int midi_of_key(int key) { return 54 + key; }
int key_of_midi(int midi) { return midi - 54; }

std::string dec_text(const std::string& bytes) {
    // 先严格按 UTF-8 解；失败则按 cp1252（不可解字节 -> U+FFFD）
    if (auto cps = utf8_decode_strict(bytes)) return bytes;
    std::vector<uint32_t> out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes) out.push_back(cp1252_decode_byte(b));
    return utf8_encode(out);
}

namespace {

std::string cp1252_encode_replace(const std::string& utf8) {
    std::string out;
    for (auto cp : utf8_decode(utf8)) {
        char c;
        if (cp1252_encode_char(cp, c))
            out.push_back(c);
        else
            out.push_back('?');
    }
    return out;
}

}  // namespace

std::string ascii_safe(const std::string& utf8) {
    // 整体能进 cp1252 就原样返回
    if (is_cp1252_encodable(utf8)) return utf8;

    std::string out;  // 始终是 UTF-8（与 Python 的 str 语义一致）
    for (auto cp : utf8_decode(utf8)) {
        char c;
        if (cp1252_encode_char(cp, c)) {
            out += utf8_encode(cp);
            continue;
        }
        if (cp >= 0x4E00 && cp <= 0x9FFF) {
            const std::string py = pinyin_first(cp);
            if (!py.empty()) {
                out += py;
                continue;
            }
        }
        // '—' '–' '─'
        if (cp == 0x2014 || cp == 0x2013 || cp == 0x2500) {
            out.push_back('-');
        } else if (cp == 0xFF08 || cp == 0xFF09) {  // （）
            out.push_back(cp == 0xFF08 ? '(' : ')');
        } else if (cp == 0x00B7 || cp == 0x30FB) {  // · ・
            out.push_back('-');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string enc_text(const std::string& utf8, bool cp1252_safe) {
    if (cp1252_safe) return cp1252_encode_replace(ascii_safe(utf8));
    return utf8;
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

namespace {

std::string read_string(const std::string& raw, size_t& o) {
    if (o + 4 > raw.size()) throw std::runtime_error("NBS 文件不完整（字符串长度字段被截断）");
    const int32_t n = rd_i32(raw, o);
    if (n < 0 || n > 1048576)
        throw std::runtime_error("NBS 文件损坏：第 " + std::to_string(o) + " 字节处的字符串长度异常（" +
                                 std::to_string(n) + "）");
    if (o + 4 + static_cast<size_t>(n) > raw.size())
        throw std::runtime_error("NBS 文件不完整（第 " + std::to_string(o) + " 字节处的字符串越过文件末尾）");
    std::string body = raw.substr(o + 4, static_cast<size_t>(n));
    o = o + 4 + static_cast<size_t>(n);
    return dec_text(body);
}

}  // namespace

std::optional<HeaderInfo> probe_header(const std::string& raw, int o) {
    try {
        size_t p = static_cast<size_t>(o);
        if (p + 2 > raw.size()) return std::nullopt;
        const int first = rd_u16(raw, p);
        p += 2;
        if (first != 0) return std::nullopt;
        if (p + 2 > raw.size()) return std::nullopt;
        const int ver = static_cast<uint8_t>(raw[p]);
        p += 1;
        const int vi = static_cast<uint8_t>(raw[p]);
        p += 1;
        if (!(ver == 1 || ver == 2 || ver == 3 || ver == 4 || ver == 5 || ver == 6)) return std::nullopt;
        if (!(vi >= 1 && vi <= 64)) return std::nullopt;

        int song_length = first;
        if (ver >= 3) {
            if (p + 2 > raw.size()) return std::nullopt;
            song_length = rd_u16(raw, p);
            p += 2;
        }
        if (p + 2 > raw.size()) return std::nullopt;
        const int nlay = rd_u16(raw, p);
        p += 2;
        if (!(nlay > 0 && nlay <= 4096)) return std::nullopt;

        std::vector<std::string> names;
        for (int i = 0; i < 4; ++i) names.push_back(read_string(raw, p));

        if (p + 2 > raw.size()) return std::nullopt;
        const int tempo = rd_u16(raw, p);
        if (!(tempo >= 50 && tempo <= 20000)) return std::nullopt;
        p += 2;
        p += 3;
        if (p < 1 || p > raw.size()) return std::nullopt;
        const int ts = static_cast<uint8_t>(raw[p - 1]);
        p += 20;
        const std::string origin = read_string(raw, p);

        int score = 0;
        if (ver >= 4 && p + 4 > raw.size()) return std::nullopt;
        if (!names[0].empty()) score += 4;
        if (!names[1].empty()) score += 4;
        if (ts >= 1 && ts <= 16) score += 2;
        if (vi == 10 || vi == 16 || vi == 20) score += 2;
        if (!origin.empty()) score += 1;

        HeaderInfo info;
        info.score = score;
        info.offset = o;
        info.ver = ver;
        info.vi = vi;
        info.song_length = song_length;
        info.nlay = nlay;
        info.tempo = tempo;
        info.names = std::move(names);
        return info;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<HeaderInfo> detect_header(const std::string& raw) {
    std::vector<HeaderInfo> cands;
    for (int off : {0, 2, 3}) {
        if (auto h = probe_header(raw, off)) cands.push_back(*h);
    }
    if (cands.empty()) return std::nullopt;
    std::stable_sort(cands.begin(), cands.end(), [](const HeaderInfo& a, const HeaderInfo& b) { return a.score > b.score; });
    return cands.front();
}

Song load(const std::string& path) {
    const std::string raw = read_file(path);

    auto info = detect_header(raw);
    if (!info) throw std::runtime_error("不是有效的 NBS 文件：" + path);

    Song s;
    s.path = path;
    s.version = info->ver;
    s.vanilla_instrument_count = info->vi;
    s.has_magic = raw.size() >= 3 && raw.compare(0, 3, "NBS") == 0;

    size_t o = static_cast<size_t>(info->offset) + 2 + 1 + 1;
    if (s.version >= 3) {
        if (o + 2 > raw.size()) throw std::runtime_error("truncated song_length");
        s.song_length = rd_u16(raw, o);
        o += 2;
    }
    if (o + 2 > raw.size()) throw std::runtime_error("truncated layer_count");
    const int nlay = rd_u16(raw, o);
    o += 2;
    s.name = read_string(raw, o);
    s.author = read_string(raw, o);
    s.orig_author = read_string(raw, o);
    s.description = read_string(raw, o);
    if (o + 2 > raw.size()) throw std::runtime_error("truncated tempo");
    s.tempo = rd_u16(raw, o);
    o += 2;
    o += 3;
    if (o < 1 || o > raw.size()) throw std::runtime_error("truncated time signature");
    s.time_signature = static_cast<uint8_t>(raw[o - 1]);
    o += 20;
    s.midi_filename = read_string(raw, o);
    if (s.version >= 4) {
        if (o + 1 > raw.size()) throw std::runtime_error("truncated loop");
        s.loop = static_cast<uint8_t>(raw[o]);
        o += 1;
        o += 1;
        if (o + 2 > raw.size()) throw std::runtime_error("truncated loop_start");
        s.loop_start = rd_u16(raw, o);
        o += 2;
    }

    s.layers.clear();
    s.layers.reserve(static_cast<size_t>(nlay));
    for (int i = 0; i < nlay; ++i) {
        Layer L;
        L.id = i;
        s.layers.push_back(std::move(L));
    }

    size_t p = o;
    int tick = -1;
    while (true) {
        if (p + 2 > raw.size()) break;
        const int jt = rd_i16(raw, p);
        p += 2;
        if (jt == 0) break;
        tick += jt;
        int layer = -1;
        while (true) {
            if (p + 2 > raw.size()) {
                p = raw.size();
                break;
            }
            const int jl = rd_i16(raw, p);
            p += 2;
            if (jl == 0) break;
            layer += jl;
            if (p + 2 > raw.size()) {
                p = raw.size();
                break;
            }
            const int inst = static_cast<uint8_t>(raw[p]);
            const int key = static_cast<uint8_t>(raw[p + 1]);
            p += 2;
            int vel = 100, pan = 100, fp = 0;
            if (s.version >= 4) {
                if (p + 4 > raw.size()) {
                    p = raw.size();
                    break;
                }
                vel = static_cast<uint8_t>(raw[p]);
                pan = static_cast<uint8_t>(raw[p + 1]);
                fp = rd_i16(raw, p + 2);
                p += 4;
            }
            if (layer >= 0 && layer < nlay) {
                Note n;
                n.tick = tick;
                n.layer = layer;
                n.instrument = inst;
                n.key = key;
                n.velocity = vel;
                n.panning = pan;
                n.pitch = fp;
                s.layers[static_cast<size_t>(layer)].notes.push_back(n);
            }
        }
        if (p >= raw.size()) break;
    }

    try {
        for (auto& L : s.layers) {
            L.name = read_string(raw, p);
            if (s.version >= 4) {
                if (p + 1 > raw.size()) throw std::runtime_error("truncated lock");
                L.lock = static_cast<uint8_t>(raw[p]);
                p += 1;
            }
            if (p + 1 > raw.size()) throw std::runtime_error("truncated volume");
            L.volume = static_cast<uint8_t>(raw[p]);
            p += 1;
            if (s.version >= 2) {
                if (p + 1 > raw.size()) throw std::runtime_error("truncated panning");
                L.panning = static_cast<uint8_t>(raw[p]);
                p += 1;
            }
        }
    } catch (const std::exception&) {
        // 容忍截断
    }

    try {
        if (p + 1 > raw.size()) throw std::runtime_error("truncated custom count");
        const int nci = static_cast<uint8_t>(raw[p]);
        p += 1;
        for (int i = 0; i < nci; ++i) {
            CustomInstrument ci;
            ci.name = read_string(raw, p);
            ci.file = read_string(raw, p);
            if (p + 2 > raw.size()) throw std::runtime_error("truncated custom instrument");
            ci.pitch = static_cast<uint8_t>(raw[p]);
            ci.press_key = static_cast<uint8_t>(raw[p + 1]);
            p += 2;
            s.custom_instruments.push_back(std::move(ci));
        }
    } catch (const std::exception&) {
        // 容忍截断
    }
    return s;
}

// ---------------------------------------------------------------------------
// 写出
// ---------------------------------------------------------------------------

std::string save_bytes(const Song& s, int version, bool cp1252_safe) {
    const int v = version >= 0 ? version : s.version;
    std::string out;

    auto W8 = [&](int val) { wr_u8(out, static_cast<uint8_t>(val)); };
    auto W16 = [&](int val) { wr_u16(out, static_cast<uint16_t>(val)); };
    auto Wi16 = [&](int val) { wr_i16(out, static_cast<int16_t>(val)); };
    auto Wi32 = [&](int val) { wr_i32(out, val); };
    auto SW = [&](const std::string& txt) {
        const std::string enc = enc_text(txt, cp1252_safe);
        Wi32(static_cast<int>(enc.size()));
        out += enc;
    };

    W16(0);
    W8(v);
    W8(s.vanilla_instrument_count);
    const int dur = std::max(s.song_length, s.duration_ticks() + 1);
    if (v >= 3) W16(std::min(dur, 65535));
    W16(static_cast<int>(s.layers.size()));
    SW(s.name);
    SW(s.author);
    SW(s.orig_author);
    SW(s.description);
    W16(s.tempo);
    W8(0);   // auto_save
    W8(10);  // auto_save_duration
    W8(s.time_signature);
    for (int i = 0; i < 5; ++i) Wi32(0);
    SW(s.midi_filename);
    if (v >= 4) {
        W8(s.loop);
        W8(0);
        W16(s.loop_start);
    }

    // 按 tick 归组（跳过 lock==1 的层）
    std::map<int, std::vector<const Note*>> by_tick;
    for (const auto& L : s.layers) {
        if (L.lock == 1) continue;
        for (const auto& n : L.notes) by_tick[n.tick].push_back(&n);
    }

    int last_tick = -1;
    for (auto& [t, notes] : by_tick) {
        const int jt = t - last_tick;
        if (!(jt > 0 && jt <= 32767))
            throw std::runtime_error("无法写入：tick 顺序错误（tick " + std::to_string(t) + "）");
        Wi16(jt);
        int last_layer = -1;
        std::stable_sort(notes.begin(), notes.end(),
                         [](const Note* a, const Note* b) { return a->layer < b->layer; });
        for (const Note* n : notes) {
            const int jl = n->layer - last_layer;
            if (!(jl > 0 && jl <= 32767))
                throw std::runtime_error("无法写入：同一 tick 内 layer 顺序错误（tick " + std::to_string(t) + "）");
            Wi16(jl);
            W8(n->instrument);
            W8(n->key);
            if (v >= 4) {
                W8(n->velocity);
                W8(n->panning);
                Wi16(n->pitch);
            }
            last_layer = n->layer;
        }
        Wi16(0);
        last_tick = t;
    }
    Wi16(0);

    for (const auto& L : s.layers) {
        SW(L.name);
        if (v >= 4) W8(L.lock);
        W8(L.volume);
        if (v >= 2) W8(L.panning);
    }

    W8(static_cast<int>(s.custom_instruments.size()));
    for (const auto& ci : s.custom_instruments) {
        SW(ci.name);
        SW(ci.file);
        W8(ci.pitch);
        W8(ci.press_key);
    }
    return out;
}

void save(const Song& s, const std::string& path, int version, bool cp1252_safe) {
    const std::string bytes = save_bytes(s, version, cp1252_safe);
    write_file(path, bytes);
}

}  // namespace nyaa

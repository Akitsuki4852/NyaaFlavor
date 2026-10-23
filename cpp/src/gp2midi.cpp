// .gp → .mid（MuseScore）＋ ghost note 清洗 —— gp2midi.py 的移植。
//
// 这里自带一个 mido 兼容的「裸 SMF」读写器（保留所有消息，写出时用 running status），
// 以便与 mido 的输出保持一致；不依赖任何第三方库。
#define NOMINMAX
#include "gp2midi.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include "common.h"

namespace nyaa {
namespace {

namespace fs = std::filesystem;

// ===========================================================================
// 裸 SMF（mido 兼容）
// ===========================================================================
enum class MsgKind { NoteOn, NoteOff, Chan, Meta, Sysex };

struct RawMsg {
    int delta = 0;
    MsgKind kind = MsgKind::Chan;
    int channel = 0;
    int note = 0;
    int velocity = 0;
    int meta_type = -1;      // kind==Meta 时的 meta 类型
    std::string payload;     // Chan/Sysex: 数据字节（Chan 不含 status）；Meta: meta 类型之后的 data
    // Chan 消息另存 status，便于重写
    int status = 0;
};

struct RawTrack {
    std::vector<RawMsg> msgs;
};

struct RawMidi {
    int type = 1;
    int ntracks = 0;
    int tpb = 480;
    std::vector<RawTrack> tracks;
};

std::string slurp(const std::string& p) {
    return read_file(p);
}

int varlen_at(const std::string& d, size_t& o, size_t n) {
    int v = 0;
    for (;;) {
        if (o >= n) return -1;
        const uint8_t b = (uint8_t)d[o++];
        v = (v << 7) | (b & 127);
        if (!(b & 128)) return v;
    }
}

int status_data_len(int status) {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 1;
        default:
            return 2;
    }
}

RawMidi read_raw_midi(const std::string& path) {
    const std::string d = slurp(path);
    if (d.size() < 14 || d.compare(0, 4, "MThd") != 0)
        throw std::runtime_error("不是有效的 MIDI 文件（找不到 MThd 文件头）：" + path);
    const auto be32 = [&](size_t o) {
        return ((uint32_t)(uint8_t)d[o] << 24) | ((uint32_t)(uint8_t)d[o + 1] << 16) |
               ((uint32_t)(uint8_t)d[o + 2] << 8) | (uint32_t)(uint8_t)d[o + 3];
    };
    RawMidi mid;
    mid.type = (int16_t)(((uint8_t)d[8] << 8) | (uint8_t)d[9]);
    mid.ntracks = (int16_t)(((uint8_t)d[10] << 8) | (uint8_t)d[11]);
    mid.tpb = (int16_t)(((uint8_t)d[12] << 8) | (uint8_t)d[13]);
    size_t o = 8 + be32(4);
    while (o + 8 <= d.size() && d.compare(o, 4, "MTrk") == 0) {
        const size_t ln = be32(o + 4);
        const std::string b = d.substr(o + 8, ln);
        o += 8 + ln;
        const size_t n = b.size();
        size_t p = 0;
        int last_status = -1;
        RawTrack tr;
        while (p < n) {
            const int delta = varlen_at(b, p, n);
            if (delta < 0 || p >= n) break;
            int sb = (uint8_t)b[p++];
            std::string peek;
            if (sb < 0x80) {
                if (last_status < 0) throw std::runtime_error("MIDI 文件损坏：running status 缺少状态字节");
                peek.push_back((char)sb);
                sb = last_status;
            } else if (sb != 0xFF) {
                last_status = sb;
            }
            RawMsg m;
            m.delta = delta;
            if (sb == 0xFF) {
                if (p >= n) break;
                const int mt = (uint8_t)b[p++];
                const int len = varlen_at(b, p, n);
                if (len < 0 || p + (size_t)len > n) break;
                m.kind = MsgKind::Meta;
                m.meta_type = mt;
                m.payload = b.substr(p, (size_t)len);
                p += (size_t)len;
                tr.msgs.push_back(std::move(m));
                if (mt == 0x2F) break;  // end_of_track
                continue;
            }
            if (sb == 0xF0 || sb == 0xF7) {
                const int len = varlen_at(b, p, n);
                if (len < 0 || p + (size_t)len > n) break;
                std::string data = b.substr(p, (size_t)len);
                p += (size_t)len;
                if (data.empty() || (uint8_t)data.back() != 0xF7)
                    throw std::runtime_error("MIDI 文件损坏：SysEx 缺少结束字节 0xF7");
                data.pop_back();
                m.kind = MsgKind::Sysex;
                m.payload = data;
                tr.msgs.push_back(std::move(m));
                continue;
            }
            const int want = status_data_len(sb);
            std::string data = peek;
            while ((int)data.size() < want) {
                if (p >= n) break;
                data.push_back(b[p++]);
            }
            if ((int)data.size() < want) break;
            m.kind = MsgKind::Chan;
            m.status = sb;
            m.channel = sb & 15;
            const int hi = sb & 0xF0;
            if (hi == 0x80 || hi == 0x90) {
                m.note = (uint8_t)data[0];
                m.velocity = (uint8_t)data[1];
                m.kind = hi == 0x90 ? MsgKind::NoteOn : MsgKind::NoteOff;
            }
            m.payload = data;
            tr.msgs.push_back(std::move(m));
        }
        mid.tracks.push_back(std::move(tr));
    }
    return mid;
}

void put_varlen(std::string& out, int v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = (uint8_t)(v & 0x7F);
    v >>= 7;
    while (v > 0) {
        buf[n++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    for (int i = n - 1; i >= 0; --i) out.push_back((char)buf[i]);
}

std::string encode_varlen(int v) {
    std::string s;
    put_varlen(s, v);
    return s;
}

// mido 的 fix_end_of_track：合并所有 end_of_track 的时间到最后
std::vector<RawMsg> fix_end_of_track(const std::vector<RawMsg>& msgs) {
    std::vector<RawMsg> out;
    int accum = 0;
    for (const auto& m : msgs) {
        if (m.kind == MsgKind::Meta && m.meta_type == 0x2F) {
            accum += m.delta;
            continue;
        }
        if (accum) {
            RawMsg c = m;
            c.delta += accum;
            out.push_back(std::move(c));
            accum = 0;
        } else {
            out.push_back(m);
        }
    }
    RawMsg eot;
    eot.kind = MsgKind::Meta;
    eot.meta_type = 0x2F;
    eot.delta = accum;
    out.push_back(std::move(eot));
    return out;
}

void write_raw_midi(const RawMidi& mid, const std::string& path) {
    std::string head;
    auto be16 = [&](std::string& s, int v) {
        s.push_back((char)((v >> 8) & 0xFF));
        s.push_back((char)(v & 0xFF));
    };
    auto be32 = [&](std::string& s, uint32_t v) {
        s.push_back((char)((v >> 24) & 0xFF));
        s.push_back((char)((v >> 16) & 0xFF));
        s.push_back((char)((v >> 8) & 0xFF));
        s.push_back((char)(v & 0xFF));
    };
    head += "MThd";
    be32(head, 6);
    be16(head, mid.type);
    be16(head, (int)mid.tracks.size());
    be16(head, mid.tpb);

    std::string body;
    for (const auto& track : mid.tracks) {
        std::string data;
        int running = -1;
        for (const RawMsg& m : fix_end_of_track(track.msgs)) {
            put_varlen(data, m.delta);
            if (m.kind == MsgKind::Meta) {
                data.push_back('\xFF');
                data.push_back((char)m.meta_type);
                data += encode_varlen((int)m.payload.size());
                data += m.payload;
                running = -1;
                continue;
            }
            if (m.kind == MsgKind::Sysex) {
                data.push_back('\xF0');
                data += encode_varlen((int)m.payload.size() + 1);
                data += m.payload;
                data.push_back('\xF7');
                running = -1;
                continue;
            }
            std::string bytes;
            bytes.push_back((char)m.status);
            bytes += m.payload;
            const int status = (uint8_t)bytes[0];
            if (status == running)
                data.append(bytes, 1, std::string::npos);
            else
                data += bytes;
            running = status < 0xF0 ? status : -1;
        }
        body += "MTrk";
        be32(body, (uint32_t)data.size());
        body += data;
    }
    write_file(path, head + body);
}

// ===========================================================================
// ghost note 清洗
// ===========================================================================
int meta_tempo(const RawMsg& m) {
    if (m.payload.size() < 3) return -1;
    return ((uint8_t)m.payload[0] << 16) | ((uint8_t)m.payload[1] << 8) | (uint8_t)m.payload[2];
}

double ms_per_tick(const RawMidi& mid) {
    int tempo = 500000;
    const int ppq = mid.tpb;
    if (!mid.tracks.empty()) {
        for (const auto& m : mid.tracks[0].msgs) {
            if (m.kind == MsgKind::Meta && m.meta_type == 0x51) {
                const int t = meta_tempo(m);
                if (t >= 0) tempo = t;
                break;
            }
        }
    }
    return tempo / 1000.0 / ppq;
}

std::set<int> chromatic_runs(const std::vector<std::array<int, 5>>& short_pairs, int max_gap) {
    // short_pairs: (channel, on_tick, off_tick, pitch, onset_index)
    std::map<int, std::vector<std::array<int, 3>>> by_channel;  // (on, pitch, idx)
    for (const auto& p : short_pairs) by_channel[p[0]].push_back({p[1], p[3], p[4]});
    std::set<int> prot;
    for (auto& kv : by_channel) {
        auto& cn = kv.second;
        std::sort(cn.begin(), cn.end());
        std::vector<std::array<int, 3>> chain;
        for (const auto& x : cn) {
            if (!chain.empty() && x[0] - chain.back()[0] <= max_gap && std::abs(x[1] - chain.back()[1]) == 1) {
                chain.push_back(x);
            } else {
                if (chain.size() >= 3)
                    for (const auto& n : chain) prot.insert(n[2]);
                chain = {x};
            }
        }
        if (chain.size() >= 3)
            for (const auto& n : chain) prot.insert(n[2]);
    }
    return prot;
}

// ===========================================================================
// MuseScore 查找 / 调用
// ===========================================================================
const char* const kCandidates[] = {
    "C:\\Program Files\\MuseScore 4\\bin\\MuseScore4.exe",
    "C:\\Program Files\\MuseScore 4\\bin\\MuseScore3.exe",
    "C:\\Program Files\\MuseScore 3\\bin\\MuseScore3.exe",
    "C:\\Program Files (x86)\\MuseScore 3\\bin\\MuseScore3.exe",
    "/Applications/MuseScore 4.app/Contents/MacOS/mscore",
    "/Applications/MuseScore 3.app/Contents/MacOS/mscore",
};

std::optional<std::string> which_exe(const std::string& name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
    if (!n || n >= 32768) return std::nullopt;
    std::wstring path(buf);
    size_t start = 0;
    while (start <= path.size()) {
        const size_t semi = path.find(L';', start);
        const std::wstring dir = path.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
        if (!dir.empty()) {
            for (const wchar_t* ext : {L".exe", L".bat", L".cmd", L""}) {
                fs::path p = fs::path(dir) / (u8_to_w(name) + ext);
                if (fs::exists(p)) return path_to_u8(p);
            }
        }
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    return std::nullopt;
}

// 运行子进程，捕获 stderr
struct ProcResult {
    int rc = -1;
    std::string err;
};

ProcResult run_process(const std::string& exe, const std::vector<std::string>& args) {
    ProcResult r;
    std::wstring cmd = L"\"" + u8_to_w(exe) + L"\"";
    for (const auto& a : args) cmd += L" \"" + u8_to_w(a) + L"\"";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                  &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return r;
    }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) r.err.append(buf, got);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.rc = (int)code;
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

std::string temp_dir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return n ? w_to_u8(std::wstring(buf, n)) : ".";
}

}  // namespace

std::string find_musescore(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        if (fs::exists(fs::u8path(explicit_path))) return explicit_path;
        throw std::runtime_error("指定的 MuseScore 不存在：" + explicit_path);
    }
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"MUSESCORE", env, MAX_PATH)) {
        const std::string s = w_to_u8(env);
        if (fs::exists(fs::u8path(s))) return s;
    }
    for (const char* c : kCandidates)
        if (fs::exists(fs::u8path(c))) return c;
    for (const char* name : {"mscore4", "mscore3", "mscore", "musescore"})
        if (auto w = which_exe(name)) return *w;
    throw std::runtime_error(
        "未找到 MuseScore 4。请安装 MuseScore 4，或在「高级…」里指定 MuseScore4.exe（命令行可用 --musescore）。");
}

int prune_ghost_notes(const std::string& src, const std::string& dst, double min_ms, bool protect_gliss, int& kept,
                      int& dropped, int& protected_n) {
    RawMidi mid = read_raw_midi(src);
    const double thresh = min_ms / ms_per_tick(mid);
    const int gap = std::max(2, mid.tpb / 12);
    RawMidi out;
    out.type = 1;
    out.tpb = mid.tpb;
    kept = dropped = protected_n = 0;

    for (const auto& tr : mid.tracks) {
        RawTrack nt;
        std::vector<std::pair<int, const RawMsg*>> events;  // (abs tick, msg)
        {
            int t = 0;
            for (const auto& m : tr.msgs) {
                t += m.delta;
                events.emplace_back(t, &m);
            }
        }
        std::map<std::pair<int, int>, std::vector<size_t>> open_notes;
        struct Pair {
            size_t off_i, on_i;
            int on_t, off_t, ch, note;
        };
        std::vector<Pair> pairs;
        for (size_t i = 0; i < events.size(); ++i) {
            const RawMsg& m = *events[i].second;
            if (m.kind == MsgKind::NoteOn && m.velocity > 0) {
                open_notes[{m.channel, m.note}].push_back(i);
            } else if (m.kind == MsgKind::NoteOff || (m.kind == MsgKind::NoteOn && m.velocity == 0)) {
                auto it = open_notes.find({m.channel, m.note});
                if (it != open_notes.end() && !it->second.empty()) {
                    const size_t on_i = it->second.front();
                    it->second.erase(it->second.begin());
                    pairs.push_back({i, on_i, events[on_i].first, events[i].first, m.channel, m.note});
                }
            }
        }
        std::vector<std::array<int, 5>> short_pairs;
        for (const auto& p : pairs)
            if (p.off_t - p.on_t < thresh) short_pairs.push_back({p.ch, p.on_t, p.off_t, p.note, (int)p.on_i});
        std::set<int> protected_ids;
        if (protect_gliss) protected_ids = chromatic_runs(short_pairs, gap);
        protected_n += (int)protected_ids.size();

        std::set<size_t> drop_ids;
        for (const auto& p : pairs) {
            if (p.off_t - p.on_t < thresh) {
                if (protected_ids.count((int)p.on_i)) {
                    kept += 1;
                } else {
                    drop_ids.insert(p.on_i);
                    drop_ids.insert(p.off_i);
                    dropped += 1;
                }
            } else {
                kept += 1;
            }
        }
        int last = 0;
        for (size_t i = 0; i < events.size(); ++i) {
            if (drop_ids.count(i)) continue;
            RawMsg m = *events[i].second;
            m.delta = events[i].first - last;
            last = events[i].first;
            nt.msgs.push_back(std::move(m));
        }
        out.tracks.push_back(std::move(nt));
    }
    write_raw_midi(out, dst);
    return 0;
}

int run_gp2midi(const Gp2MidiOptions& opt, const std::function<void(const std::string&)>& out) {
    const fs::path src = fs::u8path(opt.src);
    if (!fs::exists(src)) throw std::runtime_error("找不到文件：" + opt.src);
    const std::string stem = path_to_u8(src.stem());

    if (opt.prune_only) {
        const fs::path dst = opt.out.empty() ? src.parent_path() / fs::u8path(stem + "_noghost.mid") : fs::u8path(opt.out);
        int kept = 0, dropped = 0, prot = 0;
        prune_ghost_notes(opt.src, path_to_u8(dst), opt.min_ms, opt.protect_gliss, kept, dropped, prot);
        char buf[512];
        if (opt.quiet)
            std::snprintf(buf, sizeof buf, "%s：已过滤 %d 个短促杂音（< %.0f 毫秒）",
                          path_to_u8(src.filename()).c_str(), dropped, opt.min_ms);
        else
            std::snprintf(buf, sizeof buf, "%s → %s：保留 %d 个音，删掉 %d 个鬼音（短于 %.0f ms），保留 %d 个滑音",
                          path_to_u8(src.filename()).c_str(), path_to_u8(dst.filename()).c_str(), kept, dropped,
                          opt.min_ms, prot);
        out(buf);
        return 0;
    }

    fs::path dst = opt.out.empty() ? src.parent_path() / fs::u8path(stem + ".mid") : fs::u8path(opt.out);
    const std::string exe = find_musescore(opt.musescore);
    std::optional<TemporaryDirectory> temporary;
    if (!opt.keep_raw) temporary.emplace();
    const fs::path raw = opt.keep_raw ? src.parent_path() / fs::u8path(stem + ".raw.mid")
                                      : temporary->path / fs::u8path(stem + ".raw.mid");
    fs::remove(raw);
    const ProcResult r = run_process(exe, std::vector<std::string>{"-o", path_to_u8(raw), opt.src});
    if (!fs::exists(raw)) {
        std::string err = r.err.substr(0, 400);
        throw std::runtime_error("MuseScore 转换失败（返回码 " + std::to_string(r.rc) +
                                 "）。请先确认这个谱能在 MuseScore 4 里正常打开。" +
                                 (err.empty() ? std::string() : "\n详情：" + err));
    }
    int kept = 0, dropped = 0, prot = 0;
    prune_ghost_notes(path_to_u8(raw), path_to_u8(dst), opt.min_ms, opt.protect_gliss, kept, dropped, prot);
    char buf[512];
    if (opt.quiet)
        std::snprintf(buf, sizeof buf, "%s：已从 Guitar Pro 谱导出，并过滤 %d 个短促杂音（< %.0f 毫秒）",
                      path_to_u8(src.filename()).c_str(), dropped, opt.min_ms);
    else
        std::snprintf(buf, sizeof buf, "%s → %s：保留 %d 个音，删掉 %d 个鬼音（短于 %.0f ms），保留 %d 个滑音",
                      path_to_u8(src.filename()).c_str(), path_to_u8(dst.filename()).c_str(), kept, dropped,
                      opt.min_ms, prot);
    out(buf);
    if (!opt.keep_raw) fs::remove(raw);
    return 0;
}

}  // namespace nyaa

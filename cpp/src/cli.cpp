// 命令行入口 —— python-workflow/midi2nbs.py 与 nbs_postprocess.py 的移植。
#include "cli.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>

#include "common.h"
#include "gp2midi.h"
#include "midi2nbs.h"
#include "nbs.h"
#include "postprocess.h"

namespace nyaa {
namespace {

namespace fs = std::filesystem;

std::string fmt_2(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}
std::string fmt_1(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
std::string fmt_0(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

// 表格对齐：按显示宽度算（UTF-8 解码，CJK 宽度算 2 列），
// 否则中文块名会把右边的列顶歪。
int cp_columns(unsigned cp) {
    if (cp == 0) return 0;
    if (cp < 0x300) return 1;
    // 全角 / 宽字符的主要区间（中文、日文假名、CJK 标点、全角符号）
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6))
        return 2;
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return 2;   // emoji
    return 1;
}

size_t display_width(const std::string& s) {
    size_t width = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = c;
        size_t len = 1;
        if (c >= 0xF0) len = 4, cp = c & 0x07;
        else if (c >= 0xE0) len = 3, cp = c & 0x0F;
        else if (c >= 0xC0) len = 2, cp = c & 0x1F;
        for (size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        width += static_cast<size_t>(cp_columns(cp));
    }
    return width;
}

std::string pad_right(const std::string& s, size_t width) {
    const size_t have = display_width(s);
    return have >= width ? s : s + std::string(width - have, ' ');
}

std::string pad_left(const std::string& s, size_t width) {
    const size_t have = display_width(s);
    return have >= width ? s : std::string(width - have, ' ') + s;
}

// 简易参数解析：--k v / --flag / 位置参数
struct Args {
    std::map<std::string, std::string> opt;   // 带值
    std::map<std::string, int> flag;          // 布尔
    std::vector<std::string> pos;

    bool has(const std::string& k) const { return opt.count(k) || flag.count(k); }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
    bool on(const std::string& k) const {
        auto it = flag.find(k);
        return it != flag.end() && it->second;
    }
    int num(const std::string& k, int d) const {
        auto it = opt.find(k);
        return it == opt.end() ? d : std::atoi(it->second.c_str());
    }
    double dbl(const std::string& k, double d) const {
        auto it = opt.find(k);
        return it == opt.end() ? d : std::atof(it->second.c_str());
    }
};

// 已知的「需要值」的参数名
const char* const kValueOpts[] = {"-o", "--out", "--output", "--grid", "--time", "--gliss", "--key",
                                  "--gp", "--title", "--author", "--orig-author",
                                  "--min-ms", "--musescore", "--strings", "--compare", "--align", "--shift",
                                  "--keep-low", "--bell-offset", "--report", "--in"};

bool needs_value(const std::string& a) {
    for (const char* v : kValueOpts)
        if (a == v) return true;
    return false;
}

Args parse_args(const std::vector<std::string>& argv, size_t from = 0) {
    Args a;
    for (size_t i = from; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (s.size() > 1 && s[0] == '-') {
            if (needs_value(s)) {
                if (i + 1 < argv.size()) {
                    a.opt[s] = argv[++i];
                    if (s == "-o" || s == "--output") a.opt["--out"] = a.opt[s];
                }
            } else {
                a.flag[s] = 1;
            }
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

std::string join_pcs(const std::vector<int>& pcs) {
    std::string s;
    for (size_t i = 0; i < pcs.size(); ++i) {
        if (i) s += ", ";
        s += kPitchClassName[pcs[i]];
    }
    return s;
}

std::string gm_name(int gm) {
    for (const auto& kv : kGmDrumNames)
        if (kv.first == gm) return kv.second;
    return "?";
}

}  // namespace

int run_midi2nbs(const std::vector<std::string>& argv, const OutFn& out) {
    const Args a = parse_args(argv);
    const bool quiet = a.on("--quiet") || a.on("-q");
    if (a.on("--print-maps")) {
        out("=== 默认音色映射（u3002 / OpenNBS 表；oct 列是音色自身八度偏移，默认启用）");
        for (int p = 0; p < 128; ++p) {
            char b[128];
            std::snprintf(b, sizeof b, "  %3d -> %-16s oct=%+d", p, gm_to_nbs(kInstrumentMap[p].name),
                          kInstrumentMap[p].oct);
            out(b);
        }
        out("=== 套鼓槽位（layer 从上到下就是这个顺序）");
        for (const auto& s : kit()) {
            std::string extra;
            if (s.times > 1) {
                extra = " ×" + std::to_string(s.times) +
                        (s.step ? "（隔 " + std::to_string(s.step) + " tick）" : "（同 tick 叠）");
            }
            char b[256];
            std::snprintf(b, sizeof b, "  %-7s %-8s @ %2d (%3s)%s", s.slot.c_str(), s.instrument.c_str(), s.key,
                          key_name(s.key).c_str(), extra.c_str());
            out(b);
            std::string srcs;
            for (const auto& kv : kGmToSlotOrdered)
                if (std::string(kv.second) == s.slot) {
                    if (!srcs.empty()) srcs += ", ";
                    srcs += "GM" + std::to_string(kv.first) + "(" + gm_name(kv.first) + ")";
                }
            out("          ← " + (srcs.empty() ? std::string("（表外音高，默认按 u3002 / OpenNBS 表单独成层）") : srcs));
        }
        return 0;
    }

    if (a.flag.count("--help") || a.flag.count("-h")) {
        out("用法: NyaaFlavor-cli <输入.mid|输入.gp> [选项]");
        out("  doctor                 检查环境（MuseScore / NBS 读写）");
        out("  -o, --out FILE         输出文件（同时写同名的 _A.nbs）");
        out("  --grid 4|8|16          每拍几条 NBS tick（默认 8）");
        out("  --time grid|real       时间换算方式（real 按真实秒数）");
        out("  --gliss diatonic|keep  滑音处理（默认 diatonic）");
        out("  --key Ab|C#m           手写调名");
        out("  --title / --author / --orig-author   写进文件头的名字");
        out("  --no-octave-modifier   不使用音色自身八度差（key 一律 = MIDI−21）");
        out("  --keep-order           按 MIDI 轨顺序排版");
        out("  --no-separators        不插静音分隔轨");
        out("  --strings cp1252|utf8  字符串编码（默认 cp1252；utf8 会让 NBS 4 导出音频失败）");
        out("  --compare FILE         与参考 .nbs 对比打分");
        out("  --align FILE           自动求 tick 偏移");
        out("  --shift N              手动 tick 偏移");
        out("  --keep-low N           节奏块里保留几个 guitar（默认 3）");
        out("  --bell-offset N        bell 相对合并音的偏移（默认 -12）");
        out("  --min-ms N             短音阈值毫秒（默认 60）");
        out("  --musescore PATH       指定 MuseScore 程序");
        out("  --no-gp                不自动使用同名 .gp/.gpx");
        out("  --no-drum-fallback   表外打击乐退回军鼓");
        out("  --keep-midi            保留 ① 导出的 MIDI");
        out("  第 ③ 步逐个关掉：--no-fold / --no-rhythm / --no-merge / --no-borrow /");
        out("                    --no-borrow-table / --no-clash / --no-dedupe / --drop-empty / --no-tidy");
        out("  -q, --quiet            只打印结果，不打印统计明细");
        out("  --force / --dry-run / --print-maps / --report FILE");
        out("  -h, --help             显示这段帮助");
        return 0;
    }
    if (a.pos.empty()) {
        out("需要一个输入文件（.mid / .gp / .gpx），或者运行 `NyaaFlavor-cli doctor`");
        out("加 --help 看全部选项");
        return 2;
    }

    const fs::path midi_path = fs::u8path(a.pos[0]);
    if (!fs::exists(midi_path)) {
        out("找不到输入：" + a.pos[0]);
        return 2;
    }
    fs::path outp = a.opt.count("--out") ? fs::u8path(a.opt.at("--out")) : midi_path;
    if (!a.opt.count("--out")) outp.replace_extension(".nbs");
    if (fs::exists(outp) && !a.on("--force") && !a.on("--dry-run")) {
        out("输出已存在：" + path_to_u8(outp) + "（加 --force 覆盖）");
        return 2;
    }

    Options opt;
    opt.grid = a.num("--grid", 8);
    opt.octave_modifier = !a.on("--no-octave-modifier");
    opt.keep_order = a.on("--keep-order");
    opt.drum_fallback = !a.on("--no-drum-fallback");   // 默认开；--no-drum-fallback 关
    opt.separators = !a.on("--no-separators");
    opt.time_mode = a.get("--time", "grid");
    opt.gliss = a.get("--gliss", "diatonic");
    opt.key = a.get("--key");
    opt.gp = a.get("--gp");
    opt.auto_gp = !a.on("--no-gp");

    Header header;
    std::vector<Part> parts;
    Info info;
    convert(path_to_u8(midi_path), opt, header, parts, info);
    header.name = a.get("--title").empty() ? info.midi_stem : a.get("--title");
    header.author = a.get("--author");
    header.orig_author = a.get("--orig-author");
    std::vector<LayerDef> layers;
    std::vector<NoteOut> notes;
    Report rep;
    layout(parts, opt, layers, notes, rep);

    if (quiet) {
        int dropped_total = 0;
        for (const auto& kv : rep.dropped) dropped_total += kv.second;
        out("生成：" + std::to_string(notes.size()) + " 个音符 / " + std::to_string(layers.size()) + " 条音轨");
        if (dropped_total)
            out("注意：" + std::to_string(dropped_total) + " 个音因同一时刻、同一音轨冲突被跳过");
        if (rep.clamped) out("注意：" + std::to_string(rep.clamped) + " 个音超出可写范围，已移到边缘");
        if (rep.outside_window) {
            const double pct = 100.0 * rep.outside_window / std::max(1, (int)notes.size());
            out("注意：" + std::to_string(rep.outside_window) + " 个音超出音域（" + fmt_1(pct) +
                "%），交给 B 版处理");
        }
        if (!info.unknown_drums.empty())
            out("注意：" + std::to_string(info.unknown_drums.size()) + " 个不常见的打击乐，已暂按军鼓处理");
        {
            std::map<int, int> uniq;
            for (const auto& t : info.tempos) uniq[t.second] += 1;
            if (uniq.size() > 1)
                out("· 曲子中途变速；NBS 只能定速，已按开头 " + fmt_2(info.bpm) + " BPM 转换");
        }
        if (rep.poly_max > 40)
            out("注意：最大同时发声 " + std::to_string(rep.poly_max) + " 个，游戏里可能漏音");
    } else {
        out("=== " + info.midi_name + " → " + path_to_u8(outp.filename()));
        out("  MIDI   : 格式 " + std::to_string(info.fmt) + ", " + std::to_string(info.ntrks) + " 轨, 每拍 " +
            std::to_string(info.ppq) + " tick, " + fmt_2(info.bpm) + " BPM, " + std::to_string(info.tempos.size()) +
            " 个速度事件");
        out("  时间轴 : tempo=" + std::to_string(info.tempo) + " (" + fmt_2(info.tps) + " tick/秒), 1 拍 = " +
            std::to_string(opt.grid) + " tick, 量化误差 < " + fmt_1(0.5 / info.tps * 1000) + " ms");
        out("  调性   : " + info.key.name + "（" + info.key.source + "，调内音级 " + join_pcs(info.key.pcs) + "）");
        if (info.gliss.mode == "diatonic") {
            out("  gliss  : " + std::to_string(info.gliss.runs) + " 条半音阶跑句 → 调内装饰音（净减 " +
                std::to_string(info.gliss.dropped) + " 个音；步长阈值 " + std::to_string(info.gliss.min_gap) +
                " MIDI tick）");
        } else {
            out("  gliss  : 原样保留（--gliss keep）");
        }
        if (info.vocal.blocks) {
            const std::string src = info.vocal.gp ? "按 Guitar Pro 谱的 voice 拆" : "只有 MIDI，按和弦+多余音猜";
            out("  人声   : " + src + "；第一声部 " + std::to_string(info.vocal.v1) + " 音 / 上和声 " +
                std::to_string(info.vocal.above) + " / 下和声 " + std::to_string(info.vocal.below));
        }
        out("  生成   : " + std::to_string(layers.size()) + " 条音轨（含分隔轨）, " + std::to_string(notes.size()) +
            " 个音符");
        out("");
        // 列宽按内容算，保证中文块名不会把右边的列顶歪
        size_t title_w = display_width("声部"), inst_w = display_width("音色"), key_w = display_width("音域");
        for (const auto& blk : rep.blocks) {
            title_w = std::max(title_w, display_width(blk.title));
            inst_w = std::max(inst_w, display_width(blk.instrument));
            key_w = std::max(key_w, display_width(std::to_string(blk.key_lo) + ".." + std::to_string(blk.key_hi)));
        }
        out("  " + pad_right("声部", title_w) + " " + pad_right("音色", inst_w) + " " + pad_left("音域", key_w) + " " +
            pad_left("音轨数", 6) + " " + pad_left("音符", 6));
        for (const auto& blk : rep.blocks) {
            const std::string kk = std::to_string(blk.key_lo) + ".." + std::to_string(blk.key_hi);
            out("  " + pad_right(blk.title, title_w) + " " + pad_right(blk.instrument, inst_w) + " " +
                pad_left(kk, key_w) + " " + pad_left(std::to_string(blk.layers), 6) + " " +
                pad_left(std::to_string(blk.notes), 6));
        }
        out("");
        if (!rep.dropped.empty()) {
            std::string s = "  未能写入的重叠音：";
            bool first = true;
            for (const auto& kv : rep.dropped) {
                if (!first) s += "，";
                first = false;
                s += kv.first + " " + std::to_string(kv.second) + " 个";
            }
            out(s);
        }
        if (rep.clamped) out("  ⚠ 超出 0..87 被截到边界的音：" + std::to_string(rep.clamped));
        if (rep.outside_window) {
            const double pct = 100.0 * rep.outside_window / std::max(1, (int)notes.size());
            out("  ⚠ 落在可播放窗口 33..57 之外的音：" + std::to_string(rep.outside_window) + " (" + fmt_1(pct) +
                "%) —— 第 ③ 步会自动折进窗口");
        }
        if (!info.unknown_drums.empty()) {
            std::string s;
            for (size_t i = 0; i < info.unknown_drums.size(); ++i) {
                if (i) s += ", ";
                s += std::to_string(info.unknown_drums[i]);
            }
            out("  ⚠ 默认鼓表里没有的 GM 打击乐音高：[" + s + "]（已按军鼓兜底）");
        }
        {
            std::map<int, int> uniq;
            for (const auto& t : info.tempos) uniq[t.second] += 1;
            if (uniq.size() > 1)
                out("  · MIDI 含 " + std::to_string(info.tempos.size()) + " 个速度事件，NBS 只能定速：按首段 " +
                    fmt_2(info.bpm) + " BPM 写（要严格保时长用 --time real）");
        }
        out("  相邻不到 2 tick 的同音高重复触发：" + std::to_string(rep.retrigger) +
            (rep.retrigger_roll ? "（另有 " + std::to_string(rep.retrigger_roll) + " 处是吊镲三连击，刻意如此）"
                                : ""));
        out("  最大同时发声：" + std::to_string(rep.poly_max) + " 个" +
            (rep.poly_max > 40 ? "  ⚠ 超过 40，Notebot 可能漏音" : ""));
    }

    if (a.on("--dry-run")) return 0;
    write_nbs(path_to_u8(outp), header, layers, notes);
    const auto problems = verify(path_to_u8(outp), (int)notes.size(), (int)layers.size());
    if (!problems.empty()) {
        out("校验失败：");
        for (const auto& p : problems) out("  - " + p);
        return 1;
    }
    Song s = load(path_to_u8(outp));
    int total = 0;
    for (const auto& L : s.layers) total += (int)L.notes.size();
    const int dur = s.duration_ticks();
    if (!quiet) {
        out("");
        out("已写入 " + path_to_u8(outp));
        out("  检查通过：" + std::to_string(total) + " 个音符 / " + std::to_string(s.layers.size()) +
            " 条音轨 / " + fmt_1(s.tick_to_sec(dur)) + " 秒");
    }
    if (a.opt.count("--report")) {
        // 简化版 JSON 报告（字段与 Python 版一致）
        std::string j = "{\n";
        j += "  \"midi\": \"" + path_to_u8(midi_path) + "\",\n";
        j += "  \"output\": \"" + path_to_u8(outp) + "\",\n";
        j += "  \"tempo\": " + std::to_string(info.tempo) + ",\n";
        j += "  \"bpm\": " + fmt_2(info.bpm) + ",\n";
        j += "  \"grid\": " + std::to_string(opt.grid) + ",\n";
        j += "  \"layers\": " + std::to_string(layers.size()) + ",\n";
        j += "  \"notes\": " + std::to_string(notes.size()) + "\n";
        j += "}\n";
        write_file(a.opt.at("--report"), j);
        out("  报告：" + a.opt.at("--report"));
    }
    return 0;
}

int doctor(const OutFn& out) {
    bool ok = true;
    out("NyaaFlavor (C++ / Win32)  静态链接，无 Python / mido 依赖");
    try {
        const std::string ms = find_musescore("");
        out("MuseScore   " + ms + "  ✓");
    } catch (...) {
        out("MuseScore   找不到  —  只有 .gp/.gpx 输入才需要它（装了会自动找到）");
    }
    try {
        // 内存里跑一遍 NBS 读写
        Song song;
        song.tempo = 2000;
        song.song_length = 8;
        Layer L;
        L.id = 0;
        L.name = "probe";
        L.notes.push_back(Note{0, 0, instr_id("harp"), 45});
        song.layers.push_back(L);
        const std::string bytes = save_bytes(song);
        fs::path tmp = fs::temp_directory_path() / "nyaa_probe.nbs";
        try {
            write_file(path_to_u8(tmp), bytes);
        } catch (const std::exception&) {
            // 某些机器的 %TEMP% 被安全软件锁死；退到当前目录再试一次。
            tmp = fs::current_path() / "nyaa_probe.nbs";
            write_file(path_to_u8(tmp), bytes);
        }
        Song back = load(path_to_u8(tmp));
        std::error_code ec;
        fs::remove(tmp, ec);
        if (back.layers.empty() || back.layers[0].notes.size() != 1) throw std::runtime_error("回读不一致");
        out("NBS 读写     ✓");
    } catch (const std::exception& e) {
        ok = false;
        out(std::string("NBS 读写     ✗ ") + e.what());
    }
    out("");
    out("结论：" + std::string(ok ? "环境没问题，可以直接 NyaaFlavor-cli song.mid" : "先把上面标 ✗ 的处理掉"));
    return ok ? 0 : 1;
}

int cli_main(const std::vector<std::string>& argv, const OutFn& out) {
    if (!argv.empty() && argv[0] == "doctor") return doctor(out);

    const Args a = parse_args(argv);
    const bool quiet = a.on("--quiet") || a.on("-q");
    if (a.flag.count("--help") || a.flag.count("-h")) return run_midi2nbs({"--help"}, out);
    if (a.on("--print-maps")) return run_midi2nbs({"--print-maps"}, out);
    if (a.pos.empty()) {
        out("需要一个输入文件（.mid / .gp / .gpx），或者运行 `NyaaFlavor-cli doctor`");
        out("加 --help 看全部选项");
        return 2;
    }
    const fs::path src = fs::u8path(a.pos[0]);
    if (!fs::exists(src)) {
        out("找不到文件：" + a.pos[0]);
        return 2;
    }
    const fs::path base = src.parent_path() / src.stem();
    std::string gp_path;
    fs::path mid_path;
    const std::string ext = py_lower(path_to_u8(src.extension()));
    if (ext == ".gp" || ext == ".gpx") {
        gp_path = a.pos[0];
        mid_path = base;
        mid_path.replace_extension(".mid");
    } else if (a.opt.count("--gp")) {
        gp_path = a.opt.at("--gp");
        mid_path = src;
    } else {
        mid_path = src;
    }

    fs::path a_path, b_path;
    if (a.opt.count("--out")) {
        b_path = fs::u8path(a.opt.at("--out"));
        std::string stem = path_to_u8(b_path.stem());
        std::string a_stem = (stem.size() >= 2 && stem.compare(stem.size() - 2, 2, "_B") == 0)
                                 ? stem.substr(0, stem.size() - 2) + "_A"
                                 : stem + "_A";
        a_path = b_path.parent_path() / fs::u8path(a_stem + ".nbs");
    } else {
        a_path = base.parent_path() / (base.filename().wstring() + L"_A.nbs");
        b_path = base.parent_path() / (base.filename().wstring() + L"_B.nbs");
    }

    std::optional<TemporaryDirectory> temp_midi;
    if (ext == ".gp" || ext == ".gpx") {
        temp_midi.emplace();
        mid_path = temp_midi->path / (src.stem().wstring() + L".mid");
        if (!quiet) out("[1/3] Guitar Pro → MIDI  (" + path_to_u8(src.filename()) + ")");
        Gp2MidiOptions g;
        g.src = a.pos[0];
        g.out = path_to_u8(mid_path);
        g.min_ms = a.dbl("--min-ms", 60.0);
        g.musescore = a.get("--musescore");
        g.keep_raw = false;
        g.quiet = quiet;
        const int rc = run_gp2midi(g, out);
        if (rc) return rc;
        if (a.on("--keep-midi") && !a.on("--dry-run")) {
            const fs::path kept = src.parent_path() / mid_path.filename();
            if (fs::exists(kept) && !a.on("--force")) throw std::runtime_error("中间 MIDI 已存在（使用 --force 覆盖）");
            fs::copy_file(mid_path, kept, fs::copy_options::overwrite_existing);
            out("保留 MIDI：" + path_to_u8(kept));
        }
    } else if (!quiet) {
        out("[1/3] 输入已是 MIDI，跳过 (" + path_to_u8(src.filename()) + ")");
    }

    if (!quiet) out("[2/3] MIDI → A.nbs");
    {
        std::vector<std::string> av = {path_to_u8(mid_path), "-o", path_to_u8(a_path), "--grid",
                                       std::to_string(a.num("--grid", 8))};
        if (a.has("--time")) av.insert(av.end(), {"--time", a.get("--time")});
        if (a.has("--gliss")) av.insert(av.end(), {"--gliss", a.get("--gliss")});
        if (a.has("--key")) av.insert(av.end(), {"--key", a.get("--key")});
        if (a.on("--no-octave-modifier")) av.push_back("--no-octave-modifier");
        if (a.on("--keep-order")) av.push_back("--keep-order");
        if (a.on("--no-separators")) av.push_back("--no-separators");
        if (a.on("--no-drum-fallback")) av.push_back("--no-drum-fallback");
        if (a.has("--title")) av.insert(av.end(), {"--title", a.get("--title")});
        if (a.has("--author")) av.insert(av.end(), {"--author", a.get("--author")});
        if (a.has("--orig-author")) av.insert(av.end(), {"--orig-author", a.get("--orig-author")});
        if (a.on("--no-gp")) av.push_back("--no-gp");
        else if (!gp_path.empty()) av.insert(av.end(), {"--gp", gp_path});
        if (a.on("--dry-run")) av.push_back("--dry-run");
        if (a.on("--force")) av.push_back("--force");
        if (quiet) av.push_back("--quiet");
        const int rc = run_midi2nbs(av, out);
        if (rc) return rc;
    }

    // dry-run 只统计：A 没有落盘，第 ③ 步没有输入可读。
    if (a.on("--dry-run")) {
        out("（dry-run：只统计，未写入文件）");
        return 0;
    }

    if (!quiet) out("[3/3] A.nbs → B.nbs");
    {
        PostOptions p;
        p.src = path_to_u8(a_path);
        p.dst = path_to_u8(b_path);
        p.cp1252_safe = a.get("--strings", "cp1252") == "cp1252";
        p.quiet = quiet;
        if (a.on("--no-fold")) p.fold = false;
        if (a.on("--no-rhythm")) p.rhythm = false;
        if (a.on("--no-merge")) p.merge = false;
        if (a.on("--no-borrow")) p.borrow = false;
        if (a.on("--no-borrow-table")) p.borrow_table = false;
        if (a.on("--no-clash")) p.clash = false;
        if (a.on("--no-dedupe")) p.dedupe = false;
        if (a.on("--drop-empty")) p.drop_empty = true;
        if (a.on("--no-tidy")) p.tidy = false;
        if (a.has("--compare")) p.compare = a.get("--compare");
        if (a.has("--align")) p.align = a.get("--align");
        if (a.has("--shift")) {
            p.shift = a.num("--shift", 0);
            p.shift_set = true;
        }
        if (a.has("--keep-low")) p.keep_low = a.num("--keep-low", 3);
        if (a.has("--bell-offset")) p.bell_offset = a.num("--bell-offset", -12);
        const int rc = run_postprocess(p, out);
        if (rc) return rc;
    }

    if (quiet) {
        out("可编辑版 A：" + path_to_u8(a_path));
        out("成品 B：" + path_to_u8(b_path));
        out("用 OpenNBS 打开成品 B 即可试听。");
    } else {
        out("");
        out("完成：");
        out("  可编辑版 A（模板直出，方便手改）：" + path_to_u8(a_path));
        out("  成品 B（已整理音域/音色）：      " + path_to_u8(b_path));
        out("  用 OpenNBS / webnbs.com 打开成品 B 即可试听");
    }
    return 0;
}

}  // namespace nyaa

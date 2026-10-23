#pragma once
// .gp → .mid（MuseScore + ghost note 清洗）—— gp2midi.py 的 C++ 移植。
#include <functional>
#include <string>

namespace nyaa {

struct Gp2MidiOptions {
    std::string src;                  // .gp/.gpx，或 --prune-only 时的 .mid
    std::string out;                  // 空 = 自动
    double min_ms = 60.0;
    std::string musescore;            // 空 = 自动找
    bool keep_raw = false;
    bool prune_only = false;
    bool protect_gliss = true;        // --no-protect-gliss 时置 false
    bool quiet = false;               // 只打印一句结果，不打印清洗统计
};

// 找不到返回空字符串
std::string find_musescore(const std::string& explicit_path);

// 返回退出码（0 成功）
int run_gp2midi(const Gp2MidiOptions& opt, const std::function<void(const std::string&)>& out);

// 仅清洗 ghost note（不动 MuseScore）
int prune_ghost_notes(const std::string& src, const std::string& dst, double min_ms, bool protect_gliss, int& kept,
                      int& dropped, int& protected_n);

}  // namespace nyaa

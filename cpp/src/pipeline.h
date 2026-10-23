#pragma once
// GUI 用的流水线包装（见 pipeline.cpp）。
#include <functional>
#include <string>

namespace nyaa {

struct ConvertOptions {
    int grid = 8;                 // 每拍 tick
    std::string gliss = "diatonic";  // diatonic | keep
    bool borrow_table = true;     // 音色借用表
    bool drum_fallback = true;    // 表外打击乐按 GM 打击乐表兜底
    std::string musescore;
    std::string key;
    std::string time_mode = "grid";
    double min_ms = 60.0;
    bool keep_midi = false;
    bool use_gp = true;
};

struct ConvertResult {
    int ok = 0;          // 成功转换的文件数
    int failed = 0;
    std::string message;
};

// input: .mid/.midi/.gp/.gpx 的路径（UTF-8）
// out_b: 目标 _B.nbs 路径（UTF-8）
// log:   逐行日志回调（UTF-8，无换行）
ConvertResult convert_pipeline(const std::string& input, const std::string& out_b, const ConvertOptions& opt,
                               const std::function<void(const std::string&)>& log);

}  // namespace nyaa

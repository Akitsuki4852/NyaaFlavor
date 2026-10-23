#pragma once
// A.nbs → B.nbs 后处理 —— python-workflow/nbs_postprocess.py 的 C++ 移植。
#include <functional>
#include <string>

namespace nyaa {

struct PostOptions {
    std::string src, dst, compare, align;
    int shift = 0;              // 0 = 未指定
    bool shift_set = false;
    int keep_low = 3;
    int bell_offset = -12;
    bool fold = true;
    bool rhythm = true;
    bool merge = true;
    bool borrow = true;
    bool borrow_table = true;
    bool clash = true;
    bool dedupe = true;
    bool drop_empty = false;
    bool tidy = true;
    bool cp1252_safe = true;
    bool quiet = false;         // 只打印结果行，不打印逐项统计
};

// 返回退出码（0 成功）。日志逐行回调。
int run_postprocess(const PostOptions& opt, const std::function<void(const std::string&)>& out);

}  // namespace nyaa

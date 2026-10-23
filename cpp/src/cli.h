#pragma once
// 命令行入口。参数名与 python-workflow/ 下的两个脚本保持一致。
#include <functional>
#include <string>
#include <vector>

namespace nyaa {

using OutFn = std::function<void(const std::string&)>;

// 对应 python-workflow/midi2nbs.py 的命令行（只做第 ② 步）
int run_midi2nbs(const std::vector<std::string>& argv, const OutFn& out);

// 完整三段流水线：gp2midi → midi2nbs → nbs_postprocess
int cli_main(const std::vector<std::string>& argv, const OutFn& out);

// 环境自检
int doctor(const OutFn& out);

}  // namespace nyaa

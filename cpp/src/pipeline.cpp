// GUI 用的流水线包装：把选项拼成 CLI 的 argv，走同一条路径。
#include "pipeline.h"

#include <vector>

#include "cli.h"

namespace nyaa {

ConvertResult convert_pipeline(const std::string& input, const std::string& out_b, const ConvertOptions& opt,
                               const std::function<void(const std::string&)>& log) {
    //   argv = [f, '--out', out/<stem>_B.nbs, '--force', '--grid', N, '--gliss', G, ...]
    std::vector<std::string> argv = {input, "--out", out_b, "--force", "--quiet", "--grid",
                                    std::to_string(opt.grid), "--gliss", opt.gliss};
    if (!opt.borrow_table) argv.push_back("--no-borrow-table");
    if (!opt.drum_fallback) argv.push_back("--no-drum-fallback");
    if (!opt.musescore.empty()) argv.insert(argv.end(), {"--musescore", opt.musescore});
    if (!opt.key.empty()) argv.insert(argv.end(), {"--key", opt.key});
    argv.insert(argv.end(), {"--time", opt.time_mode, "--min-ms", std::to_string(opt.min_ms)});
    argv.insert(argv.end(), {"--tempo-mode", opt.tempo_mode});
    if (opt.keep_midi) argv.push_back("--keep-midi");
    if (!opt.use_gp) argv.push_back("--no-gp");

    ConvertResult r;
    try {
        const int rc = cli_main(argv, log);
        if (rc == 0)
            r.ok = 1;
        else {
            r.failed = 1;
            // 面向用户的原因，而不是裸的进程退出码
            if (rc == 2)
                r.message = "无法读取输入文件，或输出路径不可用";
            else
                r.message = "转换失败，请查看下面的记录";
        }
    } catch (const std::exception& e) {
        r.failed = 1;
        r.message = e.what();
    }
    return r;
}

}  // namespace nyaa

// 控制台入口（与图形界面共用同一份核心）。
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "common.h"

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(nyaa::w_to_u8(argv[i]));
    try {
        return nyaa::cli_main(args, [](const std::string& s) { std::printf("%s\n", s.c_str()); });
    } catch (const std::exception& e) {
        std::printf("错误: %s\n", e.what());
        return 1;
    }
}

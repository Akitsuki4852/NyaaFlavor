# NyaaFlavor — C++ / Win32 版

`python-workflow/` 的 C++20 移植。转换核心、NBS 读写、中文拼音表全部编译进 EXE，
静态链接运行库，没有 Python / .NET / 第三方 DLL 依赖。MIDI 转换完全离线。

## 直接用

仓库不含构建产物。先 `.\build.ps1 all`，或从 GitHub Releases 下载；生成后：
- `artifacts/release/NyaaFlavor.exe`（约 780 KB）：双击，把 `.mid` / `.gp` / `.gpx` 拖进窗口，
  结果在旁边的 `输出/`：`<名字>_A.nbs`（可编辑版）和 `<名字>_B.nbs`（成品）。
- `artifacts/release/NyaaFlavor-cli.exe`：命令行版，共用同一套核心。
  `NyaaFlavor-cli.exe 输入.mid -o 输出_B.nbs`；加 `--quiet` 只看结果，`--help` 看全部选项，`doctor` 检查环境。

`.gp` / `.gpx` 需要安装 MuseScore 4（自动查找，也可用 `--musescore` 指定）；`.mid` 不需要。

## 自己编

需要 Visual Studio 的「使用 C++ 的桌面开发」组件；`build.ps1` 用 vswhere 自动找工具链。
如果从 GitHub ZIP 下载后被 PowerShell 执行策略拦下，用：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 all
```

正常情况直接：

```powershell
.\build.ps1 cli     # 命令行版
.\build.ps1 gui     # 图形版
.\build.ps1 all     # 两个都编
.\package.ps1       # 编完复制到 artifacts\release\ 并打 zip（含 LICENSE / THIRD_PARTY_NOTICES）
```

默认按体积优化（`/O1 /Os /GL /MT`，链接 `/LTCG /OPT:REF /OPT:ICF`）。

## 源码

| 文件 | 功能 |
|---|---|
| `src/gui.cpp` | 原生 Win32 界面与后台批处理 |
| `src/cli.cpp`、`src/pipeline.*` | 命令行、GUI 共用的流水线 |
| `src/smf.*`、`src/midi2nbs.*`、`src/tables.cpp` | MIDI 解析与转换 |
| `src/nbs.*`、`src/common.*` | NBS 格式、编码、临时文件 |
| `src/postprocess.*` | B 版后处理 |
| `src/gp2midi.*`、`src/gpvoice.*` | MuseScore 调用、ghost note 清理、GP8 声部拆分 |
| `src/pinyin_table.cpp` | 生成的拼音表（`tools/gen_pinyin_table.py`） |
| `src/tables.cpp` | 生成的音色/鼓表（`tools/gen_tables.py`） |
| `third_party/tinyxml2/` | GPIF XML 解析（zlib 许可） |

算法本身来自 `python-workflow/`，同步方式见仓库根 `AGENTS.md`。
生成文件可用 `python tools/gen_tables.py --check` 与 `python tools/gen_pinyin_table.py --check` 校验。

## 测试

仓库根运行：

```powershell
pip install -r python-workflow/requirements.txt -r python-workflow/requirements-dev.txt
python -m pytest
```

CI 会在 Windows 上重新构建 CLI/GUI，并用 demo.mid 做 Python ↔ C++ 的 A/B 字节对拍。

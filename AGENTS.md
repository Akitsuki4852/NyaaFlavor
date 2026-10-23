# AGENTS.md — NyaaFlavor 仓库约定

> 给接管这个仓库的 agent。算法硬知识在 `python-workflow/AGENTS.md`，先读那份。

## 0. 一个事实来源

```
python-workflow/*.py   ← 算法的唯一事实来源
cpp/src/*.cpp          ← 移植版（手工同步，改完必须重编译 + 对拍）
```

`NyaaFlavor-archive/` 是停更的 tkinter 版，只在本地存档，不进版本库，也不要修。

## 1. 改算法的流程

1. 改 `python-workflow/`，跑 `python -m pytest`；再用 `--compare` 看 F1 是否变差；
2. 把改动翻译进 `cpp/src/`（`midi2nbs.py` ↔ `midi2nbs.cpp`，`nbs_postprocess.py` ↔ `postprocess.cpp`，表在 `tables.cpp`）；
3. `cd cpp; .\build.ps1 cli` 重编译（需要 VS 的「使用 C++ 的桌面开发」，脚本用 vswhere 自动找）；
4. **对拍**：demo 与自建样例两边各跑一遍，A 与 B 应该逐字节相同；
5. `cd cpp; .\package.ps1` 刷新 `artifacts/release/`。

`cpp/tables.cpp` 的表由 `python tools/gen_tables.py` 从 Python 侧生成；`pinyin_table.cpp`
由 `python tools/gen_pinyin_table.py` 生成。改表后重新生成并跑 `--check`。

## 2. 对拍命令

```powershell
# Python 侧
python python-workflow/midi2nbs.py 曲子.mid -o py_A.nbs --force
python python-workflow/nbs_postprocess.py --in py_A.nbs --out py_B.nbs

# C++ 侧
cpp\artifacts\release\NyaaFlavor-cli.exe 曲子.mid -o cpp_B.nbs --force

# 比字节（PowerShell 里 fc 是 Format-Custom 的别名，要写 fc.exe）
fc.exe /b py_A.nbs cpp_A.nbs
fc.exe /b py_B.nbs cpp_B.nbs
```

已知的唯一差异源是**标题拼音**：C++ 内置拼音表，Python 没装 `pypinyin` 时会出 `?`。

## 3. 目录职责

| 路径 | 职责 |
|---|---|
| `python-workflow/gp2midi.py` | GP → MIDI，删 ghost note（< 60 ms） |
| `python-workflow/midi2nbs.py` | MIDI → A.nbs（每轨一块、鼓槽位、gliss、电钢 `p4` 标记） |
| `python-workflow/nbs_postprocess.py` | A → B（折叠/借音色/合并/去重/分层） |
| `python-workflow/_agent/` | 两个脚本共用的 `nbs.py` / `smf.py`，别删 |
| `cpp/src/gui.cpp` | Win32 界面 + 批处理 |
| `cpp/src/pinyin_table.cpp` | 生成的拼音表，让 exe 不依赖 pypinyin |
| `tools/gen_tables.py` | 从 `python-workflow/midi2nbs.py` 生成 `cpp/src/tables.cpp` |
| `tools/gen_pinyin_table.py` | 从 pypinyin 生成 `cpp/src/pinyin_table.cpp` |
| `tests/` | pytest：流水线、表一致性、生成文件、旧命名检查 |

# NyaaFlavor

把 Guitar Pro / MIDI 谱转成 **Note Block Studio (`.nbs`)** 格式的一键工具，可直接导入 Notebot。<br>
Turn Guitar Pro / MIDI scores into `.nbs` files for Note Block Studio and Notebot.

![NyaaFlavor](https://akitsuki4852.github.io/Pic/assets/NyaaFlavorGUI.png)

### Contributors

<p>
  <a href="https://github.com/Akitsuki4852" title="Akitsuki4852"><img src="https://github.com/Akitsuki4852.png" width="50" height="50" style="border-radius:50%;" alt="Akitsuki4852"></a>
  <a href="https://github.com/LingYi0110" title="01 / LingYi0110"><img src="https://github.com/LingYi0110.png" width="50" height="50" style="border-radius:50%;" alt="LingYi0110"></a>
  <a href="https://www.deepseek.com/" title="DeepSeek"><img src="https://akitsuki4852.github.io/Pic/assets/DeepSeek.png" width="50" height="50" style="border-radius:50%;" alt="DeepSeek"></a>
  <a href="https://openai.com/" title="GPT"><img src="https://akitsuki4852.github.io/Pic/assets/GPT.png" width="50" height="50" style="border-radius:50%;" alt="GPT"></a>
</p>

**欢迎 PR / Issue。** DeepSeek / GPT 图标表示 AI 辅助，不作为版权持有人。

## ⬇️ 下载

<div align="center">
  <a href="https://github.com/Akitsuki4852/NyaaFlavor/releases/latest">
    <img src="https://img.shields.io/badge/%E7%82%B9%E5%87%BB%E4%B8%8B%E8%BD%BD%20%2F%20Download-NyaaFlavor.zip-brightgreen?style=for-the-badge&logo=windows" alt="Download">
  </a>
</div>

---

## 如何使用

下载 Release 压缩包（约 800 KB，Windows 10/11 64 位），解压后双击 `NyaaFlavor.exe` 打开。<br>
由于 exe 未签名，首次运行可能被 **SmartScreen** 拦截：点「更多信息」→「仍要运行」。<br>
把 `.mid` / `.gp` / `.gpx` 拖进窗口（或点「添加文件」，也可以直接拖到 exe 上）。<br>
点「③ 开始转换」。结果在旁边的 `输出/`（或你指定的目录），会得到两个文件：<br>
`<名字>_A.nbs` 保留原谱结构，方便在 NBS 里手改；`<名字>_B.nbs` 已整理音域/音色，可直接用。

两种输入都不联网：`.mid` 不需要额外软件；`.gp` 需要安装 MuseScore 4（会自动找）。<br>
转换是启发式整理，复杂曲子仍建议在 NBS 里手改一遍。

它帮你做什么：<br>
1. 删掉 GP 导出产生的 ghost note（< 60 ms 的短促杂音）；<br>
2. 按声部分层，自动认人声 / 主音 / 节奏 / 贝斯 / 鼓；<br>
3. 鼓组合并成套鼓槽位，表外打击乐单独成层；<br>
4. B 版自动折叠音域、借音色、去重、整理版面。

---

## 仓库里有什么

| 目录 | 是什么 |
|---|---|
| `cpp/` | C++ / Win32 移植，编译成单文件 exe |
| `python-workflow/` | 三个脚本：`gp2midi.py` → `midi2nbs.py` → `nbs_postprocess.py`，可以用 Python 跑，逻辑更好读 |
| `tools/` | 生成 C++ 音色表 / 拼音表 |
| `tests/` | pytest：流水线、表一致性、生成文件、旧命名检查 |

[`STYLE.md`](STYLE.md) 写了这套规则从哪来：Notebot / NBS 的客观限制，以及本项目的风格取舍。

### C++

使用 `cpp/build.ps1` 构建 exe（需要 VS 的「使用 C++ 的桌面开发」）。<br>
或直接用命令行版：

```powershell
NyaaFlavor-cli.exe 输入.mid -o 输出_B.nbs
```

`--help` 看选项，`doctor` 检查 MuseScore / NBS 读写环境。

### Python

```powershell
python python-workflow/gp2midi.py      "谱.gp"   -o out/曲子.mid      # ① GP → MIDI（删 ghost note）
python python-workflow/midi2nbs.py     "曲子.mid" -o out/曲子_A.nbs    # ② MIDI → A
python python-workflow/nbs_postprocess.py --in out/曲子_A.nbs --out out/曲子_B.nbs   # ③ A → B
```

依赖：Python 3.10+、`mido`；`.gp` 还需要 MuseScore；中文标题转拼音需要 `pypinyin`。<br>
一次装齐：`pip install -r python-workflow/requirements.txt`。<br>
试跑（②③ 两步；② 产出的是 A，不是成品）：

```powershell
python python-workflow/midi2nbs.py python-workflow/samples/demo.mid -o out/demo_A.nbs
python python-workflow/nbs_postprocess.py --in out/demo_A.nbs --out out/demo_B.nbs
```

详细用法和算法说明见仓库里的 `python-workflow/README.md`。

## 开发

```powershell
pip install -r python-workflow/requirements.txt -r python-workflow/requirements-dev.txt
python -m pytest
```

C++ 表由 Python 数据生成，改表后跑 `python tools/gen_tables.py`；
拼音表由 `python tools/gen_pinyin_table.py` 生成。两者都支持 `--check` 校验是否过期。

## 许可

MIT（`LICENSE`）。GM 映射表来自 u3002 / OpenNBS（MIT）；拼音表来自 pypinyin（MIT）；
tinyxml2 是 zlib。完整声明见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

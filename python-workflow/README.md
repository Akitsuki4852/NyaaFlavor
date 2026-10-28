# `python-workflow/` — 从 Guitar Pro 谱到 Akitsuki 风格 `.nbs` 的三段流水线

```
 song.gp ──[gp2midi]──> song.mid ──[midi2nbs]──> A.nbs ──[nbs_postprocess]──> B.nbs
           (MuseScore)            (模板直出)            (风格后处理)
           + 删 ghost note
```

三段各自独立、都可单独重跑；每段的输出都是合法 `.nbs`，可以随时用 OpenNBS 打开检查。

| 阶段 | 脚本 | 输入 → 输出 | 干什么 |
|---|---|---|---|
| ① gp→midi | `gp2midi.py` | `.gp` → `.mid` | MuseScore 导出 + **删掉 ghost note**（时长 < 60 ms）|
| ② midi→nbs | `midi2nbs.py` | `.mid` → `A.nbs` | 模板直出：每轨一块、鼓组套槽位、gliss 压成装饰音；**不做任何折叠** |
| ③ nbs 后处理 | `nbs_postprocess.py` | `A.nbs` → `B.nbs` | 折叠 / 节奏吉他转 bit / 合并重复主音 / 去重 / 分层 |

---

## 0 · 环境

| 依赖 | 用途 | 备注 |
|---|---|---|
| Python 3.10+ | 全部脚本 | ②③ 只用标准库；① 另需 `mido` |
| `mido` | ① 删 ghost | `pip install mido` |
| `pypinyin` | ②③ 中文曲名/层名转拼音（NBS 4 音频导出器按 cp1252 解码）| `pip install pypinyin`，不装则中文会变成 `?` |
| MuseScore 4 | ① `.gp` 导出 | 默认找 `C:\Program Files\MuseScore 4\bin\MuseScore4.exe` |
| `_agent/nbs.py` | 读写 `.nbs` | ② ③ 都依赖它 |

`mido` 和 `pypinyin` 都在 `requirements.txt` 里：`pip install -r python-workflow/requirements.txt`。

`midi2nbs.py` 会在自身目录或上一级找 `_agent/smf.py`，**在仓库根目录运行**最省事：

```powershell
cd NyaaFlavor
```

---

## 1 · `gp2midi.py`

```powershell
python python-workflow/gp2midi.py "song.gp"                 # -> song.mid
python python-workflow/gp2midi.py song.gp -o out.mid --min-ms 60
python python-workflow/gp2midi.py song.gp --keep-raw        # 保留未清理的导出

# 已经有一份 .mid、只想清 ghost（不经过 MuseScore）
python python-workflow/gp2midi.py old.mid --prune-only -o old_clean.mid
```

**为什么删 ghost**：<br>
gp→MIDI 会把闷音扫弦转成一堆 14/50 ms 的短音符，<br>
后面「保留率 / 和弦音数」的统计会被全部污染。

---

## 2 · `midi2nbs.py`

```powershell
python python-workflow/midi2nbs.py "song.mid" `
       --gp "song.gp" -o "out/song_A.nbs"
```

要点：

- **一个 MIDI 轨 = 一个 layer 块**，块之间插静音分隔层（版面语言）；
- **块名 = 声部前缀 + 序号**（`VOCAL1` / `LEAD1` / `RHYTHM1` / `BASS1` / `OTHER1`），
  原轨名跟在 `←` 后面。声部按轨名认（中/英/日文关键词），认不出来时按音区+同时发声数兜底；
  ③ 的 rhythm/lead 规则只认这个前缀，所以 midishow 那种 `Track 1` / `主音吉他` 也能走完整条流程；
- 和弦摊层：同一 tick 的音按音高从低到高放进 0..n-1 层；
- **不做折叠**：出界音原样保留（方便 ③ 判断「这是借音色还是折叠」）；
- 鼓组按**套鼓槽位**合并（见 `AGENTS.md` 的鼓组表），不是按 GM 音高逐条铺；
  表外的 GM 打击乐（沙锤 70、三角铁 81…）默认按 u3002 / OpenNBS 表单独成一条 `GM<n>` 层；
- **电钢（GM 4/5）会带 `p4`/`p5` 标记**（`-- LEAD3 [harp] p4 --`），③ 见到就优先借 flute；
- `--gp` 用来把 GP 的人声声部拆成 pling（第一声部）/ harp（第二声部）；
- gliss（那条半音阶划线）压成「调内装饰音 + 落点音」。

常用开关：`--gliss keep`、`--dry-run`、`--print-maps`、
`--no-drum-fallback`（关掉表外打击乐的 u3002 / OpenNBS 映射，退回军鼓兜底）、
`--tempo-align`（变速曲，见下）、
`--grid 6|12|24`（三连音档，见下）。

**变速曲（`--tempo-align`）**：MIDI 里 tempo 事件不止一个时，默认行为是「按第一个 tempo 写」——
如果第一段是慢速前奏，整首都会被拖慢。加上这个开关后：

- 先按**时长加权**找出「主速度」（大部分时间在用的那个 BPM），用它当 NBS 的 tempo；
- 其余速度段按 `round(grid × 主速度 / 段速度)` 折成**整数 ticks/拍**的阶梯：
  慢速前奏被整数倍拉长，渐快变成 `12→11→10→9→8` 几档；
- 主体段（= 主速度）每拍 tick 数不变，**逐音与原行为只差一个常数偏移**；
- 每 4 拍取一次平均速度、段边界对齐到整拍，误差不累积。

**三连音（`--grid 6 / 12 / 24`）**：默认 1 拍 = 8 tick，三连音（1/3 拍）落在 2.67 tick 上、被四舍五入。
把每拍换成 12 或 24 tick（tempo 跟着一起变，文件时长不变），1/3 拍就是 4 / 8 tick，能精确落位。

| `--grid` | 每拍 tick | 1/8 | 1/16 | 三连音 1/3 | 1/6 |
|---|---|---|---|---|---|
| 8（默认）| 8 | 4 | 2 | 2.67 ✗ | 1.33 ✗ |
| 12 | 12 | 6 | 3 | **4** ✓ | **2** ✓ |
| 24 | 24 | 12 | 6 | **8** ✓ | **4** ✓ |

6 是 8 的 3/4 档（更稀疏的三连音谱够用），12 = 8×3/2，24 = 8×3。`--grid` 不限死这几个，其它整数也照旧可用。
它们都在 `--tempo-align` **之前**生效，阶梯的每拍 tick 数、gliss 阈值、层内间距全按新精度算。

- 实测「初音ミクの消失」：8 tick 时 90.3% 的音精确落位，24 tick 时 **100%**；
- 代价：tick 数 ×3，超过 65535 会截断（240 BPM 下约 11 分钟），程序会提示。

折叠不在这里做——A 阶段的出界音原样保留，交给 ③。

---

## 3 · `nbs_postprocess.py`

```powershell
python python-workflow/nbs_postprocess.py --in A.nbs --out B.nbs
python python-workflow/nbs_postprocess.py --in A.nbs --out B.nbs --compare 参考.nbs   # 打分
python python-workflow/nbs_postprocess.py --in A.nbs --out B.nbs --align 参考.nbs     # 自动求 tick 偏移
```

八步（每步都能用 `--no-*` 关掉）：

| 步 | 规则 | 参数 |
|---|---|---|
| 1 fold | 越界音按八度折进 33..57；**鼓块只折 key、不换音色** | `--no-fold` |
| 2 rhythm | 节奏块内同一 tick 超过 `keep_low` 个 guitar → 高的转 `bit`（key −12） | `--keep-low`（默认 3）|
| 3 merge | 重复的 lead 块（同 tick 同音级）合并成 **`bell` + `bit`**，bell = 合并音 −12 | `--no-merge`、`--bell-offset` |
| 4 borrow | 剩下的单线 lead 高音 → `bit`；bit 也写不下就 → `xylophone`（key −24） | `--no-borrow` |
| 4b borrow | 用顶部 `INSTRUMENTS` 表把越界音换成能弹的音色；**电钢（p4/p5）优先 flute 再 bell**；窗口里没有音色能精确表达时，取「折八度后误差最小」的那组 | `--no-borrow-table` 关掉（**默认开**）|
| 5 clash | 与 lead guitar 撞 (tick,key) 的 `bit` 删掉 | `--no-clash` |
| 6 dedupe | 最后对 `(tick, key, 音色)` 全局去重（所有换音色都做完之后）| `--no-dedupe` |
| 7 layout | **保留静音分隔轨**；bell 层插到人声块下方 | `--drop-empty` |
| 8 tidy | 删掉**空的普通轨**、合并**相邻的静音分隔轨**（`S T T T S F F S T S` → `S T T T S T S T S`）| `--no-tidy` |

`--compare` 会打印两行：**全音**和**非鼓**。鼓是确定性的（MIDI → 槽位表，参考谱的鼓轨
若直接照抄，鼓的 F1 会接近 100%），比较时以非鼓那行为准。

### 为什么 3 必须在 1 之后

lead 的高音如果先被折成 guitar，就再也认不出「两条 lead 是八度齐奏」；
所以顺序是 **先合并（bell+bit）→ 再把剩下的单线高音转 bit**。
顺序反了会把八度齐奏拆成两条单线，bell 层会明显减少。

### 字符串编码（重要）

Note Block Studio 4 的音频导出器用自带的 Python，它把**每个字符串按 cp1252 解码**；
UTF-8 的中文标题会直接抛 `UnicodeDecodeError`。所以默认 `--strings cp1252`：
曲名转拼音（`迷星叫` → `mixingjiao`）、层名与描述全 ASCII。
要保留中文用 `--strings utf8`（界面正常，但**导出音频会失败**）。

### 对齐（`--align`）

gp 导出的 MIDI 常常比成品多 0.5–1 s 前奏，直接比对会整体错位。
`--align 参考.nbs` 用「同 tick 同音级」的重合度自动求偏移；没有参考谱时用 `--shift N` 手填。

---

## 4 · 生成表

`cpp/src/tables.cpp` 由 Python 侧的音色/鼓表生成，`cpp/src/pinyin_table.cpp` 由 pypinyin 生成：

```powershell
python tools/gen_tables.py --check          # C++ 音色/鼓表是否过期
python tools/gen_pinyin_table.py --check    # C++ 拼音表是否过期
```

改表后去掉 `--check` 重新生成；两个脚本都要求 Python 3.10+，拼音表还需要 `pypinyin==0.55.0`。

## 5 · 测试

```powershell
pip install -r python-workflow/requirements.txt -r python-workflow/requirements-dev.txt
python -m pytest
```

测试覆盖 demo 的 ②③ 两步、Python/C++ 表一致性、生成文件是否最新，以及仓库里不再出现旧命名。

---

## 6 · 一轮完整跑法

```powershell
cd NyaaFlavor

# ① gp -> midi（自动删 ghost）
python python-workflow/gp2midi.py "XXX.gp" -o "midi/XXX.mid"

# ② 模板直出
python python-workflow/midi2nbs.py "midi/XXX.mid" --gp "XXX.gp" `
       -o "out/XXX_A.nbs"

# ③ 风格后处理
python python-workflow/nbs_postprocess.py --in "out/XXX_A.nbs" --out "out/XXX_B.nbs"
```

产物：`XXX_A.nbs`（模板）→ `XXX_B.nbs`（可直接用的成品）。

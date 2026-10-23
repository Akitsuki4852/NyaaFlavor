# AGENTS.md — 这个目录的硬知识与约定

> 给接管本目录的 agent。改完跑 `python -m pytest`，再做 Python/C++ 对拍。

## 0. 这个目录是什么

`python-workflow/` 是「Guitar Pro 谱 → `.nbs`」的可复用流水线，三段：

```
song.gp --[gp2midi.py]--> song.mid --[midi2nbs.py]--> A.nbs --[nbs_postprocess.py]--> B.nbs
```

- **A** 是模板直出，**不折叠、不选音色**，出界音原样保留，比 NBS 原始转换更方便手改；
- **B** 是风格化脚本：折叠、节奏吉他转 bit、合并重复主音、去重、分层。
- 详细用法见 `README.md`；风格依据见仓库根的 `STYLE.md`。

## 1. `.nbs` 文件格式（OpenNBS v5/v6）

```
u16 0x0000            ← 新版标记（旧 ASCII "NBS" 已被两个 0 取代）
u8  version           ← 5 或 6
u8  vanilla_instrument_count   ← 现代 20，老文件 16
u16 song_length       ← v>=3
u16 layer_count
str song_name / song_author / original_author / description
u16 tempo             ← ticks per second × 100
u8  auto_save / auto_save_duration / time_signature
i32 ×5  统计
str song_origin       (参考 MIDI 文件名)
u8 loop / max_loop_count / u16 loop_start          (v>=4)
--- 音符流 ---
循环 { i16 tick_delta ; 循环 { i16 layer_delta ; u8 inst, u8 key,
                                [v>=4] u8 velocity, u8 panning+100, i16 pitch }
                         直到 layer_delta == 0 }
直到 tick_delta == 0
--- 音轨 ---
layer_count × { str name ; [v>=4] u8 lock ; u8 volume ; [v>=2] u8 panning+100 }
--- 自定义音色 ---
u8 count × { str name ; str file ; u8 pitch ; u8 press_key }
```

**三个必须记住的坑**：

1. `tick` 与 `layer` 都是**增量**编码；
2. `layer_delta == 0` 表示本 tick 结束 → **同一个 `(tick, layer)` 上不能有两个音**；
   写错会让读取器提前结束该 tick，文件**静默损坏**（音数对不上、tick 跑飞）；
3. `tick_delta` / `layer_delta` 是 `i16`，`song_length` 是 `u16`（跨度 ≤ 65535）。

## 2. 音高与音域

**可播放窗口 = 25 个 key = `33..57`**，`key = midi_pitch − 21`（key 33 = F#3 = MIDI 54）。

> OpenNBS 自己显示的是 `key 0 = F#3`（`midi = key + 54`），
> 所以它把 key 33 显示成 D#6。**同一组 25 个 key，只是八度标签不同。**

**不要逐音"折到中间"**：窗口 25 个 key、跨度 24，大多数音高有两个合法八度，
逐音折会瞎猜。做法是「整声部一个偏移 + 只折例外」，偏移用「总位移最小」定；
**跨度超过 25 个 key 的声部（贝斯/吉他）必须人工钉偏移**——参考成品的贝斯按 +24 写，
自动判据会给 +36，听感完全不同。

## 3. 音源发声八度表（**最容易搞错的一条**）

同一个 NBS key，不同音源实际发声不同：

| 音色 | 相对 harp 的实测发声 |
|---|---|
| `harp` / `bit` / `pling` / `banjo` / `iron_xyl` | **0**（同高）|
| `guitar` | **−12**（低一个八度）|
| `dbass` / `didgeridoo` | **−24**（低两个八度）|
| `flute` / `cow_bell` | **+12**（高一个八度）|
| `bell` / `xylophone` / `icechime` | **+24**（高**两**个八度）|

> key 45 时 harp/bit = 370.0 Hz，
> bell = 1476.7 Hz、xylophone = 1481.7 Hz，正好 ×4 = 两个八度。
> **跨乐器比较音高前必须先换算到发声坐标**，否则会把「八度加厚」当成「同度」。

## 4. 鼓组：真实槽位表

鼓的「音高」不是音高，而是**选哪个套鼓槽位**。同一槽位只占一条 layer，
GM 音高先并进槽位，再按槽位的固定 key 写：

| 槽位 | 音色 | key | 说明 |
|---|---|---|---|
| `HH-C` | sdrum | **55** | 闭镲；骑镲/中国镲/镲帽/铃鼓都并进来 |
| `CRASH` | sdrum | **51** | 吊镲：一次触发写 **t / t+1 / t+2 三连击** |
| `HH-O` | sdrum | 51 | 开镲 |
| `SPLASH` | sdrum | 48 | 水镲 |
| `SNARE` | sdrum | **37** | 军鼓 |
| `TOM-L` | bdrum | 41 | 低嗵 |
| `TOM-M` | bdrum | 43 | 中嗵 |
| `TOM-H` | bdrum | 48 | 高嗵 |
| `BD` | bdrum | **39** | 底鼓 |
| `STICK` | clicks | 35 | 边击 / 拍手 |

GM → 槽位：35/36→BD；37/39→STICK；38/40→SNARE；41/45→TOM-L；43/47→TOM-M；
48/50→TOM-H；42/44/51/52/53/54→HH-C；46→HH-O；49/57→CRASH；55→SPLASH。
（59 不在表里，会走下面的 u3002 / OpenNBS 表兜底，不是 HH-C。）

表外 GM 打击乐（沙锤 70、三角铁 81、木鱼…）**默认**按 u3002 / OpenNBS 打击乐表
单独成一条 `GM<n>` 层：
81 → `icechime`(chime)、70 → `clicks`、25 → `sdrum`等。

> **B 对鼓只折 key，不换音色。** 鼓的 key 只是"选哪个鼓音"，所以 step 1 把
> `GM81` 的 key 19 折成 43 时**不记进 `folded`**，4b 就不会把它借成 harp
> 判断靠 `is_drum_block()`（块名匹配 `DRUMS / GM<n> / HH-C / BD …`）；
> 注意 `DRUMS1 [flute]` 这种"名字像鼓、其实是有音高的轨"不会被误判。

## 5. 高频错误（都实际发生过）

1. **ghost note 没删**：gp→MIDI 会把闷音扫弦写成 14/50 ms 的短音。
   必须在 ① 阶段删（`gp2midi.py`，阈值 60 ms），否则后面所有统计和转换都会被污染。
2. **时轴没对齐**：gp 导出的 MIDI 常比成品多 0.5–1 s 前奏，直接比对会整体错位。
   用 `--align 参考.nbs` 或 `--shift` 求偏移。
3. **GM 音色映射**：overdrive guitar（GM 29）映射成 `guitar`，
   映射成 didgeridoo 会让整条主音变成管乐。
4. **字符串编码（最坑的一条）**：Note Block Studio 4 的音频导出器用自带的 Python，
   把**每个字符串都按 `cp1252` 解码**，遇到 `0x81/0x8d/0x8f/0x90/0x9d` 直接抛 `UnicodeDecodeError`。
   UTF-8 的中文标题（`迷星叫` = `e8 bf b7 e6 98 9f e5 8f ab`）第 7 个字节正好是 `0x8f`
   —— 打开正常，**无法导出音频**。
   所以流水线默认 `--strings cp1252`：中文用拼音（`pypinyin`）、制表符 `──` 换成 `--`、
   描述用英文。要保留中文可加 `--strings utf8`（界面能看，导出会失败）。

## 5b. 音色借用表（脚本 B 顶部的 `INSTRUMENTS`，唯一事实来源）

13 个有音高的音色（鼓与 trumpet 不参与借用）。`offset` = key 33 的实际发声相对 harp；
`up` / `down` 是越界时的首选借用目标，写出来的 key 由公式算：
`key = 目标发声 − offset[目标]`，取第一个落在 33..57 的目标。

| 音色 | offset | 原生音域 | 向上借 | 向下借 |
|---|---|---|---|---|
| harp | 0 | F#3–F#5 | **bell**, **flute** | guitar |
| dbass | -24 | F#1–F#3 | guitar | — |
| guitar | -12 | F#2–F#4 | **bit** | — |
| flute | +12 | F#4–F#6 | bell | harp, bell |
| bell | +24 | F#5–F#7 | xylophone | **harp**, flute |
| icechime | +24 | F#5–F#7 | — | harp, bell |
| xylophone | +24 | F#5–F#7 | — | harp, bit |
| iron_xyl | 0 | F#3–F#5 | cow_bell, xylophone | — |
| cow_bell | +12 | F#4–F#6 | xylophone | harp, iron_xyl |
| didgeridoo | -24 | F#1–F#3 | guitar | — |
| bit | 0 | F#3–F#5 | **xylophone**, bell | **guitar** |
| banjo | 0 | F#3–F#5 | bell | guitar |
| pling | 0 | F#3–F#5 | flute, bell | guitar |

`原生音域` = `span`，是**该音色自己的绝对音区**（与 key 同坐标系：`key 33 = F#3`），
不是"能借到哪"。`—` 表示该方向没有直接目标，但 `borrow()` 会把直接目标再展开一跳
（`guitar → bit → xylophone`），所以列表为空时仍可能借到；都落不进 33..57 才走
「折八度、取误差最小」的兜底。

**加粗**的经过了特殊设计（guitar↔bit、bell→harp、harp→flute/bell、bit→xyl），其余按音域推算默认值。

**默认启用**，要退回纯八度折叠用 `--no-borrow-table`。

## 6. B脚本风格化处理的三条核心行为

- **节奏吉他**：同名 rhythm 块内，同一 tick 的 guitar 超过 3 个时，
  最低 3 个留 guitar，**更高的转 bit（key −12）**；
- **重复主音**：两个 lead 块在同一 tick 音级集合相同 → 合并成 `bell + bit`，
  重复块整块丢弃；
- **去重**：**最后**做（所有换音色、合并、clash 都结束之后），对 `(tick, key, 音色)` **全局**去重：
  跨声部的同一个音也会并掉一个，这是版面约定（NBS 里不留两个完全相同的位置），不是 bug。
  顺序上它排在 layout 之前一步，tidy（删空轨/并分隔轨）再跟在 layout 之后。
- `bell` 层放在**人声块下方**；**静音分隔轨要保留**，它们是版面的一部分；
- **收尾**：删掉「空且不是分隔轨」的层，并把相邻的分隔轨合并成一条
  （`S T T T S F F S T S F S T T S` → `S T T T S T S T T S`）。

**电钢琴特例（GM program 4/5）**：A 里这类块的分隔层会带 `p4`/`p5` 标记
（如 `-- LEAD3 [harp] p4 --`，`midi2nbs.EP_PROGRAMS`），B 见到就把
`prefer=("flute","bell")` 传给 `borrow()`：发声 ≤ 69 用 **flute**（key −12），
flute 写不下（> 69）才用 **bell**（key −24）。默认的 harp 是直接 bell，
这一条只对 p4/p5 生效，其它块不受影响（`postprocess.block_programs()` 解析标记）。

**Lead吉他特例**：两条 lead 八度齐奏合并时，`bell = 合并音 − 12`、`bit = 合并音`（bell 因此高一个八度）；bit 也写不下时（发声 > 57）再借 xylophone：key = 发声 − 24 = bit key − 24。

### 6b. 块名约定（A 的产出，B 靠它认声部）

**块名一律是「声部前缀 + 序号」，不是原轨名**——B 的 rhythm/lead 规则只认这个前缀，
所以中文/日文/通用轨名（midishow 的 `Track 1`、`主音吉他`、`贝司`）也能走完整条流水线：

```
VOCAL1 [pling] ← 主唱          VOCAL1H / VOCAL1L = 上/下和声（有 .gp 时按 voice 拆）
LEAD1  [guitar] ← 主音吉他     主音/主旋律/melody/solo → LEAD
RHYTHM1 [guitar] ← 节奏吉他    节奏/和弦/伴奏/guitar  → RHYTHM（同一轨多 program 会拆成 RHYTHM1/2）
BASS1  [dbass] ← 贝司          贝斯/贝司/低音/bass    → BASS
OTHER1 [harp] ← Acoustic Piano 认不出的保持 OTHER，**不套用任何风格规则**
```

`←` 后面是原始轨名，只作可读性；`block_names()` 取 `[` 之前的部分做匹配。
轨名认不出时的内容兜底：文件里 **≥3 条有音高的轨**才猜（音区低+单音→BASS、
和弦型→RHYTHM、音区高+单音→LEAD），独奏/钢琴谱保持 OTHER。

## 7. 本仓库的可复用工具

`python-workflow/_agent/` 下是蒸馏产出的工具，可直接拿来跑别的曲子：

| 文件 | 用途 |
|---|---|
| `python-workflow/_agent/nbs.py` | 在 229 个 `.nbs` 文件上校验过的读写库（纯 Python，含 UTF-8 支持）|
| `python-workflow/_agent/smf.py` | 容错 SMF(MIDI) 解析器（能读 mido 读不了的坏文件）|

**`python-workflow/` 是把这些规则固化成流水线的地方**：

| 文件 | 用途 |
|---|---|
| `python-workflow/gp2midi.py` | `.gp` → `.mid`（MuseScore）+ **删 ghost note** |
| `python-workflow/midi2nbs.py` | `.mid` → `A.nbs` 模板直出（不折叠、不选音色）|
| `python-workflow/nbs_postprocess.py` | `A.nbs` → `B.nbs` 风格化（步骤见 §6）|
| `python-workflow/README.md` / `AGENTS.md` | 用法与硬知识（含鼓组/八度表勘误）|

### 直接调用示例

```python
import sys; sys.path.insert(0, r"python-workflow\_agent")
import nbs

song = nbs.load(r"曲子_B.nbs")
print(song.tps, len(song.layers), sum(len(L.notes) for L in song.layers))
```
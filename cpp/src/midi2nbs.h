#pragma once
// MIDI → NBS 转换核心 —— midi2nbs.py 的 C++ 移植。
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nbs.h"
#include "smf.h"

namespace nyaa {

// ---- 常量 ------------------------------------------------------------------
extern const int KEY_LO, KEY_HI;
constexpr int kMidiBase = 21;
constexpr int kKeyMinByte = 0, kKeyMaxByte = 87;

enum Role { ROLE_VOCAL = 0, ROLE_LEAD, ROLE_RHYTHM, ROLE_BASS, ROLE_DRUM, ROLE_OTHER };

struct DrumSlot {
    std::string slot;
    std::string instrument;
    int key = 0;
    int times = 1;
    int step = 0;
};

const std::vector<DrumSlot>& kit();

// GM 乐器表：[program] -> (音符盒乐器名, octaveModifier)。数据来源 u3002 / OpenNBS（MIT）。
struct GmEntry {
    const char* name;
    int oct;
};
extern const GmEntry kInstrumentMap[128];
extern const std::map<int, std::pair<const char*, int>> kPercussionMap;
extern const std::map<int, const char*> kGmToSlot;
extern const std::vector<std::pair<int, const char*>> kGmToSlotOrdered;
extern const std::vector<std::pair<int, const char*>> kGmDrumNames;
extern const char* const kPitchClassName[12];
extern const char* const kPitchClassFlat[12];
const char* gm_to_nbs(const std::string& name);

enum class TempoMode { None = 0, Staircase = 1, Changer = 2 };

struct TempoStepInfo {
    double bpm = 0;
    int tpb = 8;
    double beats = 0;
    int nbs_t0 = 0;
};

struct Options {
    int grid = 8;
    bool octave_modifier = true;
    bool keep_order = false;
    bool drum_fallback = true;   // 表外打击乐默认走 u3002 / OpenNBS 表（--no-drum-fallback 关）
    bool separators = true;
    std::string time_mode = "grid";
    TempoMode tempo_mode = TempoMode::Staircase;   // 变速拟似：阶梯 / TempoChanger（占位）/ 无
    std::string gliss = "diatonic";
    std::string key;  // 空 = 自动
    std::string gp;   // 空 = 无
    bool auto_gp = true;
};

struct PartMeta {
    int program = 0;
    std::string track;
    int octave = 0;
    std::string tag;
    std::string role_src;
    bool desc = false;
    std::string group;
    std::string plain_name;
    std::string slot;
    int key = 0;
    int step = 0;
    int times = 1;
    std::set<int> sources;
};

struct Part {
    std::string title;
    int role = ROLE_OTHER;
    int order0 = 0;
    int order1 = 0;
    std::string instrument;
    std::string kind;  // "pitched" | "drum"
    std::vector<std::pair<int, int>> notes;  // (midi_tick, midi_pitch)
    int stack = 1;
    PartMeta meta;
};

struct KeyInfo {
    int root = 0;
    std::string mode;
    std::string source;
    std::string name;
    std::vector<int> pcs;
};

struct GlissInfo {
    int runs = 0;
    int dropped = 0;
    int min_gap = 0;
    std::string mode;
};

struct VocalInfo {
    int v1 = 0;
    int above = 0;
    int below = 0;
    bool gp = false;
    int blocks = 0;
};

struct Info {
    int fmt = 0, ntrks = 0, ppq = 0;
    double bpm = 0, tps = 0;
    int grid = 8;
    int tempo = 0;
    std::vector<std::pair<int, int>> tempos;
    std::vector<TempoStepInfo> tempo_steps;   // 变速拟似生效时的阶梯
    TempoMode tempo_mode = TempoMode::None;
    double duration_sec = 0;
    std::map<std::string, int> stats;
    std::vector<int> unknown_drums;
    KeyInfo key;
    VocalInfo vocal;
    GlissInfo gliss;
    std::string gp_path;
    std::string midi_name;  // 输入文件名（basename）
    std::string midi_stem;
};

struct Header {
    std::string name, author, orig_author, description, midi_filename;
    int tempo = 1000;
    int time_signature = 4;
    int version = 6;
    int vanilla = 20;
};

struct LayerDef {
    std::string name;
    int lock = 0;
    int volume = 100;
    int panning = 100;
    std::string block;
};

struct NoteOut {
    int tick = 0, layer = 0, instrument = 0, key = 0, velocity = 100, panning = 100;
};

struct BlockReport {
    std::string title, kind, instrument, source;
    int layers = 0, base = 0;
    std::optional<int> key;
    int key_lo = 0, key_hi = 0, notes = 0;
};

struct Report {
    std::vector<BlockReport> blocks;
    std::map<std::string, int> dropped;
    int clamped = 0, outside_window = 0, retrigger = 0, retrigger_roll = 0;
    int notes_in = 0, notes_out = 0, poly_max = 0;
};

void convert(const std::string& midi_path, const Options& opt, Header& header, std::vector<Part>& parts, Info& info);
void layout(const std::vector<Part>& parts, const Options& opt, std::vector<LayerDef>& layers,
            std::vector<NoteOut>& notes, Report& report);
void write_nbs(const std::string& path, const Header& header, const std::vector<LayerDef>& layers,
               const std::vector<NoteOut>& notes);
std::vector<std::string> verify(const std::string& path, int expect_notes, int layer_count);

// 小工具
std::string midi_name(int p);
std::string key_name(int k);  // 作者参照系：key = MIDI − 21
bool in_window(int key);
int clamp_byte(int key);

}  // namespace nyaa

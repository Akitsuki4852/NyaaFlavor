#pragma once
// NBS(OpenNBS) 二进制格式读写 —— nbs.py 的 C++ 移植。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyaa {

extern const char* const NOTE_NAMES[12];
extern const char* const INSTRUMENTS[20];
extern const char* const INSTR_CN[16];

struct Note {
    int tick = 0;
    int layer = 0;
    int instrument = 0;
    int key = 0;
    int velocity = 100;
    int panning = 100;
    int pitch = 0;
};

struct Layer {
    int id = 0;
    std::string name;
    int lock = 0;
    int volume = 100;
    int panning = 100;
    std::vector<Note> notes;
    bool muted() const { return lock == 1; }
};

struct CustomInstrument {
    std::string name;
    std::string file;
    int pitch = 0;
    int press_key = 0;
};

struct Song {
    int version = 5;
    int vanilla_instrument_count = 16;
    int song_length = 0;
    int tempo = 1000;
    std::string name;
    std::string author;
    std::string orig_author;
    std::string description;
    int time_signature = 4;
    std::string midi_filename;
    int loop = 0;
    int loop_start = 0;
    std::vector<CustomInstrument> custom_instruments;
    std::vector<Layer> layers;
    std::string path;
    bool has_magic = false;

    double tps() const { return tempo / 100.0; }
    double tick_to_sec(double tick) const { return tick / tps(); }
    double sec_to_tick(double sec) const { return sec * tps(); }
    int duration_ticks() const;
    std::optional<std::string> instrument_of_layer(int li) const;
};

std::string iname(int i);
int instr_id(const std::string& name);  // INSTR_ID[name]，找不到抛异常
std::string keyname(int key);
std::string user_keyname(int key);
int midi_of_key(int key);
int key_of_midi(int midi);

// 文本编解码（UTF-8 为内部表示，与 Python 的 str 对应）
std::string dec_text(const std::string& bytes);                        // _dec
std::string enc_text(const std::string& utf8, bool cp1252_safe);       // _enc
std::string ascii_safe(const std::string& utf8);                       // ascii_safe

struct HeaderInfo {
    int score = 0;
    int offset = 0;
    int ver = 0;
    int vi = 0;
    int song_length = 0;
    int nlay = 0;
    int tempo = 0;
    std::vector<std::string> names;
};

std::optional<HeaderInfo> probe_header(const std::string& raw, int o);
std::optional<HeaderInfo> detect_header(const std::string& raw);

Song load(const std::string& path);
void save(const Song& s, const std::string& path, int version = -1, bool cp1252_safe = false);
// 供测试用：写进内存
std::string save_bytes(const Song& s, int version = -1, bool cp1252_safe = false);

}  // namespace nyaa

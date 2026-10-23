#pragma once
// 容错 SMF 解析 —— smf.py 的 C++ 移植。
#include <cstdint>
#include <string>
#include <vector>

namespace nyaa {

enum class EvKind : char { On = 'n', Off = 'f', Prog = 'p', Name = 'm', Tempo = 't', Other = 'o' };

struct MidiEvent {
    int tick = 0;
    EvKind kind = EvKind::Other;
    int channel = 0;
    int a = 0;  // 音高 / program
    int b = 0;  // 力度 / tempo
    std::string text;
};

using MidiTrack = std::vector<MidiEvent>;

struct MidiFile {
    int fmt = 0;
    int ntrks = 0;
    int ppq = 0;
    std::vector<MidiTrack> tracks;
};

// read_varlen；越界返回 false
bool read_varlen(const std::string& d, size_t& o, int& out);

MidiFile parse_midi(const std::string& path);
MidiTrack parse_track(const std::string& body);

// MIDI 音名，如 C4
std::string midi_note_name(int p);

// 调号事件（FF 59 02 sf mi）；找不到返回 false
bool read_key_signature(const std::string& path, int& sf, int& mi);

}  // namespace nyaa

#pragma once
// 从 Guitar Pro 8 的 .gpif（zip + XML）里读每个音的声部号 —— midi2nbs.read_gp_voice_map。
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nyaa {

// 返回 {(MIDI tick, MIDI 音高): [voice, ...]}；读不了就返回空表。
std::map<std::pair<int, int>, std::vector<int>> read_gp_voice_map(const std::string& gp_path,
                                                                  const std::string& track_name, int ppq);

}  // namespace nyaa

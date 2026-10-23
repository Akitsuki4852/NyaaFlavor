// MIDI → NBS 转换核心 —— midi2nbs.py 的移植。
//
// 与 Python 版逐字节等价是验收标准（用 python-workflow/samples/ 下的 MIDI 对拍）。
// 注意几处「原样照抄」的行为，它们看起来像 bug 但是被刻意保留的：
//   * 鼓层放置的 used 只检查第 0 层，第 1 层可能重复
//   * read_key_signature 取「众数」时并列取先出现者
#include "midi2nbs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <numeric>

#include "common.h"
#include "gpvoice.h"

namespace nyaa {

namespace {

// ---- Python 语义小工具 -----------------------------------------------------

int bisect_right(const std::vector<int>& a, int x) {
    return static_cast<int>(std::upper_bound(a.begin(), a.end(), x) - a.begin());
}

void inc(std::map<std::string, int>& m, const std::string& k, int n = 1) { m[k] += n; }

std::string fmt_d(const std::string& a, int b) { return a + std::to_string(b); }

}  // namespace

std::string midi_name(int p) { return midi_note_name(p); }
std::string key_name(int k) { return midi_name(k + kMidiBase); }
bool in_window(int key) { return KEY_LO <= key && key <= KEY_HI; }
int clamp_byte(int key) { return std::max(kKeyMinByte, std::min(kKeyMaxByte, key)); }

// ---------------------------------------------------------------------------
// 速度表
// ---------------------------------------------------------------------------
namespace {

std::vector<std::pair<int, int>> build_tempo_map(const std::vector<MidiTrack>& tracks) {
    std::vector<std::pair<int, int>> ev;
    for (const auto& tr : tracks)
        for (const auto& e : tr)
            if (e.kind == EvKind::Tempo) ev.emplace_back(e.tick, e.b);
    std::sort(ev.begin(), ev.end());
    if (ev.empty()) return {{0, 500000}};
    std::vector<std::pair<int, int>> out;
    for (const auto& p : ev) {
        if (!out.empty() && out.back().first == p.first)
            out.back() = p;
        else
            out.push_back(p);
    }
    if (out[0].first > 0) out.insert(out.begin(), {0, out[0].second});
    return out;
}

struct TimeMap {
    std::vector<std::pair<int, int>> tempos;
    int ppq = 0;
    std::vector<int> ticks;
    std::vector<double> sec;

    TimeMap(std::vector<std::pair<int, int>> t, int p) : tempos(std::move(t)), ppq(p) {
        for (const auto& x : tempos) ticks.push_back(x.first);
        sec.push_back(0.0);
        double s = 0.0;
        int last_t = tempos[0].first, last_us = tempos[0].second;
        for (size_t i = 1; i < tempos.size(); ++i) {
            s += (double)(tempos[i].first - last_t) / ppq * (last_us / 1000000.0);
            sec.push_back(s);
            last_t = tempos[i].first;
            last_us = tempos[i].second;
        }
    }

    double seconds(int tick) const {
        int i = bisect_right(ticks, tick) - 1;
        i = std::max(0, std::min(i, (int)tempos.size() - 1));
        const auto& [bt, bu] = tempos[i];
        return sec[i] + (double)(tick - bt) / ppq * (bu / 1000000.0);
    }
};

std::string track_name(const MidiTrack& tr, int idx) {
    for (const auto& e : tr)
        if (e.kind == EvKind::Name && !e.text.empty()) return py_strip(e.text);
    return "Track " + std::to_string(idx + 1);
}

std::map<int, std::vector<std::pair<int, int>>> program_at(const MidiTrack& tr) {
    std::map<int, std::vector<std::pair<int, int>>> out;
    for (const auto& e : tr)
        if (e.kind == EvKind::Prog) out[e.channel].emplace_back(e.tick, e.a);
    for (auto& kv : out) std::sort(kv.second.begin(), kv.second.end());
    return out;
}

int program_for(const std::map<int, std::vector<std::pair<int, int>>>& prog, int ch, int tick) {
    auto it = prog.find(ch);
    if (it == prog.end() || it->second.empty()) return 0;
    std::vector<int> ts;
    for (const auto& p : it->second) ts.push_back(p.first);
    const int i = bisect_right(ts, tick) - 1;
    return i >= 0 ? it->second[i].second : 0;
}

// ---- 调性 ------------------------------------------------------------------
const double kKkMajor[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
const double kKkMinor[12] = {6.33, 2.68, 3.52, 5.38, 2.6, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
const int kMajorSteps[7] = {0, 2, 4, 5, 7, 9, 11};
const int kMinorSteps[7] = {0, 2, 3, 5, 7, 8, 10};
const char* const kPcName[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kPcFlat[12] = {"C", "D\u266d", "D", "E\u266d", "E", "F", "G\u266d", "G", "A\u266d", "A", "B\u266d", "B"};
static_assert(true, "音级名表定义在 tables.cpp（kPitchClassName / kPitchClassFlat）");

int pc_of_letter(char c) {
    switch (c) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default: return -1;
    }
}

std::pair<int, std::string> parse_key_name(const std::string& text) {
    // s = text.strip().replace('♭','b').replace('♯','#')
    std::string s = py_strip(text);
    std::string s2;
    for (auto cp : utf8_decode(s)) {
        if (cp == 0x266D) cp = 'b';
        else if (cp == 0x266F) cp = '#';
        s2 += utf8_encode(cp);
    }
    s = s2;
    // mode: 'm' in lower(s.replace('major',''))
    const std::string low = py_lower(s);
    const std::string low_nomajor = [&] {
        std::string t = low, out;
        size_t p = 0;
        while (p < t.size()) {
            if (t.compare(p, 5, "major") == 0) {
                p += 5;
            } else {
                out.push_back(t[p]);
                ++p;
            }
        }
        return out;
    }();
    const std::string mode = low_nomajor.find('m') != std::string::npos ? "minor" : "major";

    // letters = [ch for ch in s if ch.isalpha() and ch.lower() in 'abcdefg']
    std::vector<uint32_t> cps = utf8_decode(s);
    std::string letters;
    size_t first_idx = std::string::npos;  // 在 cps 里的下标
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = cps[i];
        if (!(cp < 0x80 && std::isalpha(static_cast<unsigned char>(cp)))) continue;
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(cp)));
        if (c < 'a' || c > 'g') continue;
        if (letters.empty()) first_idx = i;
        letters.push_back(c);
    }
    if (letters.empty()) throw std::runtime_error("看不懂的调名：" + text);

    char root = static_cast<char>(std::toupper(static_cast<unsigned char>(letters[0])));
    int pc = pc_of_letter(root);
    // rest = s[s.index(letters[0]) + 1:]
    std::string rest;
    for (size_t i = first_idx + 1; i < cps.size(); ++i) rest += utf8_encode(cps[i]);
    if (!rest.empty() && rest[0] == '#') ++pc;
    else if (!rest.empty() && rest[0] == 'b') --pc;
    return {pymod(pc, 12), mode};
}

std::set<int> scale_pcs(int root, const std::string& mode) {
    std::set<int> out;
    if (mode == "minor")
        for (int s : kMinorSteps) out.insert(pymod(root + s, 12));
    else
        for (int s : kMajorSteps) out.insert(pymod(root + s, 12));
    return out;
}

std::pair<int, std::string> key_from_signature(int sf, int mi) {
    const int major_root = pymod(7 * sf, 12);
    if (mi) return {pymod(major_root + 9, 12), "minor"};
    return {major_root, "major"};
}

std::pair<int, std::string> estimate_key(const std::vector<int>& pitches) {
    double hist[12] = {0};
    for (int p : pitches) hist[pymod(p, 12)] += 1.0;
    double total = 0;
    for (double h : hist) total += h;
    if (total == 0) total = 1.0;
    for (double& h : hist) h /= total;
    double best_score = 0.0;
    int best_root = 0;
    std::string best_mode = "major";
    for (int root = 0; root < 12; ++root) {
        for (int which = 0; which < 2; ++which) {
            const std::string mode = which == 0 ? "major" : "minor";
            const double* prof = which == 0 ? kKkMajor : kKkMinor;
            double rot[12];
            for (int i = 0; i < 12; ++i) rot[i] = prof[pymod(i - root, 12)];
            double sum_rot = 0;
            for (double r : rot) sum_rot += r;
            const double m = sum_rot / 12;
            double num = 0, s1 = 0, s2 = 0;
            for (int i = 0; i < 12; ++i) {
                const double a = hist[i] - 0.08333333333333333;
                const double b = rot[i] - m;
                num += a * b;
                s1 += a * a;
                s2 += b * b;
            }
            double den = std::sqrt(s1 * s2);
            if (den == 0.0) den = 1.0;
            const double score = num / den;
            if (score > best_score) {
                best_score = score;
                best_root = root;
                best_mode = mode;
            }
        }
    }
    return {best_root, best_mode};
}

void resolve_key(const std::string& path, const std::vector<int>& pitches, const std::string& override,
                 int& root, std::string& mode, std::string& src, std::string& display) {
    if (!override.empty()) {
        auto pr = parse_key_name(override);
        root = pr.first;
        mode = pr.second;
        src = "手写 --key";
        display = override;
        return;
    }
    int sf = 0, mi = 0;
    if (read_key_signature(path, sf, mi)) {
        auto pr = key_from_signature(sf, mi);
        root = pr.first;
        mode = pr.second;
        src = "MIDI 调号 (sf=" + std::to_string(sf) + ", " + (mi ? "minor" : "major") + ")";
        display = std::string(sf < 0 ? kPcFlat[root] : kPcName[root]) + " " + mode;
        return;
    }
    auto pr = estimate_key(pitches);
    root = pr.first;
    mode = pr.second;
    src = "按音级直方图估的";
    display = std::string(kPcName[root]) + " " + mode;
}

// ---- glissando -------------------------------------------------------------
int diatonic_neighbor(int pitch, int direction, const std::set<int>& pcs) {
    const int d = direction >= 0 ? 1 : -1;
    int p = pitch - d;
    for (int i = 0; i < 12; ++i) {
        if (pcs.count(pymod(p, 12))) return p;
        p -= d;
    }
    return pitch - d;
}

std::vector<std::vector<std::pair<int, int>>> find_chromatic_runs(std::vector<std::pair<int, int>> notes, int max_gap,
                                                                 int max_step, int min_len) {
    std::sort(notes.begin(), notes.end());
    std::vector<std::vector<std::pair<int, int>>> chains;
    std::vector<size_t> active;
    for (const auto& np : notes) {
        const int t = np.first, p = np.second;
        {
            std::vector<size_t> keep;
            for (size_t ci : active)
                if (t - chains[ci].back().first <= max_gap) keep.push_back(ci);
            active.swap(keep);
        }
        size_t best = SIZE_MAX;
        bool have_best = false;
        int best_gap = 0;
        for (size_t ci : active) {
            const auto& ch = chains[ci];
            const int lt = ch.back().first, lp = ch.back().second;
            const int g = t - lt;
            if (g <= 0 || g > max_gap) continue;
            const int d = p - lp;
            if (std::abs(d) != max_step) continue;
            if (ch.size() >= 2 && (ch.back().second - ch[ch.size() - 2].second) * d <= 0) continue;
            if (!have_best || g < best_gap) {
                best = ci;
                best_gap = g;
                have_best = true;
            }
        }
        if (have_best) {
            chains[best].emplace_back(t, p);
            continue;
        }
        chains.push_back({{t, p}});
        active.push_back(chains.size() - 1);
    }
    std::vector<std::vector<std::pair<int, int>>> out;
    for (auto& c : chains)
        if ((int)c.size() >= min_len) out.push_back(c);
    return out;
}

std::pair<std::vector<std::pair<int, int>>, int> apply_glissando(std::vector<std::pair<int, int>> notes,
                                                                const std::set<int>& pcs, int max_gap,
                                                                int tick_step, int min_len) {
    if ((int)notes.size() < min_len) return {notes, 0};
    auto runs = find_chromatic_runs(notes, max_gap, 1, min_len);
    if (runs.empty()) return {notes, 0};

    std::vector<std::pair<int, int>> ns = notes;
    std::sort(ns.begin(), ns.end());
    std::set<std::pair<int, int>> drop;
    std::vector<std::pair<int, int>> add;
    for (const auto& run : runs) {
        const int t_end = run.back().first, end_p = run.back().second;
        const bool up = end_p > run[0].second;
        std::vector<std::pair<int, int>> cands;
        for (const auto& x : ns)
            if (0 < x.first - t_end && x.first - t_end <= max_gap) cands.push_back(x);
        int dest_t = 0, dest_p = 0;
        bool join = false;
        int direction = 0;
        if (!cands.empty()) {
            int t_min = cands[0].first;
            for (const auto& x : cands) t_min = std::min(t_min, x.first);
            std::vector<std::pair<int, int>> tie;
            for (const auto& x : cands)
                if (x.first == t_min) tie.push_back(x);
            size_t bi = 0;
            for (size_t i = 1; i < tie.size(); ++i)
                if (std::abs(tie[i].second - end_p) < std::abs(tie[bi].second - end_p)) bi = i;
            dest_t = tie[bi].first;
            dest_p = tie[bi].second;
            join = true;
            direction = dest_p > end_p ? 1 : dest_p < end_p ? -1 : (up ? 1 : -1);
        } else {
            dest_t = t_end;
            dest_p = end_p;
            join = false;
            direction = up ? 1 : -1;
        }
        const int grace = diatonic_neighbor(dest_p, direction, pcs);
        for (const auto& x : run)
            if (join || x != run.back()) drop.insert(x);
        add.emplace_back(std::max(0, dest_t - tick_step), grace);
    }
    std::vector<std::pair<int, int>> out;
    for (const auto& x : ns)
        if (!drop.count(x)) out.push_back(x);
    for (const auto& x : add) out.push_back(x);
    std::sort(out.begin(), out.end());
    return {out, (int)runs.size()};
}

// ---- 声部拆分 --------------------------------------------------------------
std::pair<int, int> split_vocal_instruments(const std::vector<std::pair<int, int>>& notes,
                                            const std::map<std::pair<int, int>, std::vector<int>>& voice_map,
                                            std::vector<std::pair<int, int>>& v1,
                                            std::vector<std::pair<int, int>>& v2) {
    v1.clear();
    v2.clear();
    if (!voice_map.empty()) {
        std::map<std::pair<int, int>, std::vector<int>> pool = voice_map;
        std::vector<std::pair<int, int>> sorted = notes;
        std::sort(sorted.begin(), sorted.end());
        for (const auto& tp : sorted) {
            int v = 1;
            auto it = pool.find(tp);
            if (it != pool.end() && !it->second.empty()) {
                v = it->second.front();
                it->second.erase(it->second.begin());
            }
            (v >= 2 ? v2 : v1).push_back(tp);
        }
        return {0, 0};
    }
    std::map<int, std::vector<int>> by_tick;  // 有序 → 相当于 sorted(by_tick)
    for (const auto& tp : notes) by_tick[tp.first].push_back(tp.second);
    std::vector<int> prev;
    for (auto& kv : by_tick) {
        const int tick = kv.first;
        std::vector<int> ps = kv.second;
        std::sort(ps.begin(), ps.end());
        ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
        if (ps.size() <= 2) {
            for (int q : ps) v1.emplace_back(tick, q);
            prev = ps;
        } else {
            std::vector<int> order = ps;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                auto key = [&](int q) {
                    int best = 0;
                    bool have = false;
                    for (int r : prev) {
                        const int d = std::abs(q - r);
                        if (!have || d < best) {
                            best = d;
                            have = true;
                        }
                    }
                    return std::make_pair(best, q);
                };
                return key(a) < key(b);
            });
            std::vector<int> keep;
            for (int q : order)
                if (keep.size() < 2) keep.push_back(q);
            std::sort(keep.begin(), keep.end());
            for (int q : ps) {
                const bool in_keep = std::find(keep.begin(), keep.end(), q) != keep.end();
                (in_keep ? v1 : v2).emplace_back(tick, q);
            }
            prev = keep;
        }
    }
    return {0, 0};
}

void split_v2_phrases(std::vector<std::pair<int, int>> v2, const std::set<int>& v1_ticks, int gap,
                      std::vector<std::pair<int, int>>& above, std::vector<std::pair<int, int>>& below) {
    above.clear();
    below.clear();
    if (v2.empty()) return;
    std::sort(v2.begin(), v2.end());
    std::vector<std::vector<std::pair<int, int>>> phrases;
    phrases.push_back({v2[0]});
    for (size_t i = 1; i < v2.size(); ++i) {
        if (v2[i].first - phrases.back().back().first <= gap)
            phrases.back().push_back(v2[i]);
        else
            phrases.push_back({v2[i]});
    }
    for (const auto& ph : phrases) {
        bool in_rest = false;
        for (const auto& x : ph)
            if (!v1_ticks.count(x.first)) {
                in_rest = true;
                break;
            }
        auto& dst = in_rest ? below : above;
        dst.insert(dst.end(), ph.begin(), ph.end());
    }
}

// ---- 轨名 → 声部 -----------------------------------------------------------
struct RoleKeywords {
    int role;
    const char* words[12];  // nullptr 结尾
};

const RoleKeywords kRoleKeywords[] = {
    {ROLE_VOCAL, {"vocal", "vox", "voice", "choir", "ボーカル", "人声", "主唱", "唱", "歌", nullptr}},
    {ROLE_LEAD, {"lead", "solo", "melody", "リード", "メロディ", "主音", "主旋律", "旋律", "独奏", nullptr}},
    {ROLE_RHYTHM, {"rhythm", "chord", "riff", "gtr", "guitar", "コード", "吉他", "和弦", "伴奏", nullptr}},
    {ROLE_BASS, {"bass", "ベース", "ベース", "贝斯", "贝司", "低音", nullptr}},
    {ROLE_DRUM, {"drum", "perc", "ドラム", "鼓", "打击", nullptr}},
};

int role_of(const std::string& name) {
    const std::string n = py_lower(name);
    for (const auto& rk : kRoleKeywords) {
        for (const char* const* w = rk.words; *w; ++w)
            if (n.find(*w) != std::string::npos) return rk.role;
    }
    return ROLE_OTHER;
}

int infer_role_by_content(double median_pitch, double mean_poly, int n_pitched_tracks) {
    if (n_pitched_tracks < 3) return ROLE_OTHER;
    if (median_pitch <= 47 && mean_poly <= 1.6) return ROLE_BASS;
    if (mean_poly >= 1.8) return ROLE_RHYTHM;
    if (median_pitch >= 56) return ROLE_LEAD;
    return ROLE_OTHER;
}

const char* role_tag(int role) {
    switch (role) {
        case ROLE_VOCAL: return "VOCAL";
        case ROLE_LEAD: return "LEAD";
        case ROLE_RHYTHM: return "RHYTHM";
        case ROLE_BASS: return "BASS";
        case ROLE_DRUM: return "DRUMS";
        default: return "OTHER";
    }
}

// ---- 鼓 --------------------------------------------------------------------
std::optional<DrumSlot> drum_slot_for(int gm, bool drum_fallback) {
    auto it = kGmToSlot.find(gm);
    if (it != kGmToSlot.end()) {
        for (const auto& s : kit())
            if (s.slot == it->second) return s;
        return std::nullopt;
    }
    if (drum_fallback) {
        auto p = kPercussionMap.find(gm);
        if (p != kPercussionMap.end()) {
            DrumSlot d;
            d.slot = "GM" + std::to_string(gm);
            d.instrument = gm_to_nbs(p->second.first);
            d.key = p->second.second;
            return d;
        }
    }
    return std::nullopt;
}

std::pair<int, int> kit_sort_index(const std::string& slot_id) {
    const auto& k = kit();
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i].slot == slot_id) return {0, static_cast<int>(i)};
    std::string digits;
    for (auto cp : utf8_decode(slot_id))
        if (py_isdigit(cp)) digits += utf8_encode(cp);
    return {1, digits.empty() ? 999 : std::atoi(digits.c_str())};
}

// ---- pitched 部件的有序容器（对应 Python 的 dict 插入序） -------------------
struct PitchedStore {
    std::vector<Part> slots;
    std::vector<std::pair<int, int>> keys;  // 插入序
    std::vector<char> alive;
    std::map<std::pair<int, int>, size_t> index;

    void put(const std::pair<int, int>& k, Part p) {
        slots.push_back(std::move(p));
        keys.push_back(k);
        alive.push_back(1);
        index[k] = slots.size() - 1;
    }
    void erase(const std::pair<int, int>& k) {
        auto it = index.find(k);
        if (it == index.end()) return;
        alive[it->second] = 0;
        index.erase(it);
    }
    Part* get(const std::pair<int, int>& k) {
        auto it = index.find(k);
        return it == index.end() ? nullptr : &slots[it->second];
    }
    std::vector<Part*> values() {
        std::vector<Part*> out;
        for (size_t i = 0; i < slots.size(); ++i)
            if (alive[i]) out.push_back(&slots[i]);
        return out;
    }
    std::vector<std::pair<int, int>> keys_in_order() const {
        std::vector<std::pair<int, int>> out;
        for (size_t i = 0; i < keys.size(); ++i)
            if (alive[i]) out.push_back(keys[i]);
        return out;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// convert
// ---------------------------------------------------------------------------
void convert(const std::string& midi_path, const Options& opt, Header& header, std::vector<Part>& parts, Info& info) {
    if (opt.grid < 1 || opt.grid > 128) throw std::runtime_error("每拍 tick 必须在 1..128 之间");
    namespace fs = std::filesystem;
    MidiFile mf = parse_midi(midi_path);
    const std::vector<MidiTrack>& tracks = mf.tracks;
    auto tempos = build_tempo_map(tracks);
    TimeMap tm(tempos, mf.ppq);

    std::string gp_path;
    if (!opt.gp.empty()) {
        if (fs::exists(fs::u8path(opt.gp))) gp_path = opt.gp;
    } else if (opt.auto_gp) {
        for (const char* ext : {".gp", ".gpx"}) {
            fs::path cand = fs::u8path(midi_path);
            cand.replace_extension(ext);
            if (fs::exists(cand)) {
                gp_path = path_to_u8(cand);
                break;
            }
        }
    }

    const int first_us = tempos[0].second;
    const double bpm = 60000000.0 / first_us;
    double tps = bpm * opt.grid / 60.0;
    int nbs_tempo = std::max(1, std::min(65535, py_round_int(tps * 100)));
    tps = nbs_tempo / 100.0;

    auto tick_of = [&](int midi_tick) -> int {
        if (opt.time_mode == "real") return py_round_int(tm.seconds(midi_tick) * tps);
        return py_round_int((double)midi_tick * opt.grid / mf.ppq);
    };

    // 每轨统计
    struct TrackStat {
        double median = 0, mean_poly = 0;
    };
    std::map<int, TrackStat> track_stat;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const MidiTrack& tr = tracks[ti];
        std::vector<int> pitches;
        std::map<int, int> per_tick;
        for (const auto& e : tr) {
            if (e.kind != EvKind::On || e.channel == 9) continue;
            pitches.push_back(e.a);
            per_tick[e.tick] += 1;
        }
        if (pitches.empty()) continue;
        std::sort(pitches.begin(), pitches.end());
        const size_t mid = pitches.size() / 2;
        const double median = (pitches.size() % 2) ? (double)pitches[mid]
                                                  : (pitches[mid - 1] + pitches[mid]) / 2.0;
        track_stat[(int)ti] = {median, (double)pitches.size() / std::max(1, (int)per_tick.size())};
    }
    const int n_pitched_tracks = (int)track_stat.size();

    PitchedStore pitched;
    std::map<std::string, Part> drums;  // 有序字典（slot 名）
    std::vector<std::string> drum_order;
    std::map<std::string, int> stats;
    std::set<int> unknown_drums;
    std::map<int, int> role_counter;
    double last_sec = 0.0;

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const MidiTrack& tr = tracks[ti];
        const std::string name = track_name(tr, (int)ti);
        const auto prog = program_at(tr);
        int role = role_of(name);
        std::string role_src = "name";
        auto si = track_stat.find((int)ti);
        if (role == ROLE_OTHER && si != track_stat.end()) {
            role = infer_role_by_content(si->second.median, si->second.mean_poly, n_pitched_tracks);
            if (role != ROLE_OTHER) role_src = "content";
        }
        for (const auto& e : tr) {
            if (e.kind != EvKind::On) continue;
            const int ch = e.channel, pitch = e.a;
            const double sec = tm.seconds(e.tick);
            last_sec = std::max(last_sec, sec);
            if (ch == 9) {
                inc(stats, "drum_notes");
                auto slot = drum_slot_for(pitch, opt.drum_fallback);
                if (!slot) {
                    unknown_drums.insert(pitch);
                    for (const auto& s : kit())
                        if (s.slot == "SNARE") slot = s;
                }
                Part* p = nullptr;
                auto it = drums.find(slot->slot);
                if (it == drums.end()) {
                    Part np;
                    np.title = slot->slot;
                    np.role = ROLE_DRUM;
                    auto ki = kit_sort_index(slot->slot);
                    np.order0 = ki.first;
                    np.order1 = ki.second;
                    np.instrument = slot->instrument;
                    np.kind = "drum";
                    np.stack = slot->times;
                    np.meta.slot = slot->slot;
                    np.meta.key = slot->key;
                    np.meta.step = slot->step;
                    np.meta.times = slot->times;
                    np.meta.track = name;
                    np.meta.group = "drums";
                    drums[slot->slot] = np;
                    drum_order.push_back(slot->slot);
                    it = drums.find(slot->slot);
                }
                p = &it->second;
                p->notes.emplace_back(e.tick, pitch);
                p->meta.sources.insert(pitch);
                continue;
            }
            const int pnum = program_for(prog, ch, e.tick);
            const std::string instrument = gm_to_nbs(kInstrumentMap[pnum].name);
            const int mod = opt.octave_modifier ? kInstrumentMap[pnum].oct : 0;
            const std::pair<int, int> key{(int)ti, pnum};
            Part* p = pitched.get(key);
            if (!p) {
                role_counter[role] += 1;
                const std::string tag = std::string(role_tag(role)) + std::to_string(role_counter[role]);
                Part np;
                np.title = name;
                np.role = role;
                np.order0 = (int)ti;
                np.order1 = pnum;
                np.instrument = instrument;
                np.kind = "pitched";
                np.meta.program = pnum;
                np.meta.track = name;
                np.meta.octave = mod;
                np.meta.tag = tag;
                np.meta.role_src = role_src;
                pitched.put(key, std::move(np));
                p = pitched.get(key);
            }
            p->notes.emplace_back(e.tick, pitch);
            inc(stats, "pitched_notes");
        }
    }

    // 人声拆分
    std::map<std::string, int> vocal_splits;
    bool gp_used = false;
    int vocal_track_no = 0;
    {
        const std::vector<std::pair<int, int>> keys = pitched.keys_in_order();
        for (const auto& key : keys) {
            Part* part = pitched.get(key);
            if (!part || part->role != ROLE_VOCAL) continue;
            std::map<std::pair<int, int>, std::vector<int>> voice_map;
            if (!gp_path.empty()) voice_map = read_gp_voice_map(gp_path, part->title, mf.ppq);
            if (!voice_map.empty()) gp_used = true;
            std::vector<std::pair<int, int>> v1, v2;
            split_vocal_instruments(part->notes, voice_map, v1, v2);
            std::set<int> v1_ticks;
            for (const auto& x : v1) v1_ticks.insert(x.first);
            std::vector<std::pair<int, int>> above, below;
            split_v2_phrases(v2, v1_ticks, 2 * mf.ppq, above, below);
            const std::string base_name = part->title;
            const std::string part_role_src = part->meta.role_src;
            const int pnum = part->meta.program;
            pitched.erase(key);
            vocal_track_no += 1;
            const std::string vtag = std::string(role_tag(ROLE_VOCAL)) + std::to_string(vocal_track_no);
            struct Spec {
                const char* suffix;
                const char* inst;
                const std::vector<std::pair<int, int>>* ns;
                const char* tsuffix;
            };
            const Spec specs[3] = {
                {"上和声", "harp", &above, "H"},
                {"第一声部", "pling", &v1, ""},
                {"下和声", "harp", &below, "L"},
            };
            for (int k = 0; k < 3; ++k) {
                const auto& sp = specs[k];
                if (sp.ns->empty()) continue;
                Part np;
                np.title = base_name + " " + sp.suffix;
                np.role = ROLE_VOCAL;
                np.order0 = -1;
                np.order1 = key.first * 10 + k;
                np.instrument = sp.inst;
                np.kind = "pitched";
                std::vector<std::pair<int, int>> ns2 = *sp.ns;
                std::sort(ns2.begin(), ns2.end());
                np.notes = std::move(ns2);
                np.meta.program = pnum;
                np.meta.track = base_name;
                np.meta.octave = 0;
                np.meta.desc = true;
                np.meta.tag = vtag + sp.tsuffix;
                np.meta.group = "vocal";
                np.meta.plain_name = base_name;
                np.meta.role_src = part_role_src;
                pitched.put({key.first, 1000 + k}, std::move(np));
            }
            vocal_splits["v1"] += (int)v1.size();
            vocal_splits["above"] += (int)above.size();
            vocal_splits["below"] += (int)below.size();
        }
    }
    stats["gp"] = gp_used ? 1 : 0;

    std::vector<int> all_pitches;
    for (auto* p : pitched.values())
        for (const auto& x : p->notes) all_pitches.push_back(x.second);

    int root = 0;
    std::string mode, key_src, key_display;
    resolve_key(midi_path, all_pitches, opt.key, root, mode, key_src, key_display);
    const std::set<int> pcs = scale_pcs(root, mode);

    const int gliss_gap = std::max(2, mf.ppq / 12);
    const int tick_step = std::max(1, mf.ppq / opt.grid);
    int gliss_runs = 0, gliss_dropped = 0;
    for (auto* p : pitched.values()) {
        const size_t before = p->notes.size();
        if (opt.gliss == "diatonic") {
            auto pr = apply_glissando(p->notes, pcs, gliss_gap, tick_step, 2);
            p->notes = std::move(pr.first);
            gliss_runs += pr.second;
            gliss_dropped += (int)(before - p->notes.size());
        }
        std::sort(p->notes.begin(), p->notes.end());
    }
    for (auto* p : pitched.values())
        for (auto& x : p->notes) x.first = std::max(0, tick_of(x.first));
    for (const auto& sid : drum_order)
        for (auto& x : drums[sid].notes) x.first = std::max(0, tick_of(x.first));

    parts.clear();
    for (auto* p : pitched.values()) parts.push_back(*p);
    if (opt.keep_order)
        std::stable_sort(parts.begin(), parts.end(),
                         [](const Part& a, const Part& b) {
                             return std::make_pair(a.order0, a.order1) < std::make_pair(b.order0, b.order1);
                         });
    else
        std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
            if (a.role != b.role) return a.role < b.role;
            if (a.order0 != b.order0) return a.order0 < b.order0;
            return a.order1 < b.order1;
        });
    std::vector<Part> dp;
    for (const auto& sid : drum_order) dp.push_back(drums[sid]);
    std::stable_sort(dp.begin(), dp.end(), [](const Part& a, const Part& b) {
        return kit_sort_index(a.title) < kit_sort_index(b.title);
    });
    parts.insert(parts.end(), dp.begin(), dp.end());

    // 头 & 信息
    fs::path mp = fs::u8path(midi_path);
    header = Header{};
    header.name = path_to_u8(mp.stem());
    header.author = "";
    header.orig_author = "";
    header.description = "Generated by NyaaFlavor (MIDI -> Note Block Studio)";
    header.tempo = nbs_tempo;
    header.time_signature = 4;
    header.midi_filename = path_to_u8(mp.filename());
    header.version = 6;
    header.vanilla = 20;

    info = Info{};
    info.fmt = mf.fmt;
    info.ntrks = (int)tracks.size();
    info.ppq = mf.ppq;
    info.bpm = bpm;
    info.tps = tps;
    info.grid = opt.grid;
    info.tempo = nbs_tempo;
    info.tempos = tempos;
    info.duration_sec = last_sec;
    info.stats = stats;
    info.unknown_drums.assign(unknown_drums.begin(), unknown_drums.end());
    info.key.root = root;
    info.key.mode = mode;
    info.key.source = key_src;
    info.key.name = key_display;
    for (int pc : pcs) info.key.pcs.push_back(pc);
    info.vocal.v1 = vocal_splits["v1"];
    info.vocal.above = vocal_splits["above"];
    info.vocal.below = vocal_splits["below"];
    info.vocal.gp = stats["gp"] != 0;
    {
        int n = 0;
        for (auto* p : pitched.values())
            if (p->meta.group == "vocal") ++n;
        info.vocal.blocks = n;
    }
    info.gp_path = gp_path;
    info.gliss.runs = gliss_runs;
    info.gliss.dropped = gliss_dropped;
    info.gliss.min_gap = gliss_gap;
    info.gliss.mode = opt.gliss;
    info.midi_name = path_to_u8(mp.filename());
    info.midi_stem = path_to_u8(mp.stem());
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
void layout(const std::vector<Part>& parts, const Options& opt, std::vector<LayerDef>& layers,
            std::vector<NoteOut>& notes_out, Report& report) {
    layers.clear();
    notes_out.clear();
    report = Report{};
    std::set<int> roll_layers;

    auto add_separator = [&](const std::string& title) {
        LayerDef d;
        d.name = "-- " + title + " --";
        d.lock = 1;
        layers.push_back(d);
    };

    std::vector<const Part*> pitched_parts, drum_parts, vocal_parts;
    for (const auto& p : parts) (p.kind == "drum" ? drum_parts : pitched_parts).push_back(&p);
    for (const auto* p : pitched_parts)
        if (p->meta.group == "vocal") vocal_parts.push_back(p);

    std::string prev_group = "\x01 sentinel";
    if (opt.separators) add_separator("MIDI2NBS");

    std::vector<const Part*> all = pitched_parts;
    all.insert(all.end(), drum_parts.begin(), drum_parts.end());
    for (const Part* part : all) {
        const bool is_drum = part->kind == "drum";
        int n_layers = 0;
        std::string title;
        std::string tag;
        std::vector<std::array<int, 4>> block_notes;  // tick, li, iid, key
        if (is_drum) {
            const std::string& inst = part->instrument;
            const int dkey = clamp_byte(part->meta.key);
            const int times = std::max(1, part->meta.times);
            const int step = std::max(0, part->meta.step);
            tag = part->meta.slot.empty() ? part->title : part->meta.slot;
            std::vector<int> hits;
            for (const auto& n : part->notes) hits.push_back(n.first);
            std::sort(hits.begin(), hits.end());
            hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
            std::vector<std::pair<int, int>> events;
            for (int tick : hits)
                for (int j = 0; j < times; ++j) events.emplace_back(tick + j * step, j);
            std::sort(events.begin(), events.end());
            std::map<int, std::set<int>> used;
            std::vector<std::pair<int, int>> placed;
            for (const auto& ev : events) {
                const int tick = ev.first;
                int li = 0;
                if (used[li].count(tick)) {
                    li += 1;
                } else {
                    used[li].insert(tick);
                    placed.emplace_back(tick, li);
                }
            }
            // n_layers = max(used) + 1 if used else 0
            n_layers = 0;
            for (const auto& kv : used) n_layers = std::max(n_layers, kv.first + 1);
            const int iid = instr_id(inst);
            for (const auto& pl : placed) block_notes.push_back({pl.first, pl.second, iid, dkey});
            title = tag + " " + key_name(dkey);
            if (times > 1) {
                title += " \u00d7" + std::to_string(times);
                if (step) title += "(\u9694" + std::to_string(step) + "tick)";
            }
        } else {
            tag = part->meta.tag.empty() ? part->title : part->meta.tag;
            const bool desc = part->meta.desc;
            std::map<int, std::vector<int>> by_tick;  // 有序 → 相当于 sorted(by_tick)
            for (const auto& np : part->notes) {
                if (np.first < 0) continue;
                int k = np.second - kMidiBase + 12 * part->meta.octave;
                if (k < kKeyMinByte || k > kKeyMaxByte) {
                    report.clamped += 1;
                    k = clamp_byte(k);
                }
                if (!in_window(k)) report.outside_window += 1;
                by_tick[np.first].push_back(k);
            }
            for (const auto& kv : by_tick) {
                std::vector<int> u = kv.second;
                std::sort(u.begin(), u.end());
                u.erase(std::unique(u.begin(), u.end()), u.end());
                n_layers = std::max(n_layers, (int)u.size());
            }
            title = tag + " [" + part->instrument + "]";
            // 电钢琴（GM 4/5）越界时 B 优先借 flute：用 program 号做机器可读标记
            if (part->meta.program == 4 || part->meta.program == 5)
                title += " p" + std::to_string(part->meta.program);
            const std::string& src = part->meta.track;
            if (!src.empty() && src != tag && is_ascii(src)) title += " \u2190 " + src;
            const int iid = instr_id(part->instrument);
            for (const auto& kv : by_tick) {
                std::vector<int> u = kv.second;
                std::sort(u.begin(), u.end());
                u.erase(std::unique(u.begin(), u.end()), u.end());
                if (desc) std::sort(u.begin(), u.end(), std::greater<int>());
                for (size_t i = 0; i < u.size(); ++i)
                    block_notes.push_back({kv.first, (int)i, iid, u[i]});
            }
        }
        report.notes_in += (int)part->notes.size();
        if (block_notes.empty()) continue;

        if (opt.separators) {
            const std::string& group = part->meta.group;
            if (!group.empty() && group == prev_group) {
                // 同一组连续，不插分隔层
            } else if (group == "drums") {
                add_separator("DRUMS (" + std::to_string(drum_parts.size()) +
                              " slots: cymbal > snare > tom > kick > stick)");
            } else if (group == "vocal") {
                std::string names;
                for (size_t i = 0; i < vocal_parts.size(); ++i) {
                    if (i) names += " / ";
                    names += vocal_parts[i]->meta.tag;
                }
                add_separator("VOCAL (" + std::to_string(vocal_parts.size()) + " blocks: " + names + ")");
            } else {
                add_separator(title);
            }
            prev_group = group;
        }

        const int base = (int)layers.size();
        for (int i = 0; i < n_layers; ++i) {
            LayerDef d;
            d.name = tag + (n_layers == 1 ? std::string()
                                          : " " + std::to_string(i + 1) + "/" + std::to_string(n_layers));
            layers.push_back(d);
        }
        if (is_drum && part->meta.step > 0)
            for (int i = base; i < base + n_layers; ++i) roll_layers.insert(i);

        std::set<std::pair<int, int>> seen;
        int dropped = 0;
        for (const auto& bn : block_notes) {
            const std::pair<int, int> sig{bn[0], base + bn[1]};
            if (seen.count(sig)) {
                dropped += 1;
            } else {
                seen.insert(sig);
                NoteOut no;
                no.tick = bn[0];
                no.layer = base + bn[1];
                no.instrument = bn[2];
                no.key = bn[3];
                no.velocity = 100;
                no.panning = 100;
                notes_out.push_back(no);
            }
        }
        if (dropped) report.dropped[title] += dropped;

        BlockReport br;
        br.title = title;
        br.kind = part->kind;
        br.layers = n_layers;
        br.base = base;
        br.instrument = part->instrument;
        if (is_drum) br.key = part->meta.key;
        int lo = block_notes[0][3], hi = block_notes[0][3];
        std::set<std::array<int, 4>> uniq;
        for (const auto& bn : block_notes) {
            lo = std::min(lo, bn[3]);
            hi = std::max(hi, bn[3]);
            uniq.insert(bn);
        }
        br.key_lo = lo;
        br.key_hi = hi;
        br.notes = (int)uniq.size();
        br.source = part->meta.track;
        report.blocks.push_back(br);
    }
    if (opt.separators) add_separator("END");
    report.notes_out = (int)notes_out.size();

    {
        std::vector<NoteOut> sorted = notes_out;
        std::sort(sorted.begin(), sorted.end(), [](const NoteOut& a, const NoteOut& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.layer != b.layer) return a.layer < b.layer;
            if (a.instrument != b.instrument) return a.instrument < b.instrument;
            if (a.key != b.key) return a.key < b.key;
            if (a.velocity != b.velocity) return a.velocity < b.velocity;
            return a.panning < b.panning;
        });
        std::map<std::array<int, 3>, int> last;
        for (const auto& n : sorted) {
            const std::array<int, 3> key{n.layer, n.instrument, n.key};
            auto it = last.find(key);
            if (it != last.end() && n.tick - it->second < 2) {
                if (roll_layers.count(n.layer))
                    report.retrigger_roll += 1;
                else
                    report.retrigger += 1;
            }
            last[key] = n.tick;
        }
    }
    {
        std::map<int, int> per_tick;
        for (const auto& n : notes_out) per_tick[n.tick] += 1;
        int mx = 0;
        for (const auto& kv : per_tick) {
            auto it = per_tick.find(kv.first + 1);
            mx = std::max(mx, kv.second + (it == per_tick.end() ? 0 : it->second));
        }
        report.poly_max = mx;
    }
}

// ---------------------------------------------------------------------------
// write_nbs
// ---------------------------------------------------------------------------
void write_nbs(const std::string& path, const Header& header, const std::vector<LayerDef>& layers,
               const std::vector<NoteOut>& notes) {
    if (layers.size() > 65535) throw std::runtime_error("\u5c42\u6570\u8fc7\u591a\uff1a" + std::to_string(layers.size()));
    std::map<int, std::vector<const NoteOut*>> by_tick;
    for (const auto& n : notes) by_tick[n.tick].push_back(&n);
    int max_tick = 0;
    if (!by_tick.empty()) max_tick = by_tick.rbegin()->first;
    const int song_length = std::min(65535, std::max(1, max_tick + 1));

    std::string buf;
    auto S = [&](const std::string& txt) {
        const std::string b = cp1252_encode_str(ascii_safe(txt));
        wr_i32(buf, (int)b.size());
        buf += b;
    };

    wr_u16(buf, 0);
    wr_u8(buf, (uint8_t)header.version);
    wr_u8(buf, (uint8_t)header.vanilla);
    wr_u16(buf, (uint16_t)song_length);
    wr_u16(buf, (uint16_t)layers.size());
    S(header.name);
    S(header.author);
    S(header.orig_author);
    S(header.description);
    wr_u16(buf, (uint16_t)header.tempo);
    wr_u8(buf, 0);
    wr_u8(buf, 10);
    wr_u8(buf, (uint8_t)header.time_signature);
    for (int i = 0; i < 5; ++i) wr_i32(buf, 0);
    S(header.midi_filename);
    wr_u8(buf, 0);
    wr_u8(buf, 0);
    wr_u16(buf, 0);

    int last_tick = -1;
    for (auto& kv : by_tick) {
        const int tick = kv.first;
        const int dt = tick - last_tick;
        if (!(dt > 0 && dt <= 32767))
            throw std::runtime_error("tick \u589e\u91cf\u975e\u6cd5\uff1a" + std::to_string(tick) + " - " +
                                     std::to_string(last_tick));
        wr_i16(buf, (int16_t)dt);
        last_tick = tick;
        std::vector<const NoteOut*> row = kv.second;
        std::stable_sort(row.begin(), row.end(), [](const NoteOut* a, const NoteOut* b) { return a->layer < b->layer; });
        int last_layer = -1;
        for (const NoteOut* n : row) {
            const int dl = n->layer - last_layer;
            if (dl <= 0)
                throw std::runtime_error("layer \u589e\u91cf\u975e\u6cd5\uff1atick " + std::to_string(tick) +
                                         " layer " + std::to_string(n->layer));
            wr_i16(buf, (int16_t)dl);
            wr_u8(buf, (uint8_t)n->instrument);
            wr_u8(buf, (uint8_t)n->key);
            wr_u8(buf, (uint8_t)n->velocity);
            wr_u8(buf, (uint8_t)n->panning);
            wr_i16(buf, 0);
            last_layer = n->layer;
        }
        wr_i16(buf, 0);
    }
    wr_i16(buf, 0);
    for (const auto& L : layers) {
        S(L.name);
        wr_u8(buf, (uint8_t)L.lock);
        wr_u8(buf, (uint8_t)L.volume);
        wr_u8(buf, (uint8_t)L.panning);
    }
    wr_u8(buf, 0);

    namespace fs = std::filesystem;
    fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    write_file(path, buf);
}

std::vector<std::string> verify(const std::string& path, int expect_notes, int layer_count) {
    std::vector<std::string> problems;
    Song s = load(path);
    int got = 0;
    for (const auto& L : s.layers) got += (int)L.notes.size();
    if (got != expect_notes)
        problems.push_back("\u97f3\u7b26\u6570\u4e0d\u7b26\uff1a\u5199 " + std::to_string(expect_notes) +
                           "\uff0c\u8bfb\u56de " + std::to_string(got));
    if ((int)s.layers.size() != layer_count)
        problems.push_back("\u5c42\u6570\u4e0d\u7b26\uff1a\u5199 " + std::to_string(layer_count) +
                           "\uff0c\u8bfb\u56de " + std::to_string(s.layers.size()));
    std::set<std::pair<int, int>> seen;
    for (size_t li = 0; li < s.layers.size(); ++li) {
        for (const auto& n : s.layers[li].notes) {
            if (seen.count({n.tick, (int)li})) {
                problems.push_back("(tick,layer) \u91cd\u590d\uff1a" + std::to_string(n.tick) + "," +
                                   std::to_string(li));
                return problems;
            }
            seen.insert({n.tick, (int)li});
        }
    }
    int lo = 0, hi = 0;
    bool any = false;
    for (const auto& L : s.layers)
        for (const auto& n : L.notes) {
            if (!any) {
                lo = hi = n.key;
                any = true;
            }
            lo = std::min(lo, n.key);
            hi = std::max(hi, n.key);
        }
    if (any && (lo < 0 || hi > 87))
        problems.push_back("key \u8d8a\u754c\uff1a" + std::to_string(lo) + ".." + std::to_string(hi));
    return problems;
}

}  // namespace nyaa

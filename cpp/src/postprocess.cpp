// A.nbs → B.nbs（Akitsuki 风格化）—— python-workflow/nbs_postprocess.py 的移植。
#include "postprocess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "common.h"
#include "nbs.h"

namespace nyaa {
namespace {

// 0..1 的比例 → "91.0"
std::string pct(double ratio) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", 100 * ratio);
    return b;
}

constexpr int LO = 33, HI = 57;
constexpr int kRhythmKeepLowDefault = 3;
constexpr int kBellOffsetDefault = -12;
constexpr const char* kRhythmPrefix = "rhythm";
constexpr const char* kLeadPrefix = "lead";

struct InstInfo {
    int offset;
    std::vector<const char*> up;
    std::vector<const char*> down;
};

const std::map<std::string, InstInfo>& instruments() {
    static const std::map<std::string, InstInfo> k = {
        {"harp", {0, {"bell", "flute"}, {"guitar"}}},
        {"dbass", {-24, {"guitar"}, {}}},
        {"guitar", {-12, {"bit"}, {}}},
        {"flute", {12, {"bell"}, {"harp", "bell"}}},
        {"bell", {24, {"xylophone"}, {"harp", "flute"}}},
        {"icechime", {24, {}, {"harp", "bell"}}},
        {"xylophone", {24, {}, {"harp", "bit"}}},
        {"iron_xyl", {0, {"cow_bell", "xylophone"}, {}}},
        {"cow_bell", {12, {"xylophone"}, {"harp", "iron_xyl"}}},
        {"didgeridoo", {-24, {"guitar"}, {}}},
        {"bit", {0, {"xylophone", "bell"}, {"guitar"}}},
        {"banjo", {0, {"bell"}, {"guitar"}}},
        {"pling", {0, {"flute", "bell"}, {"guitar"}}},
    };
    return k;
}

// 字符串：按给定字符集合剥两端（对应 Python str.strip('─- ')）
std::string strip_chars(const std::string& s, const std::string& chars) {
    const std::vector<uint32_t> cps = utf8_decode(s);
    std::set<uint32_t> cs;
    for (auto cp : utf8_decode(chars)) cs.insert(cp);
    size_t a = 0, b = cps.size();
    while (a < b && cs.count(cps[a])) ++a;
    while (b > a && cs.count(cps[b - 1])) --b;
    return utf8_encode(std::vector<uint32_t>(cps.begin() + a, cps.begin() + b));
}

std::string starts_with_py(const std::string& s, std::initializer_list<const char*> prefixes) {
    for (const char* p : prefixes)
        if (s.rfind(p, 0) == 0) return p;
    return {};
}

// 有序 Counter（保插入序，值可为整数或字符串）
struct Stats {
    std::vector<std::pair<std::string, std::string>> items;
    std::map<std::string, size_t> idx;

    void set(const std::string& k, const std::string& v) {
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = items.size();
            items.emplace_back(k, v);
        } else {
            items[it->second].second = v;
        }
    }
    void inc(const std::string& k, long long n = 1) {
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = items.size();
            items.emplace_back(k, std::to_string(n));
        } else {
            items[it->second].second = std::to_string(std::stoll(items[it->second].second) + n);
        }
    }
    // 一次输出 4 条，避免一长串计数把终端/日志挤成一行。
    std::vector<std::string> lines(size_t per_line = 4) const {
        std::vector<std::string> result;
        std::string line;
        size_t count = 0;
        for (const auto& item : items) {
            if (!line.empty()) line += " · ";
            line += item.first + " " + item.second;
            if (++count % per_line == 0) {
                result.push_back(line);
                line.clear();
            }
        }
        if (!line.empty()) result.push_back(line);
        return result;
    }
};

struct Row {
    int tick = 0;
    int li = 0;
    int inst = 0;
    int key = 0;
    bool dead = false;
    bool has_fold = false;
    bool fold_high = false;
    int fold_orig = 0;
    int fold_orig_inst = 0;
};

// 按插入序的 map：键 → 值
template <typename K, typename V>
struct OrderedMap {
    std::vector<std::pair<K, V>> items;
    std::map<K, size_t> idx;

    V& operator[](const K& k) {
        auto it = idx.find(k);
        if (it != idx.end()) return items[it->second].second;
        idx[k] = items.size();
        items.emplace_back(k, V{});
        return items.back().second;
    }
    typename std::vector<std::pair<K, V>>::iterator find(const K& k) {
        auto it = idx.find(k);
        return it == idx.end() ? items.end() : items.begin() + it->second;
    }
    bool count(const K& k) const { return idx.count(k) != 0; }
};

std::map<int, std::string> block_names(const Song& song) {
    std::map<int, std::string> out;
    std::string cur = "misc";
    for (size_t li = 0; li < song.layers.size(); ++li) {
        const std::string name = py_strip(song.layers[li].name);
        if (!starts_with_py(name, {"\u2500\u2500", "--"}).empty()) {
            std::string t = strip_chars(name, "\u2500- ");
            const size_t br = t.find('[');
            if (br != std::string::npos) t = t.substr(0, br);
            cur = py_strip(t);
        }
        out[(int)li] = cur;
    }
    return out;
}

int fold_key(int key) {
    while (key < LO) key += 12;
    while (key > HI) key -= 12;
    return key;
}

bool is_drum_block(const std::string& name) {
    static const std::regex re(
        R"(^(?:DRUMS\b|GM\d+$|HH-[CO]$|CRASH$|SPLASH$|SNARE$|TOM-[LMH]$|BD$|STICK$))");
    return std::regex_search(py_strip(name), re, std::regex_constants::match_continuous);
}

// layer -> A 阶段写在分隔层里的 GM program（电钢的 p4/p5），没有则 -1
std::map<int, int> block_programs(const Song& song) {
    static const std::regex re(R"(\bp(\d{1,3})\b)");
    std::map<int, int> out;
    int cur = -1;
    for (size_t li = 0; li < song.layers.size(); ++li) {
        const std::string name = py_strip(song.layers[li].name);
        if (!starts_with_py(name, {"\u2500\u2500", "--"}).empty()) {
            std::smatch m;
            cur = std::regex_search(name, m, re) ? std::stoi(m[1].str()) : -1;
        }
        out[(int)li] = cur;
    }
    return out;
}

bool lead_block(const std::map<int, std::string>& blk, int li) {
    auto it = blk.find(li);
    if (it == blk.end()) return false;
    return py_lower(it->second).rfind(kLeadPrefix, 0) == 0;
}

bool rhythm_block(const std::map<int, std::string>& blk, int li) {
    auto it = blk.find(li);
    if (it == blk.end()) return false;
    return py_lower(it->second).rfind(kRhythmPrefix, 0) == 0;
}

std::optional<std::pair<std::string, int>> borrow(const std::string& inst, int sounding,
                                                 const std::vector<std::string>& prefer = {}) {
    const auto& tbl = instruments();
    auto it = tbl.find(inst);
    if (it == tbl.end()) return std::nullopt;
    std::vector<std::string> chain;
    auto push1 = [&](const std::string& t) {
        if (t != inst && tbl.count(t) && std::find(chain.begin(), chain.end(), t) == chain.end())
            chain.push_back(t);
    };
    for (const std::string& t : prefer) push1(t);   // 电钢：flute -> bell
    for (const char* t : it->second.up) push1(t);
    for (const char* t : it->second.down) push1(t);
    const std::vector<std::string> snapshot = chain;
    for (const std::string& t : snapshot) {
        auto ti = tbl.find(t);
        if (ti == tbl.end()) continue;
        for (const char* t2 : ti->second.up) push1(t2);
        for (const char* t2 : ti->second.down) push1(t2);
    }
    for (const std::string& t : chain) {
        const int k = sounding - tbl.at(t).offset;
        if (k >= LO && k <= HI) return std::make_pair(t, k);
    }
    // 窗口里没有音色能精确表达（发声 83 这种）：取「折八度之后误差最小」的一组，
    // 音级不变，最多差 12 个半音；比原地折成 47（差 36）强。
    std::optional<std::tuple<int, std::string, int>> best;
    std::vector<std::string> all{inst};
    all.insert(all.end(), chain.begin(), chain.end());
    for (const std::string& t : all) {
        auto ti = tbl.find(t);
        if (ti == tbl.end()) continue;
        const int k = fold_key(sounding - ti->second.offset);
        const int err = std::abs(k + ti->second.offset - sounding);
        if (!best || err < std::get<0>(*best)) best = std::make_tuple(err, t, k);
    }
    if (!best) return std::nullopt;
    return std::make_pair(std::get<1>(*best), std::get<2>(*best));
}

int best_shift(const std::vector<const Row*>& notes, const std::vector<std::array<int, 3>>& ref_notes,
               int max_shift = 64) {
    std::map<int, std::set<int>> ref;
    for (const auto& r : ref_notes) ref[r[0]].insert(pymod(r[1], 12));
    int best_s = 0, best_hit = -1;
    for (int s = -max_shift; s <= max_shift; ++s) {
        int hit = 0;
        for (const Row* r : notes) {
            auto it = ref.find(r->tick + s);
            if (it != ref.end() && it->second.count(pymod(r->key, 12))) ++hit;
        }
        if (hit > best_hit) {
            best_hit = hit;
            best_s = s;
        }
    }
    return best_s;
}

// 音色用量：guitar 12 · sdrum 10 · …
std::string format_instrument_counts(const std::vector<std::pair<std::string, int>>& v, size_t limit) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < limit; ++i) {
        if (i) s += " · ";
        s += v[i].first + " " + std::to_string(v[i].second);
    }
    if (v.size() > limit) s += " · …";
    return s;
}

// 有序 counter：key → count，保插入序
struct InsertionCounter {
    std::vector<std::pair<std::string, int>> items;
    std::map<std::string, size_t> idx;
    void inc(const std::string& k) {
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = items.size();
            items.emplace_back(k, 1);
        } else {
            items[it->second].second += 1;
        }
    }
    void sort_desc() {
        std::stable_sort(items.begin(), items.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    size_t size() const { return items.size(); }
    int get(const std::string& k) const {
        auto it = idx.find(k);
        return it == idx.end() ? 0 : items[it->second].second;
    }
};

std::string path_name(const std::string& p) {
    return path_to_u8(std::filesystem::u8path(p).filename());
}

}  // namespace

int run_postprocess(const PostOptions& opt, const std::function<void(const std::string&)>& out) {
    Song song = load(opt.src);
    const auto blk = block_names(song);
    Stats stats;
    const int keep_low = opt.keep_low;
    const int bell_offset = opt.bell_offset;

    std::vector<Row> rows;
    for (size_t li = 0; li < song.layers.size(); ++li)
        for (const auto& n : song.layers[li].notes)
            rows.push_back(Row{n.tick, (int)li, n.instrument, n.key});

    // 1. FOLD
    if (opt.fold) {
        for (auto& r : rows) {
            if (r.key >= LO && r.key <= HI) continue;
            auto bit = blk.find(r.li);
            const std::string bname = bit == blk.end() ? std::string() : bit->second;
            if (is_drum_block(bname)) {
                // 鼓的 key 只是"选哪个鼓音"，折八度不改音色，也不进 4b
                r.key = fold_key(r.key);
                stats.inc("鼓块只折八度");
                continue;
            }
            r.has_fold = true;
            r.fold_high = r.key > HI;
            r.fold_orig = r.key;
            r.fold_orig_inst = r.inst;
            r.key = fold_key(r.key);
            stats.inc("八度折叠");
        }
    }

    // 2. RHYTHM
    if (opt.rhythm) {
        OrderedMap<std::pair<std::string, int>, std::vector<size_t>> per_block_tick;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (rhythm_block(blk, r.li) && iname(r.inst) == "guitar") {
                auto it = blk.find(r.li);
                per_block_tick[std::make_pair(it->second, r.tick)].push_back(i);
            }
        }
        for (auto& kv : per_block_tick.items) {
            std::vector<size_t>& idxs = kv.second;
            if ((int)idxs.size() <= keep_low) continue;
            std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) { return rows[a].key < rows[b].key; });
            for (size_t i = (size_t)keep_low; i < idxs.size(); ++i) {
                Row& r = rows[idxs[i]];
                r.inst = instr_id("bit");
                r.key = fold_key(r.key - 12);
                stats.inc("节奏吉他转 bit");
            }
            for (size_t i = 0; i < (size_t)keep_low; ++i) {
                Row& r = rows[idxs[i]];
                if (r.key < LO) {
                    r.key += 12;
                    stats.inc("低音 guitar 升八度");
                }
            }
        }
    }

    // 3. MERGE
    if (opt.merge) {
        std::map<std::tuple<int, std::string, int>, size_t> keep;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (!lead_block(blk, r.li) || iname(r.inst) != "guitar") continue;
            auto k = std::make_tuple(r.tick, blk.at(r.li), pymod(r.key, 12));
            auto it = keep.find(k);
            if (it == keep.end() || r.key < rows[it->second].key) keep[k] = i;
        }
        std::set<size_t> keep_ids;
        for (const auto& kv : keep) keep_ids.insert(kv.second);
        int dup = 0;
        std::vector<char> drop(rows.size(), 0);
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (lead_block(blk, r.li) && iname(r.inst) == "guitar" && !keep_ids.count(i)) {
                drop[i] = 1;
                ++dup;
            }
        }
        stats.set("八度重复", std::to_string(dup));
        std::vector<Row> keep_rows;
        for (size_t i = 0; i < rows.size(); ++i)
            if (!drop[i]) keep_rows.push_back(rows[i]);
        rows = std::move(keep_rows);

        OrderedMap<int, OrderedMap<std::string, std::vector<size_t>>> by_tick;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (lead_block(blk, r.li) && iname(r.inst) == "guitar") by_tick[r.tick][blk.at(r.li)].push_back(i);
        }
        std::vector<Row> bell_rows;
        for (auto& tk : by_tick.items) {
            const int tick = tk.first;
            auto& per_block = tk.second;
            if (per_block.items.size() < 2) continue;
            // sig: 块 → 音级集合
            std::vector<std::pair<std::set<int>, std::string>> sig;
            for (auto& b : per_block.items) {
                std::set<int> s;
                for (size_t i : b.second) s.insert(pymod(rows[i].key, 12));
                sig.emplace_back(s, b.first);
            }
            OrderedMap<std::set<int>, std::vector<std::string>> groups;
            for (auto& s : sig) groups[s.first].push_back(s.second);
            for (auto& g : groups.items) {
                std::vector<std::string>& blocks = g.second;
                if (blocks.size() < 2) continue;
                std::stable_sort(blocks.begin(), blocks.end(), [&](const std::string& a, const std::string& b) {
                    auto min_key = [&](const std::string& bn) {
                        int m = 1 << 30;
                        for (size_t i : per_block.items[per_block.idx[bn]].second) m = std::min(m, rows[i].key);
                        return m;
                    };
                    return min_key(a) < min_key(b);
                });
                const std::string keep_b = blocks[0];
                for (size_t bi = 1; bi < blocks.size(); ++bi)
                    for (size_t i : per_block.items[per_block.idx[blocks[bi]]].second) rows[i].dead = true;
                for (size_t i : per_block.items[per_block.idx[keep_b]].second) {
                    Row& r = rows[i];
                    const int k = r.key;
                    r.inst = instr_id("bit");
                    const int bk = k + bell_offset;
                    if (bk < LO) {
                        bell_rows.push_back(Row{tick, -1, instr_id("harp"), fold_key(bk + 24)});
                        stats.inc("bell 越界改 harp");
                    } else {
                        bell_rows.push_back(Row{tick, -1, instr_id("bell"), fold_key(bk)});
                        stats.inc("重复主音合并");
                    }
                }
            }
        }
        int dead_n = 0;
        for (const auto& r : rows)
            if (r.dead) ++dead_n;
        stats.inc("重复块丢弃", dead_n);
        std::vector<Row> keep_rows2;
        for (const auto& r : rows)
            if (!r.dead) keep_rows2.push_back(r);
        rows = std::move(keep_rows2);
        rows.insert(rows.end(), bell_rows.begin(), bell_rows.end());
    }

    // 4. BORROW（lead 高音）
    if (opt.borrow) {
        for (auto& r : rows) {
            if (!r.has_fold || !r.fold_high || r.li < 0) continue;
            if (!lead_block(blk, r.li)) continue;
            if (iname(r.inst) != "guitar") continue;
            const int sounding = r.fold_orig + instruments().at("guitar").offset;
            auto hit = borrow("guitar", sounding);
            if (hit) {
                r.inst = instr_id(hit->first);
                r.key = hit->second;
                stats.inc("主音高音改 " + hit->first);
            } else {
                stats.inc("主音高音无音色可用");
            }
        }
    }

    // 4b. BORROW TABLE
    if (opt.borrow_table) {
        const std::map<int, int> prog = block_programs(song);
        for (auto& r : rows) {
            if (!r.has_fold || r.li < 0) continue;
            auto bit = blk.find(r.li);
            if (is_drum_block(bit == blk.end() ? std::string() : bit->second))
                continue;   // 鼓保留自己的音色
            if (r.inst != r.fold_orig_inst) continue;
            const std::string src = iname(r.inst);
            auto reg = instruments().find(src);
            if (reg == instruments().end()) continue;
            std::vector<std::string> prefer;
            auto pit = prog.find(r.li);
            const int p = pit == prog.end() ? -1 : pit->second;
            if (src == "harp" && (p == 4 || p == 5)) prefer = {"flute", "bell"};
            auto hit = borrow(src, r.fold_orig + reg->second.offset, prefer);
            if (!hit) continue;
            r.inst = instr_id(hit->first);
            r.key = fold_key(hit->second);
            stats.inc("借音色 " + src + "→" + hit->first);
        }
    }

    // 5. CLASH
    if (opt.clash) {
        std::map<int, std::set<int>> lead_keys;
        for (const auto& r : rows)
            if (r.li >= 0 && lead_block(blk, r.li) && iname(r.inst) == "guitar") lead_keys[r.tick].insert(r.key);
        std::vector<Row> kept;
        for (const auto& r : rows) {
            auto it = lead_keys.find(r.tick);
            if (iname(r.inst) == "bit" && it != lead_keys.end() && it->second.count(r.key)) {
                stats.inc("与主音撞音删除");
            } else {
                kept.push_back(r);
            }
        }
        rows = std::move(kept);
    }

    // 6. DEDUPE
    if (opt.dedupe) {
        std::set<std::tuple<int, int, int>> seen;
        std::vector<Row> kept;
        for (const auto& r : rows) {
            auto sig = std::make_tuple(r.tick, r.key, r.inst);
            if (seen.count(sig)) continue;
            seen.insert(sig);
            kept.push_back(r);
        }
        stats.set("去重删除", std::to_string(rows.size() - kept.size()));
        rows = std::move(kept);
    }

    // 7. ALIGN / SHIFT
    int shift = opt.shift;
    if (!shift && !opt.align.empty()) {
        Song ref = load(opt.align);
        std::vector<std::array<int, 3>> ref_rows;
        for (const auto& L : ref.layers)
            for (const auto& n : L.notes) ref_rows.push_back({n.tick, n.key, instr_id(iname(n.instrument))});
        std::vector<const Row*> ptrs;
        for (const auto& r : rows) ptrs.push_back(&r);
        shift = best_shift(ptrs, ref_rows);
        stats.set("对齐偏移", std::to_string(shift));
    }
    if (shift)
        for (auto& r : rows) r.tick = std::max(0, r.tick + shift);

    // 8. 铺层
    std::map<int, std::vector<Note>> per_layer;
    std::vector<Note> bell_notes;
    for (const auto& r : rows) {
        Note n;
        n.tick = r.tick;
        n.layer = 0;
        n.instrument = r.inst;
        n.key = r.key;
        if (r.li < 0)
            bell_notes.push_back(n);
        else
            per_layer[r.li].push_back(n);
    }
    std::vector<Layer> layers;
    for (size_t li = 0; li < song.layers.size(); ++li) {
        const Layer& src = song.layers[li];
        auto it = per_layer.find((int)li);
        std::vector<Note> got = it == per_layer.end() ? std::vector<Note>{} : it->second;
        if (opt.drop_empty && got.empty()) continue;
        Layer L;
        L.id = (int)layers.size();
        L.name = src.name;
        L.lock = src.lock;
        L.volume = src.volume;
        L.panning = src.panning;
        L.notes = std::move(got);
        layers.push_back(std::move(L));
    }

    if (!bell_notes.empty()) {
        std::map<int, int> per_tick;
        for (const auto& n : bell_notes) per_tick[n.tick] += 1;
        int width = 0;
        for (const auto& kv : per_tick) width = std::max(width, kv.second);
        std::vector<std::vector<Note>> bins((size_t)width);
        std::map<int, int> seen;
        std::vector<Note> sorted = bell_notes;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Note& a, const Note& b) { return a.tick < b.tick; });
        for (const auto& n : sorted) {
            bins[(size_t)(seen[n.tick] % width)].push_back(n);
            seen[n.tick] += 1;
        }
        std::vector<Layer> bell_layers;
        for (size_t i = 0; i < bins.size(); ++i) {
            Layer L;
            L.id = 0;
            L.name = "Bell" + std::to_string(i + 1);
            L.notes = bins[i];
            bell_layers.push_back(std::move(L));
        }
        int insert_at = 0;
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].lock != 1 && py_lower(layers[i].name).find("vocal") != std::string::npos)
                insert_at = (int)i + 1;
        Layer silent;
        silent.id = 0;
        silent.name = "-- Bell --";
        silent.lock = 1;
        std::vector<Layer> merged;
        merged.insert(merged.end(), layers.begin(), layers.begin() + insert_at);
        merged.push_back(silent);
        merged.insert(merged.end(), bell_layers.begin(), bell_layers.end());
        merged.insert(merged.end(), layers.begin() + insert_at, layers.end());
        layers = std::move(merged);
    }

    // 9. TIDY
    if (opt.tidy) {
        auto is_separator = [](const Layer& L) {
            return L.lock != 0 || !starts_with_py(py_strip(L.name), {"\u2500\u2500", "--"}).empty();
        };
        std::vector<Layer> kept_layers;
        const size_t before = layers.size();
        for (const auto& L : layers) {
            const bool empty = L.notes.empty();
            if (empty && !is_separator(L)) {
                stats.inc("删空轨");
            } else if (empty && !kept_layers.empty() && kept_layers.back().notes.empty() &&
                       is_separator(kept_layers.back())) {
                kept_layers.back() = L;
                stats.inc("合并分隔轨");
            } else {
                kept_layers.push_back(L);
            }
        }
        layers = std::move(kept_layers);
        stats.set("层数", std::to_string(before) + "→" + std::to_string(layers.size()));
    }

    for (size_t i = 0; i < layers.size(); ++i) {
        layers[i].id = (int)i;
        for (auto& n : layers[i].notes) n.layer = (int)i;
    }
    song.layers = layers;
    save(song, opt.dst, -1, opt.cp1252_safe);

    // ---- 输出 ----
    InsertionCounter insts;
    std::vector<int> keys;
    for (const auto& L : layers)
        for (const auto& n : L.notes) {
            insts.inc(iname(n.instrument));
            keys.push_back(n.key);
        }
    insts.sort_desc();

    if (opt.quiet) {
        out("整理后：" + std::to_string(keys.size()) + " 个音符 / " + std::to_string(layers.size()) + " 条音轨");
    } else {
        if (stats.items.empty()) {
            out("  改动：无");
        } else {
            out("  改动：");
            for (const auto& line : stats.lines()) out("    " + line);
        }
        if (!keys.empty()) {
            out("  产出：" + std::to_string(keys.size()) + " 个音 / " + std::to_string(layers.size()) + " 层 / key " +
                std::to_string(*std::min_element(keys.begin(), keys.end())) + "–" +
                std::to_string(*std::max_element(keys.begin(), keys.end())));
        } else {
            out("  产出：0 个音 / " + std::to_string(layers.size()) + " 层");
        }
        out("  音色：" + format_instrument_counts(insts.items, 10));
    }

    if (!opt.compare.empty()) {
        Song ref = load(opt.compare);
        std::set<std::tuple<int, int, std::string>> mine, theirs;
        for (const auto& L : layers)
            for (const auto& n : L.notes) mine.insert({n.tick, n.key, iname(n.instrument)});
        for (const auto& L : ref.layers)
            for (const auto& n : L.notes) theirs.insert({n.tick, n.key, iname(n.instrument)});
        auto score = [](const std::set<std::tuple<int, int, std::string>>& a,
                        const std::set<std::tuple<int, int, std::string>>& b) {
            size_t inter = 0;
            for (const auto& x : a)
                if (b.count(x)) ++inter;
            const double rec = b.empty() ? 0.0 : (double)inter / b.size();
            const double prec = a.empty() ? 0.0 : (double)inter / a.size();
            const double f1 = (rec + prec) ? 2 * rec * prec / (rec + prec) : 0.0;
            return std::make_tuple(rec, prec, f1);
        };
        auto [rec, prec, f1] = score(mine, theirs);
        out("  与参考谱 " + path_name(opt.compare) + " 对比（全部音）：命中率 " + pct(rec) + "% / 准确率 " +
            pct(prec) + "% / F1 " + pct(f1) + "%");
        const std::set<std::string> drums = {"bdrum", "sdrum", "clicks"};
        std::set<std::tuple<int, int, std::string>> m2, t2;
        for (const auto& x : mine)
            if (!drums.count(std::get<2>(x))) m2.insert(x);
        for (const auto& x : theirs)
            if (!drums.count(std::get<2>(x))) t2.insert(x);
        auto [r2, p2, f2] = score(m2, t2);
        out("  与参考谱对比（不含鼓）：命中率 " + pct(r2) + "% / 准确率 " + pct(p2) + "% / F1 " +
            pct(f2) + "%");
    }
    return 0;
}

}  // namespace nyaa

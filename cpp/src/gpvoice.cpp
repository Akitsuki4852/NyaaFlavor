// .gp (GP8) 声部表读取：zip 解包 + raw DEFLATE 解压 + 极简 XML 解析。
//
// 只为了 midi2nbs.read_gp_voice_map 服务，所以没引入任何第三方库：
// inflate 与 XML 都是自带的最小实现。
#include "gpvoice.h"
#include "../third_party/tinyxml2/tinyxml2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include "common.h"

namespace nyaa {
namespace {

// ===========================================================================
// 1. raw DEFLATE（puff 风格：不建查找表，逐位解码）
// ===========================================================================
struct BitReader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t pos = 0;
    int bitpos = 0;
    bool ok = true;

    int next_bit() {
        if (pos >= n) {
            ok = false;
            return 0;
        }
        const int b = (p[pos] >> bitpos) & 1;
        if (++bitpos == 8) {
            bitpos = 0;
            ++pos;
        }
        return b;
    }
    uint32_t bits(int need) {
        uint32_t v = 0;
        for (int i = 0; i < need; ++i) v |= (uint32_t)next_bit() << i;
        return v;
    }
};

struct Huffman {
    int count[16] = {0};
    std::vector<int> sym;

    bool build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        if (count[0] == n) return true;  // 全 0：不参与解码
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;  // 码长不合法
        }
        int offs[16] = {0};
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        sym.assign((size_t)n, 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) sym[offs[lengths[i]]++] = i;
        return true;
    }

    // 返回 -1 表示数据损坏
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.next_bit();
            if (!br.ok) return -1;
            const int cnt = count[len];
            if (code - first < cnt) return sym[(size_t)(index + (code - first))];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_raw(const uint8_t* in, size_t n, std::string& out) {
    BitReader br{in, n, 0, 0, true};
    for (;;) {
        const int last = br.next_bit();
        const int type = (int)br.bits(2);
        if (!br.ok) return false;
        if (type == 0) {  // stored
            br.bitpos = 0;
            if (br.pos + 4 > n) return false;
            const uint32_t len = in[br.pos] | ((uint32_t)in[br.pos + 1] << 8);
            br.pos += 4;
            if (br.pos + len > n) return false;
            out.append((const char*)in + br.pos, len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            Huffman lit, dist;
            if (type == 1) {
                uint8_t ll[288];
                for (int i = 0; i < 144; ++i) ll[i] = 8;
                for (int i = 144; i < 256; ++i) ll[i] = 9;
                for (int i = 256; i < 280; ++i) ll[i] = 7;
                for (int i = 280; i < 288; ++i) ll[i] = 8;
                uint8_t dl[30];
                for (int i = 0; i < 30; ++i) dl[i] = 5;
                if (!lit.build(ll, 288) || !dist.build(dl, 30)) return false;
            } else {
                const int hlit = (int)br.bits(5) + 257;
                const int hdist = (int)br.bits(5) + 1;
                const int hclen = (int)br.bits(4) + 4;
                if (!br.ok) return false;
                static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; ++i) cl[order[i]] = (uint8_t)br.bits(3);
                Huffman clh;
                if (!clh.build(cl, 19)) return false;
                std::vector<uint8_t> lens((size_t)(hlit + hdist), 0);
                int i = 0;
                while (i < hlit + hdist) {
                    const int s = clh.decode(br);
                    if (s < 0) return false;
                    if (s < 16) {
                        lens[i++] = (uint8_t)s;
                    } else {
                        int rep = 0;
                        uint8_t val = 0;
                        if (s == 16) {
                            if (i == 0) return false;
                            val = lens[i - 1];
                            rep = 3 + (int)br.bits(2);
                        } else if (s == 17) {
                            rep = 3 + (int)br.bits(3);
                        } else {
                            rep = 11 + (int)br.bits(7);
                        }
                        if (i + rep > hlit + hdist) return false;
                        while (rep--) lens[i++] = val;
                    }
                    if (!br.ok) return false;
                }
                if (!lit.build(lens.data(), hlit)) return false;
                // 距离码允许全 0（没有距离码）
                if (!dist.build(lens.data() + hlit, hdist)) return false;
            }
            for (;;) {
                const int s = lit.decode(br);
                if (s < 0) return false;
                if (s < 256) {
                    out.push_back((char)s);
                } else if (s == 256) {
                    break;
                } else {
                    const int li = s - 257;
                    if (li >= 29) return false;
                    const int len = kLenBase[li] + (int)br.bits(kLenExtra[li]);
                    const int ds = dist.decode(br);
                    if (ds < 0 || ds >= 30) return false;
                    const int d = kDistBase[ds] + (int)br.bits(kDistExtra[ds]);
                    if (!br.ok || (size_t)d > out.size()) return false;
                    const size_t start = out.size() - d;
                    for (int k = 0; k < len; ++k) out.push_back(out[start + k]);
                }
            }
        } else {
            return false;  // 保留类型
        }
        if (last) break;
        if (!br.ok) return false;
    }
    return true;
}

// ===========================================================================
// 2. 最小 zip 读取（只支持 store / deflate）
// ===========================================================================
uint32_t le32(const std::string& b, size_t o) {
    return (uint32_t)(uint8_t)b[o] | ((uint32_t)(uint8_t)b[o + 1] << 8) | ((uint32_t)(uint8_t)b[o + 2] << 16) |
           ((uint32_t)(uint8_t)b[o + 3] << 24);
}
uint16_t le16(const std::string& b, size_t o) {
    return (uint16_t)((uint8_t)b[o] | ((uint8_t)b[o + 1] << 8));
}

std::optional<std::string> zip_read_entry(const std::string& raw, const std::string& want) {
    if (raw.size() < 22) return std::nullopt;
    // 从尾部找 EOCD
    size_t eocd = std::string::npos;
    const size_t scan_from = raw.size() > 65557 ? raw.size() - 65557 : 0;
    for (size_t i = raw.size() - 22 + 1; i-- > scan_from;) {
        if (raw.compare(i, 4, "PK\x05\x06") == 0) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return std::nullopt;
    uint32_t count = le16(raw, eocd + 10);
    uint32_t cd_off = le32(raw, eocd + 16);
    size_t p = cd_off;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > raw.size() || raw.compare(p, 4, "PK\x01\x02") != 0) return std::nullopt;
        const uint16_t method = le16(raw, p + 10);
        const uint32_t csize = le32(raw, p + 20);
        const uint16_t nlen = le16(raw, p + 28);
        const uint16_t elen = le16(raw, p + 30);
        const uint16_t clen = le16(raw, p + 32);
        const uint32_t lho = le32(raw, p + 42);
        const std::string name = raw.substr(p + 46, nlen);
        p += 46u + nlen + elen + clen;
        if (name != want) continue;
        if (lho + 30 > raw.size() || raw.compare(lho, 4, "PK\x03\x04") != 0) return std::nullopt;
        const uint16_t lnlen = le16(raw, lho + 26);
        const uint16_t lelen = le16(raw, lho + 28);
        const size_t data_off = lho + 30u + lnlen + lelen;
        if (data_off + csize > raw.size()) return std::nullopt;
        if (method == 0) return raw.substr(data_off, csize);
        if (method == 8) {
            std::string out;
            if (!inflate_raw((const uint8_t*)raw.data() + data_off, csize, out)) return std::nullopt;
            return out;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// ===========================================================================
// 3. 极简 XML DOM
// ===========================================================================
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> kids;
    std::string text;

    const std::string* attr(const std::string& k) const {
        for (const auto& a : attrs)
            if (a.first == k) return &a.second;
        return nullptr;
    }
    // 直接子元素里第一个同名者
    const XmlNode* child(const std::string& n) const {
        for (const auto& c : kids)
            if (c.name == n) return &c;
        return nullptr;
    }
    // findtext：本元素的直接子元素同名者之文本
    const std::string* findtext(const std::string& n) const {
        const XmlNode* c = child(n);
        if (!c) return nullptr;
        return c->text.empty() ? nullptr : &c->text;
    }
};

// TinyXML2 handles XML declarations, UTF-8 BOMs, entities and CDATA in GPIF files.
struct XmlParser {
    const std::string& source;
    static void copy(const tinyxml2::XMLElement* element, XmlNode& output) {
        output.name = element->Name();
        if (const char* value = element->GetText()) output.text = value;
        for (auto attr = element->FirstAttribute(); attr; attr = attr->Next())
            output.attrs.emplace_back(attr->Name(), attr->Value());
        for (auto child = element->FirstChildElement(); child; child = child->NextSiblingElement()) {
            XmlNode node;
            copy(child, node);
            output.kids.push_back(std::move(node));
        }
    }
    bool parse(XmlNode& root) {
        tinyxml2::XMLDocument document;
        if (document.Parse(source.data(), source.size()) != tinyxml2::XML_SUCCESS || !document.RootElement())
            return false;
        copy(document.RootElement(), root);
        return true;
    }
};

// GP voice assignment.
double note_value_dur(const std::string& v) {
    static const std::map<std::string, double> k = {
        {"Whole", 4.0},        {"Half", 2.0},         {"Quarter", 1.0},     {"Eighth", 0.5},
        {"16th", 0.25},        {"Sixteenth", 0.25},   {"32nd", 0.125},      {"ThirtySecond", 0.125},
        {"64th", 0.0625},      {"SixtyFourth", 0.0625}, {"128th", 0.03125},
    };
    auto it = k.find(v);
    return it == k.end() ? 1.0 : it->second;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::strchr(" \t\r\n", s[i])) ++i;
        const size_t st = i;
        while (i < s.size() && !std::strchr(" \t\r\n", s[i])) ++i;
        if (i > st) out.push_back(s.substr(st, i - st));
    }
    return out;
}

std::optional<int> note_midi(const XmlNode& n) {
    const XmlNode* props = n.child("Properties");
    if (!props) return std::nullopt;
    for (const auto& prop : props->kids) {
        if (prop.name != "Property") continue;
        const std::string* nm = prop.attr("name");
        if (!nm || *nm != "Midi") continue;
        const std::string* num = prop.findtext("Number");
        if (!num) return std::nullopt;
        char* end = nullptr;
        const double d = std::strtod(num->c_str(), &end);
        if (end == num->c_str()) return std::nullopt;
        return (int)d;
    }
    return std::nullopt;
}

}  // namespace

std::map<std::pair<int, int>, std::vector<int>> read_gp_voice_map(const std::string& gp_path,
                                                                  const std::string& track_name, int ppq) {
    std::map<std::pair<int, int>, std::vector<int>> out;
    std::string raw;
    try { raw = read_file(gp_path); }
    catch (const std::runtime_error&) { return out; }
    auto gpif = zip_read_entry(raw, "Content/score.gpif");
    if (!gpif) return out;
    XmlNode root;
    {
        XmlParser p{*gpif};
        if (!p.parse(root)) return out;
    }

    const XmlNode* tracks_el = root.child("Tracks");
    if (!tracks_el) return out;
    std::vector<std::string> names;
    int tr_idx = -1;
    for (const auto& t : tracks_el->kids) {
        const std::string* nm = t.findtext("Name");
        names.push_back(nm ? *nm : std::string());
    }
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == track_name) {
            tr_idx = (int)i;
            break;
        }
    }
    if (tr_idx < 0) return out;

    // 节奏时长
    std::map<std::string, double> durs;
    if (const XmlNode* rh_el = root.child("Rhythms")) {
        for (const auto& r : rh_el->kids) {
            const std::string* nv = r.findtext("NoteValue");
            double d = note_value_dur(nv ? *nv : "Quarter");
            if (const XmlNode* dot = r.child("AugmentationDot")) {
                const std::string* c = dot->attr("count");
                const int cnt = c ? std::atoi(c->c_str()) : 1;
                d *= 2 - std::pow(0.5, cnt);
            }
            if (const XmlNode* tup = r.child("Tuplet")) {
                const std::string* nn = tup->findtext("Num");
                const std::string* dd = tup->findtext("Den");
                const double n = nn ? std::atof(nn->c_str()) : 3.0;
                const double de = dd ? std::atof(dd->c_str()) : 2.0;
                d *= de / n;
            }
            const std::string* id = r.attr("id");
            durs[id ? *id : std::string()] = d;
        }
    }

    auto by_id = [](const XmlNode* el) {
        std::map<std::string, const XmlNode*> m;
        if (!el) return m;
        for (const auto& k : el->kids) {
            const std::string* id = k.attr("id");
            if (id) m[*id] = &k;
        }
        return m;
    };
    const auto bars = by_id(root.child("Bars"));
    const auto voices = by_id(root.child("Voices"));
    const auto beats = by_id(root.child("Beats"));
    const auto notes = by_id(root.child("Notes"));

    double pos = 0.0;
    const XmlNode* mbs = root.child("MasterBars");
    if (!mbs) return out;
    for (const auto& mbar : mbs->kids) {
        const std::string* tm = mbar.findtext("Time");
        const std::string ts = tm ? *tm : "4/4";
        const size_t slash = ts.find('/');
        const double a = std::atof(ts.substr(0, slash == std::string::npos ? ts.size() : slash).c_str());
        const double b = slash == std::string::npos ? 4.0 : std::atof(ts.substr(slash + 1).c_str());
        const double bar_len = 4.0 * a / b;

        const std::string* bs = mbar.findtext("Bars");
        const std::vector<std::string> ids = split_ws(bs ? *bs : std::string());
        if (tr_idx < (int)ids.size()) {
            auto bit = bars.find(ids[(size_t)tr_idx]);
            const XmlNode* bar = bit == bars.end() ? nullptr : bit->second;
            const std::string* vs = bar ? bar->findtext("Voices") : nullptr;
            const std::vector<std::string> vids = split_ws(vs ? *vs : std::string());
            for (size_t vi = 0; vi < vids.size(); ++vi) {
                const std::string& vid = vids[vi];
                if (vid.empty() || vid == "-1" || !voices.count(vid)) continue;
                double start = pos;
                const std::string* bs2 = voices.at(vid)->findtext("Beats");
                for (const std::string& bid : split_ws(bs2 ? *bs2 : std::string())) {
                    auto bi = beats.find(bid);
                    if (bi == beats.end()) continue;
                    const XmlNode* beat = bi->second;
                    const XmlNode* rh = beat->child("Rhythm");
                    const std::string ref = (rh && rh->attr("ref")) ? *rh->attr("ref") : std::string();
                    auto di = durs.find(ref);
                    const double dur = di == durs.end() ? 1.0 : di->second;
                    const std::string* ns = beat->findtext("Notes");
                    for (const std::string& nid : split_ws(ns ? *ns : std::string())) {
                        auto ni = notes.find(nid);
                        if (ni == notes.end()) continue;
                        auto p = note_midi(*ni->second);
                        if (p) out[{py_round_int(start * ppq), *p}].push_back((int)vi + 1);
                    }
                    start += dur;
                }
            }
        }
        pos += bar_len;
    }
    return out;
}

}  // namespace nyaa

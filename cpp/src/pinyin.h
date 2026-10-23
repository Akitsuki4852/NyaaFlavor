#pragma once
// 汉字 -> 拼音首音节（与 pypinyin.lazy_pinyin(...)[0] 完全一致，表由脚本生成）
#include <string>

namespace nyaa {
// 非 U+4E00..U+9FFF 或该字无拼音时返回空串
std::string pinyin_first(unsigned cp);
}  // namespace nyaa

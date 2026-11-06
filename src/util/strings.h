// strings.h — 纯字符串工具（不依赖 Win32）：UTF-8 ⇄ UTF-16、ASCII 小写、trim、按字符 split、iequals、ends_with_ci。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

namespace strings_detail {

inline void append_utf16(std::wstring& out, std::uint32_t cp) {
    if (cp < 0x10000) {
        out += static_cast<wchar_t>(cp);
    } else {
        cp -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (cp >> 10));
        out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
    }
}

inline void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

template <class C>
constexpr bool is_space(C c) {
    return c == C(' ') || c == C('\t') || c == C('\r') || c == C('\n') || c == C('\v') || c == C('\f');
}

template <class C>
constexpr C lower(C c) {
    return (c >= C('A') && c <= C('Z')) ? static_cast<C>(c - C('A') + C('a')) : c;
}

template <class C>
std::basic_string_view<C> trim(std::basic_string_view<C> s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

template <class C>
std::vector<std::basic_string_view<C>> split(std::basic_string_view<C> s, C sep) {
    std::vector<std::basic_string_view<C>> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::basic_string_view<C>::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

template <class C>
bool iequals(std::basic_string_view<C> a, std::basic_string_view<C> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

template <class C>
std::basic_string<C> to_lower(std::basic_string_view<C> s) {
    std::basic_string<C> out(s);
    for (auto& c : out) c = lower(c);
    return out;
}

}  // namespace strings_detail

// UTF-8 → UTF-16。非法序列（截断、过长编码、代理区、> U+10FFFF）各替换为一个 U+FFFD。
inline std::wstring utf8_to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) {
            out += static_cast<wchar_t>(b0);
            ++i;
            continue;
        }
        int len = 0;
        std::uint32_t cp = 0, min_cp = 0;
        if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1Fu;
            min_cp = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0Fu;
            min_cp = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07u;
            min_cp = 0x10000;
        } else {
            out += L'\xFFFD';
            ++i;
            continue;
        }
        int k = 1;
        for (; k < len && i + k < n; ++k) {
            const auto bk = static_cast<unsigned char>(s[i + k]);
            if ((bk & 0xC0) != 0x80) break;
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        if (k < len || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += L'\xFFFD';
            i += static_cast<std::size_t>(k);   // 跳过已读的前导字节和合法续字节
            continue;
        }
        strings_detail::append_utf16(out, cp);
        i += static_cast<std::size_t>(len);
    }
    return out;
}

// UTF-16 → UTF-8。不成对的代理项替换为 U+FFFD。
inline std::string wide_to_utf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < w.size()) {
                const std::uint32_t lo = static_cast<std::uint16_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        strings_detail::append_utf8(out, cp);
    }
    return out;
}

// 单个字符的 ASCII 小写（非 ASCII 字符原样返回）。
inline char ascii_lower(char c) { return strings_detail::lower(c); }
inline wchar_t ascii_lower(wchar_t c) { return strings_detail::lower(c); }

// 整串 ASCII 小写（非 ASCII 字符原样保留）。
inline std::string to_lower_ascii(std::string_view s) { return strings_detail::to_lower(s); }
inline std::wstring to_lower_ascii(std::wstring_view s) { return strings_detail::to_lower(s); }

// 去掉首尾空白（空格、\t、\r、\n、\v、\f）；返回原串的视图。
inline std::string_view trim(std::string_view s) { return strings_detail::trim(s); }
inline std::wstring_view trim(std::wstring_view s) { return strings_detail::trim(s); }

// 按单个分隔字符切分，保留空段（"a;;b" → "a","","b"；"" → 一个空段）；返回原串的视图。
inline std::vector<std::string_view> split(std::string_view s, char sep) { return strings_detail::split(s, sep); }
inline std::vector<std::wstring_view> split(std::wstring_view s, wchar_t sep) { return strings_detail::split(s, sep); }

// ASCII 大小写不敏感的相等比较。
inline bool iequals(std::string_view a, std::string_view b) { return strings_detail::iequals(a, b); }
inline bool iequals(std::wstring_view a, std::wstring_view b) { return strings_detail::iequals(a, b); }

// s 是否以 suffix 结尾（ASCII 大小写不敏感）。
inline bool ends_with_ci(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}
inline bool ends_with_ci(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// s 是否以 prefix 开头（ASCII 大小写不敏感）。
inline bool starts_with_ci(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}
inline bool starts_with_ci(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

}  // namespace mc

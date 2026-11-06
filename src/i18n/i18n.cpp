// i18n.cpp — i18n/i18n 的实现。
#include "i18n/i18n.h"

#include <windows.h>

namespace mc::i18n {
namespace {

Lang g_lang = Lang::ZhCN;

const wchar_t* const* table(Lang lang) {
    switch (lang) {
        case Lang::ZhCN: return kStringsZhCn;
        case Lang::ZhTW: return kStringsZhTw;
        case Lang::En: return kStringsEn;
    }
    return kStringsZhCn;
}

std::wstring lower_ascii(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c == L'_') c = L'-';
    }
    return out;
}

bool starts_with(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool is_name_char(wchar_t c) { return (c >= L'a' && c <= L'z') || c == L'_'; }

}  // namespace

Lang current() { return g_lang; }

void set_language(Lang lang) { g_lang = lang; }

const wchar_t* tr(StrId id, Lang lang) {
    const auto i = static_cast<std::size_t>(id);
    if (i >= kStrCount) return L"?";
    const wchar_t* s = table(lang)[i];
    if (s == nullptr || *s == 0) s = kStringsZhCn[i];
    return s != nullptr ? s : L"?";
}

std::wstring format_text(std::wstring_view pattern, std::initializer_list<Arg> args) {
    std::wstring out;
    out.reserve(pattern.size() + 16);
    std::size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == L'{') {
            std::size_t j = i + 1;
            while (j < pattern.size() && is_name_char(pattern[j])) ++j;
            if (j > i + 1 && j < pattern.size() && pattern[j] == L'}') {
                const std::wstring_view name = pattern.substr(i + 1, j - i - 1);
                const Arg* hit = nullptr;
                for (const Arg& a : args) {
                    if (a.name == name) hit = &a;
                }
                if (hit != nullptr) {
                    out += hit->value;
                    i = j + 1;
                    continue;
                }
            }
        }
        out += pattern[i];
        ++i;
    }
    return out;
}

std::vector<std::wstring> placeholders(std::wstring_view pattern) {
    std::vector<std::wstring> out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'{') continue;
        std::size_t j = i + 1;
        while (j < pattern.size() && is_name_char(pattern[j])) ++j;
        if (j > i + 1 && j < pattern.size() && pattern[j] == L'}') {
            std::wstring name(pattern.substr(i + 1, j - i - 1));
            bool seen = false;
            for (const auto& n : out) seen = seen || n == name;
            if (!seen) out.push_back(std::move(name));
            i = j;
        }
    }
    return out;
}

Lang lang_from_locale_name(std::wstring_view name) {
    const std::wstring n = lower_ascii(name);
    if (n != L"zh" && !starts_with(n, L"zh-")) return Lang::En;
    if (starts_with(n, L"zh-hant")) return Lang::ZhTW;
    if (starts_with(n, L"zh-hans")) return Lang::ZhCN;
    if (starts_with(n, L"zh-tw") || starts_with(n, L"zh-hk") || starts_with(n, L"zh-mo")) return Lang::ZhTW;
    return Lang::ZhCN;   // zh-CN、zh-SG、只有 zh
}

Lang detect_system_language() {
    ULONG count = 0;
    ULONG size = 0;
    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0) {
        std::wstring buf(size, L'\0');
        if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size) && count > 0 && buf[0] != 0) {
            return lang_from_locale_name(buf.c_str());   // 多字符串的第一项
        }
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    const LANGID id = ::GetUserDefaultUILanguage();
    if (::LCIDToLocaleName(MAKELCID(id, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        return lang_from_locale_name(name);
    }
    return Lang::En;
}

Lang resolve(LangSetting setting) {
    switch (setting) {
        case LangSetting::ZhCN: return Lang::ZhCN;
        case LangSetting::ZhTW: return Lang::ZhTW;
        case LangSetting::En: return Lang::En;
        case LangSetting::Auto: break;
    }
    return detect_system_language();
}

const wchar_t* lang_code(Lang lang) {
    switch (lang) {
        case Lang::ZhCN: return L"zh-CN";
        case Lang::ZhTW: return L"zh-TW";
        case Lang::En: return L"en";
    }
    return L"zh-CN";
}

bool parse_lang_setting(std::wstring_view text, LangSetting* out) {
    const std::wstring t = lower_ascii(text);
    LangSetting v{};
    if (t == L"auto") {
        v = LangSetting::Auto;
    } else if (t == L"zh-cn" || t == L"zh-hans") {
        v = LangSetting::ZhCN;
    } else if (t == L"zh-tw" || t == L"zh-hant") {
        v = LangSetting::ZhTW;
    } else if (t == L"en") {
        v = LangSetting::En;
    } else {
        return false;
    }
    if (out != nullptr) *out = v;
    return true;
}

}  // namespace mc::i18n

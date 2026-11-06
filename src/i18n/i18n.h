// i18n.h — 界面语言（localization）：当前语言、按 StrId 取文字、{name} 占位符格式化、从系统 UI 语言推断默认语言。
// 零依赖：文字表是编进 exe 的 constexpr 数组（strings_*.cpp），不用资源 DLL / MUI。
// 线程：当前语言只在主线程读写（设置窗口、托盘、overlay 文字都在主线程）。
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "i18n/strings.h"

namespace mc::i18n {

// 界面语言。
enum class Lang { ZhCN = 0, ZhTW = 1, En = 2 };
inline constexpr std::size_t kLangCount = 3;

// [ui] language 的取值：auto 跟随系统，其余为固定语言。
enum class LangSetting { Auto = 0, ZhCN = 1, ZhTW = 2, En = 3 };

// 当前语言（进程启动时为 zh-CN，直到宿主调用 set_language）。
Lang current();
void set_language(Lang lang);

// 某种语言下的文字；表里缺项（不应发生）时回退到 zh-CN，再不行返回 L"?"。返回的指针指向静态存储。
const wchar_t* tr(StrId id, Lang lang);
inline const wchar_t* tr(StrId id) { return tr(id, current()); }

// 占位符 {name} 的一个实参。
struct Arg {
    std::wstring_view name;
    std::wstring value;
};
// 把 pattern 中的每个 {name} 换成同名实参；没有对应实参的占位符原样保留。
std::wstring format_text(std::wstring_view pattern, std::initializer_list<Arg> args);
inline std::wstring format(StrId id, std::initializer_list<Arg> args) { return format_text(tr(id), args); }
// pattern 中出现的占位符名（按出现顺序去重）。测试用来检查三种语言的占位符集合一致。
std::vector<std::wstring> placeholders(std::wstring_view pattern);

// 系统 UI 语言名（BCP-47，例如 "zh-TW"、"zh-Hant-HK"、"en-US"）→ 界面语言：
// zh-TW / zh-HK / zh-MO / zh-Hant* → 繁體中文；zh-CN / zh-SG / zh-Hans* / 只有 "zh" → 简体中文；其余 → English。纯函数。
Lang lang_from_locale_name(std::wstring_view name);
// GetUserPreferredUILanguages（取第一个）；失败时用 GetUserDefaultUILanguage → LCIDToLocaleName。
Lang detect_system_language();
// LangSetting → Lang（Auto 时调用 detect_system_language）。
Lang resolve(LangSetting setting);
// "zh-CN" / "zh-TW" / "en"（日志与 settingsdemo 用）。
const wchar_t* lang_code(Lang lang);
// "zh-CN" / "zh-TW" / "en" / "auto"（不区分大小写；也接受 "zh-Hans" / "zh-Hant"）→ LangSetting；不认识时返回 false。
bool parse_lang_setting(std::wstring_view text, LangSetting* out);

}  // namespace mc::i18n

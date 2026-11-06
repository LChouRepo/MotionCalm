// ini.h — 纯 UTF-8 INI 文本层（不含 Win32、不知道键的类型）：解析为有序条目；按模板逐行替换值重建文件。
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::ini {

inline constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// 一个 key=value 条目。section / key 已转 ASCII 小写用于匹配；*_text 保留原写法。
struct Entry {
    std::string section;        // 小写节名；第一个节标题之前的键为空串
    std::string key;            // 小写键名
    std::string section_text;   // 节名原写法（已 trim）
    std::string key_text;       // 键名原写法（已 trim）
    std::string value;          // 原始值：第一个 = 之后、去掉首尾空白；不剥离行尾注释
    int line = 0;               // 1 起的行号
};

// 解析 UTF-8 文本：可选 BOM、CRLF/LF、[节] 后可跟注释、整行 ; / # 注释、按第一个 = 切分。
// 无法识别的行（没有 =、空键名、缺 ] 的节标题）被忽略。返回按出现顺序排列的全部条目（含重复键）。
std::vector<Entry> parse_ini(std::string_view utf8);

// 值里行尾注释的起点：第一个"空白 + ; 或 #"中 ; / # 的位置（值以 ; / # 开头也算）；没有返回 npos。
std::size_t find_inline_comment(std::string_view value);
// 去掉行尾注释并 trim（标量键用；列表键不要调用）。
std::string_view strip_inline_comment(std::string_view value);

// regenerate 的取值回调：参数为小写节名与小写键名；返回 nullopt 表示保留模板里的原值。
using Lookup = std::function<std::optional<std::string>(std::string_view section, std::string_view key)>;

// 未知键 / [extra] 键：name 为 "原节名.键"（或 [extra] 里的原键名），value 为原始值（UTF-8）。
using ExtraList = std::vector<std::pair<std::string, std::string>>;

// 变更 2 预留的"动态节"：模板中该节的键不固定，条目在该节标题及其后紧跟的注释行之后，按 key 字母序展开。
struct DynamicSection {
    std::string section;   // 小写节名
    std::vector<std::pair<std::string, std::string>> entries;
};

// 以模板为骨架重建整个文件：注释行、空行、节标题、键顺序原样保留；对每个 key=value 行用 lookup 的值替换
// "= 之后到行尾注释之前"的区间，原有行尾注释用空格补齐回原列（至少 1 个空格）。BOM 与换行风格沿用模板。
// extras 非空时在文件末尾追加空行 + [extra] 节。
std::string regenerate(std::string_view template_text, const Lookup& lookup, const ExtraList& extras,
                       const std::vector<DynamicSection>& dynamic_sections = {});

}  // namespace mc::ini

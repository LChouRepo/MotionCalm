// ini.cpp — config/ini 的实现：逐行解析与按模板重建（纯文本，不含 Win32）。
#include "config/ini.h"

#include <algorithm>

#include "util/strings.h"

namespace mc::ini {
namespace {

struct Line {
    std::string_view body;   // 不含换行符
    std::string_view eol;    // "\r\n"、"\n" 或空（最后一行没有换行）
};

std::string_view skip_bom(std::string_view text) {
    if (text.substr(0, kUtf8Bom.size()) == kUtf8Bom) text.remove_prefix(kUtf8Bom.size());
    return text;
}

std::vector<Line> split_lines(std::string_view text) {
    std::vector<Line> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.push_back(Line{text.substr(pos), {}});
            break;
        }
        std::size_t end = nl;
        std::size_t eol_start = nl;
        if (end > pos && text[end - 1] == '\r') eol_start = end - 1;
        lines.push_back(Line{text.substr(pos, eol_start - pos), text.substr(eol_start, nl + 1 - eol_start)});
        pos = nl + 1;
    }
    return lines;
}

bool is_comment(std::string_view trimmed) { return !trimmed.empty() && (trimmed[0] == ';' || trimmed[0] == '#'); }

bool is_blank_char(char c) { return c == ' ' || c == '\t'; }

// 节标题：返回节名（trim 后），不是节标题时返回 nullopt。
std::optional<std::string_view> section_name(std::string_view trimmed) {
    if (trimmed.empty() || trimmed[0] != '[') return std::nullopt;
    const std::size_t close = trimmed.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    return trim(trimmed.substr(1, close - 1));
}

}  // namespace

std::vector<Entry> parse_ini(std::string_view utf8) {
    std::vector<Entry> out;
    std::string section, section_text;
    int line_no = 0;
    for (const Line& ln : split_lines(skip_bom(utf8))) {
        ++line_no;
        const std::string_view t = trim(ln.body);
        if (t.empty() || is_comment(t)) continue;
        if (t[0] == '[') {
            if (const auto name = section_name(t)) {
                section_text.assign(*name);
                section = to_lower_ascii(*name);
            }
            continue;
        }
        const std::size_t eq = t.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(t.substr(0, eq));
        if (key.empty()) continue;
        Entry e;
        e.section = section;
        e.section_text = section_text;
        e.key = to_lower_ascii(key);
        e.key_text.assign(key);
        e.value.assign(trim(t.substr(eq + 1)));
        e.line = line_no;
        out.push_back(std::move(e));
    }
    return out;
}

std::size_t find_inline_comment(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((value[i] == ';' || value[i] == '#') && (i == 0 || is_blank_char(value[i - 1]))) return i;
    }
    return std::string_view::npos;
}

std::string_view strip_inline_comment(std::string_view value) {
    const std::size_t c = find_inline_comment(value);
    return trim(c == std::string_view::npos ? value : value.substr(0, c));
}

std::string regenerate(std::string_view template_text, const Lookup& lookup, const ExtraList& extras,
                       const std::vector<DynamicSection>& dynamic_sections) {
    const bool has_bom = template_text.substr(0, kUtf8Bom.size()) == kUtf8Bom;
    const std::vector<Line> lines = split_lines(skip_bom(template_text));
    std::string_view nl = "\r\n";
    for (const Line& ln : lines) {
        if (!ln.eol.empty()) {
            nl = ln.eol;
            break;
        }
    }

    std::string out;
    out.reserve(template_text.size() + 256);
    if (has_bom) out += kUtf8Bom;

    std::string section;
    const DynamicSection* pending_dynamic = nullptr;
    auto flush_dynamic = [&] {
        if (pending_dynamic == nullptr) return;
        auto entries = pending_dynamic->entries;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [k, v] : entries) {
            out += k;
            out += '=';
            out += v;
            out += nl;
        }
        pending_dynamic = nullptr;
    };

    for (const Line& ln : lines) {
        const std::string_view t = trim(ln.body);
        if (pending_dynamic != nullptr && !is_comment(t)) flush_dynamic();   // 注释块结束：展开动态条目

        if (!t.empty() && t[0] == '[') {
            if (const auto name = section_name(t)) {
                section = to_lower_ascii(*name);
                for (const auto& d : dynamic_sections) {
                    if (d.section == section) pending_dynamic = &d;
                }
            }
        } else if (!t.empty() && !is_comment(t)) {
            const std::size_t eq = ln.body.find('=');
            if (eq != std::string_view::npos) {
                const std::string key = to_lower_ascii(trim(ln.body.substr(0, eq)));
                std::optional<std::string> value;
                if (!key.empty() && lookup) value = lookup(section, key);
                if (value) {
                    std::size_t vstart = eq + 1;
                    while (vstart < ln.body.size() && is_blank_char(ln.body[vstart])) ++vstart;
                    const std::string_view rest = ln.body.substr(vstart);
                    const std::size_t c = find_inline_comment(rest);
                    std::string rebuilt(ln.body.substr(0, vstart));
                    rebuilt += *value;
                    if (c != std::string_view::npos) {
                        const std::size_t comment_col = vstart + c;
                        const std::size_t pad = comment_col > rebuilt.size() ? comment_col - rebuilt.size() : 1;
                        rebuilt.append(pad, ' ');
                        rebuilt += ln.body.substr(comment_col);
                    }
                    out += rebuilt;
                    out += ln.eol;
                    continue;
                }
            }
        }
        out += ln.body;
        out += ln.eol;
    }
    if (!lines.empty() && lines.back().eol.empty() && (pending_dynamic != nullptr || !extras.empty())) out += nl;
    flush_dynamic();

    if (!extras.empty()) {
        out += nl;
        out += "[extra]";
        out += nl;
        for (const auto& [k, v] : extras) {
            out += k;
            out += '=';
            out += v;
            out += nl;
        }
    }
    return out;
}

}  // namespace mc::ini

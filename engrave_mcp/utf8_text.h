#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace music {

// Escapes backslash, [quote] (0 escapes none), ASCII controls and the Unicode line breaks NEL, LS and PS, so the
// result never breaks a line or closes its quotes in a one-line text rendering.
inline std::string escape_text(std::string_view text, char quote = 0) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const auto rest = text.substr(i);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else if (c == '\\' || (quote && c == static_cast<unsigned char>(quote))) {
            out += '\\';
            out += static_cast<char>(c);
        } else if (rest.starts_with("\xc2\x85")) {
            out += "\\u0085";
            i += 1;
        } else if (rest.starts_with("\xe2\x80\xa8") || rest.starts_with("\xe2\x80\xa9")) {
            out += rest[2] == '\xa8' ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

inline bool utf8_exceeds_length(std::string_view text, std::size_t limit) {
    return text.size() > limit &&
           static_cast<std::size_t>(std::ranges::count_if(text, [](unsigned char c) { return (c & 0xC0) != 0x80; })) > limit;
}

// Largest prefix of [text] that ends on a UTF-8 code-point boundary.
inline std::size_t utf8_prefix(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return text.size();
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return cut;
}

// Abbreviate to [limit] bytes, keeping both ends and a UTF-8-safe boundary.
inline std::string utf8_abbrev(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);
    constexpr std::size_t ellipsis = 3;
    if (limit <= ellipsis) return std::string(text.substr(0, utf8_prefix(text, limit)));
    const std::size_t head_budget = (limit - ellipsis) / 2;
    const std::size_t head = utf8_prefix(text, head_budget);
    std::size_t tail_start = text.size() - (limit - ellipsis - head_budget);
    while (tail_start < text.size() && (static_cast<unsigned char>(text[tail_start]) & 0xC0) == 0x80) ++tail_start;
    std::string result;
    result.reserve(head + ellipsis + (text.size() - tail_start));
    result.append(text.substr(0, head));
    result.append("...");
    result.append(text.substr(tail_start));
    return result;
}

} // namespace music

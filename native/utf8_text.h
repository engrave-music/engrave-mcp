#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace music {

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

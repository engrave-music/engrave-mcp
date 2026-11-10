// SPDX-License-Identifier: GPL-3.0-only
#include "json.h"

namespace music {

namespace {

constexpr std::string_view replacement = "\xEF\xBF\xBD";

// Returns the length of a valid UTF-8 sequence starting at i, or the number of
// bytes to consume for one replacement (the lead plus its valid continuations).
std::size_t utf8_step(std::string_view text, std::size_t i, bool &valid) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80) {
        valid = true;
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (byte >= 0xC2 && byte <= 0xDF) {
        length = 2;
    } else if (byte == 0xE0) {
        length = 3;
        low = 0xA0;
    } else if (byte >= 0xE1 && byte <= 0xEC) {
        length = 3;
    } else if (byte == 0xED) {
        length = 3;
        high = 0x9F;
    } else if (byte >= 0xEE && byte <= 0xEF) {
        length = 3;
    } else if (byte == 0xF0) {
        length = 4;
        low = 0x90;
    } else if (byte >= 0xF1 && byte <= 0xF3) {
        length = 4;
    } else if (byte == 0xF4) {
        length = 4;
        high = 0x8F;
    } else {
        valid = false;
        return 1;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        if (i + offset >= text.size()) {
            valid = false;
            return offset;
        }
        const auto next = static_cast<unsigned char>(text[i + offset]);
        const unsigned char minimum = offset == 1 ? low : 0x80;
        const unsigned char maximum = offset == 1 ? high : 0xBF;
        if (next < minimum || next > maximum) {
            valid = false;
            return offset;
        }
    }
    valid = true;
    return length;
}

} // namespace

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        bool valid = false;
        const auto length = utf8_step(text, i, valid);
        if (!valid) return false;
        i += length;
    }
    return true;
}

std::string replace_invalid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        bool valid = false;
        const auto length = utf8_step(text, i, valid);
        if (valid) {
            out.append(text.substr(i, length));
        } else {
            out.append(replacement);
        }
        i += length;
    }
    return out;
}

namespace json_detail {

Json sanitize(const Json &value) {
    if (value.is_string()) {
        const auto &text = value.get<std::string>();
        if (valid_utf8(text)) return value;
        return Json(replace_invalid_utf8(text));
    }
    if (value.is_array()) {
        Json result = make_array<Json>();
        for (const auto &item : value.get_array()) json_push(result, sanitize(item));
        return result;
    }
    if (value.is_object()) {
        Json result = make_object<Json>();
        for (const auto &[key, item] : value.get_object()) {
            result[valid_utf8(key) ? key : replace_invalid_utf8(key)] = sanitize(item);
        }
        return result;
    }
    return value;
}

} // namespace json_detail

} // namespace music

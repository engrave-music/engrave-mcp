// SPDX-License-Identifier: GPL-3.0-only
// Host JSON DOM: sorted object keys (the previous serializer's order), u64 number
// mode, glaze's writer, and the wire's UTF-8 replacement rules.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/json/generic.hpp>

namespace music {

using Json = glz::generic_sorted_u64;

template <class J = Json>
J make_array() {
    return J(typename J::array_t{});
}

template <class J = Json>
J make_object() {
    return J(typename J::object_t{});
}

template <class J = Json, class... V>
J json_array(V &&...values) {
    J array = make_array<J>();
    (array.get_array().emplace_back(J(std::forward<V>(values))), ...);
    return array;
}

template <class J = Json>
J json_array(std::initializer_list<J> values) {
    J array = make_array<J>();
    for (const auto &value : values) array.get_array().push_back(value);
    return array;
}

template <class J, class V>
void json_push(J &array, V &&value) {
    array.get_array().emplace_back(std::forward<V>(value));
}

template <class J>
void json_erase(J &object, std::string_view key) {
    if (!object.is_object()) return;
    // find + erase(iterator): heterogeneous erase(key) is C++23 and missing from the pinned libstdc++.
    auto &members = object.get_object();
    if (const auto found = members.find(key); found != members.end()) members.erase(found);
}

// A missing key returns the fallback; integers convert through u64/i64/double.
template <class T, class J>
T json_value(const J &object, std::string_view key, T fallback) {
    if (!object.is_object()) return fallback;
    const auto &values = object.get_object();
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    const auto &value = found->second;
    if constexpr (std::same_as<T, J>) {
        return value;
    } else if constexpr (std::same_as<T, bool>) {
        return value.template get<bool>();
    } else if constexpr (std::integral<T> || std::floating_point<T>) {
        return value.template as<T>();
    } else {
        return value.template get<T>();
    }
}

// Object merge in source order: existing keys keep their position, new keys
// are appended (json::update semantics).
template <class J>
void json_merge(J &target, const J &source) {
    for (const auto &[key, value] : source.get_object()) target[key] = value;
}

template <class J>
bool json_equal(const J &left, const J &right) {
    if (left.is_number() && right.is_number()) {
        if (left.template holds<uint64_t>() && right.template holds<uint64_t>()) {
            return left.template get<uint64_t>() == right.template get<uint64_t>();
        }
        if (left.template holds<int64_t>() && right.template holds<int64_t>()) {
            return left.template get<int64_t>() == right.template get<int64_t>();
        }
        if (left.template holds<double>() || right.template holds<double>()) return left.as_number() == right.as_number();
        const auto unsigned_value = left.template holds<uint64_t>() ? left.template get<uint64_t>() : right.template get<uint64_t>();
        const auto signed_value = left.template holds<int64_t>() ? left.template get<int64_t>() : right.template get<int64_t>();
        return signed_value >= 0 && unsigned_value == static_cast<uint64_t>(signed_value);
    }
    if (left.template holds<std::string>() && right.template holds<std::string>()) {
        return left.template get<std::string>() == right.template get<std::string>();
    }
    if (left.template holds<bool>() && right.template holds<bool>()) return left.template get<bool>() == right.template get<bool>();
    if (left.template holds<std::nullptr_t>() && right.template holds<std::nullptr_t>()) return true;
    if (left.is_array() && right.is_array()) {
        const auto &a = left.get_array();
        const auto &b = right.get_array();
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!json_equal(a[i], b[i])) return false;
        }
        return true;
    }
    if (left.is_object() && right.is_object()) {
        const auto &a = left.get_object();
        const auto &b = right.get_object();
        if (a.size() != b.size()) return false;
        for (const auto &[key, value] : a) {
            const auto found = b.find(key);
            if (found == b.end() || !json_equal(value, found->second)) return false;
        }
        return true;
    }
    return false;
}

} // namespace music

namespace glz {

// Convenience comparisons for the host's sorted generic; numbers compare by
// value, as the previous integer/float comparisons did.
inline bool operator==(const music::Json &left, const music::Json &right) { return music::json_equal(left, right); }

inline bool operator!=(const music::Json &left, const music::Json &right) { return !music::json_equal(left, right); }

inline bool operator==(const music::Json &left, std::string_view right) {
    return left.is_string() && std::string_view(left.get<std::string>()).compare(right) == 0;
}

inline bool operator==(std::string_view left, const music::Json &right) { return right == left; }

inline bool operator!=(const music::Json &left, std::string_view right) { return !(left == right); }

inline bool operator!=(std::string_view left, const music::Json &right) { return !(right == left); }

inline bool operator==(const music::Json &left, const std::string &right) { return left == std::string_view(right); }

inline bool operator==(const std::string &left, const music::Json &right) { return right == std::string_view(left); }

inline bool operator!=(const music::Json &left, const std::string &right) { return !(left == right); }

inline bool operator!=(const std::string &left, const music::Json &right) { return !(right == left); }

inline bool operator==(const music::Json &left, const char *right) { return left == std::string_view(right); }

inline bool operator==(const char *left, const music::Json &right) { return right == std::string_view(left); }

inline bool operator!=(const music::Json &left, const char *right) { return !(left == right); }

inline bool operator!=(const char *left, const music::Json &right) { return !(right == left); }

template <std::integral T>
bool operator==(const music::Json &left, T right) {
    return music::json_equal(left, music::Json(right));
}

template <std::integral T>
bool operator==(T left, const music::Json &right) {
    return music::json_equal(music::Json(left), right);
}

template <std::integral T>
bool operator!=(const music::Json &left, T right) {
    return !(left == right);
}

template <std::integral T>
bool operator!=(T left, const music::Json &right) {
    return !(right == left);
}

} // namespace glz

namespace music {

// UTF-8 validation and repair compatible with the previous wire encoder:
// one U+FFFD per maximal invalid subsequence.
bool valid_utf8(std::string_view text);
std::string replace_invalid_utf8(std::string_view text);

// Non-throwing parse: the transport classifies errors from the result.
template <class J = Json>
std::optional<J> parse_json(std::string_view text);

template <>
std::optional<Json> parse_json<Json>(std::string_view text);

// Serialize with the wire UTF-8 rules; indent < 0 is compact, otherwise that
// many spaces per level.
std::string dump_json(const Json &value, int indent = -1);

} // namespace music

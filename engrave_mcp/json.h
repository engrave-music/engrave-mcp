// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/json/generic.hpp>
#include <gtl/btree.hpp>

namespace music {

// Indirected string-keyed B-tree map usable with incomplete value types
// (recursive glz::generic_json). Empty objects hold nullptr with zero heap
// allocation and keep sizeof == sizeof(void*).
template <class Value>
class SortedJsonMap {
public:
    using map_type = gtl::btree_map<std::string, Value, gtl::Less<std::string>>;
    using key_type = std::string;
    using mapped_type = Value;
    using value_type = std::pair<const std::string, Value>;
    using key_compare = std::less<>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    SortedJsonMap() noexcept = default;

    SortedJsonMap(const SortedJsonMap &other) : map_(other.map_ && !other.map_->empty() ? new map_type(*other.map_) : nullptr) {}

    SortedJsonMap(SortedJsonMap &&other) noexcept : map_(std::exchange(other.map_, nullptr)) {}

    SortedJsonMap &operator=(const SortedJsonMap &other) {
        if (this == &other) return *this;
        if (!other.map_ || other.map_->empty()) {
            if (map_) map_->clear();
            return *this;
        }
        if (map_) {
            *map_ = *other.map_;
        } else {
            map_ = new map_type(*other.map_);
        }
        return *this;
    }

    SortedJsonMap &operator=(SortedJsonMap &&other) noexcept {
        if (this != &other) {
            delete map_;
            map_ = std::exchange(other.map_, nullptr);
        }
        return *this;
    }

    ~SortedJsonMap() noexcept { delete map_; }

    [[nodiscard]] bool empty() const noexcept { return !map_ || map_->empty(); }

    [[nodiscard]] size_type size() const noexcept { return map_ ? map_->size() : 0; }

    void clear() noexcept {
        if (map_) map_->clear();
    }

    void swap(SortedJsonMap &other) noexcept { std::swap(map_, other.map_); }

    friend void swap(SortedJsonMap &lhs, SortedJsonMap &rhs) noexcept { lhs.swap(rhs); }

    auto begin() noexcept { return map_ ? map_->begin() : typename map_type::iterator{}; }

    auto end() noexcept { return map_ ? map_->end() : typename map_type::iterator{}; }

    auto begin() const noexcept { return map_ ? map_->cbegin() : typename map_type::const_iterator{}; }

    auto end() const noexcept { return map_ ? map_->cend() : typename map_type::const_iterator{}; }

    auto cbegin() const noexcept { return begin(); }

    auto cend() const noexcept { return end(); }

    auto find(std::string_view key) { return map_ ? map_->find(key) : typename map_type::iterator{}; }

    auto find(std::string_view key) const { return map_ ? map_->find(key) : typename map_type::const_iterator{}; }

    [[nodiscard]] bool contains(std::string_view key) const { return map_ && map_->find(key) != map_->end(); }

    [[nodiscard]] size_type count(std::string_view key) const { return contains(key) ? 1 : 0; }

    Value &at(std::string_view key) {
        if (map_) {
            if (auto it = map_->find(key); it != map_->end()) return it->second;
        }
        throw std::out_of_range("SortedJsonMap::at: key not found");
    }

    const Value &at(std::string_view key) const {
        if (map_) {
            if (auto it = map_->find(key); it != map_->end()) return it->second;
        }
        throw std::out_of_range("SortedJsonMap::at: key not found");
    }

    template <class K>
        requires std::convertible_to<const K &, std::string_view>
    Value &operator[](K &&key) {
        auto &m = ensure();
        if constexpr (std::same_as<K, std::string>) {
            return m[std::move(key)];
        } else {
            const std::string_view sv(key);
            if (auto it = m.find(sv); it != m.end()) return it->second;
            return m.emplace(std::string(sv), Value{}).first->second;
        }
    }

    template <class... Args>
    auto insert(Args &&...args) {
        return ensure().insert(std::forward<Args>(args)...);
    }

    template <class... Args>
    auto emplace(Args &&...args) {
        return ensure().emplace(std::forward<Args>(args)...);
    }

    template <class K, class... Args>
    auto try_emplace(K &&key, Args &&...args) {
        return ensure().try_emplace(std::forward<K>(key), std::forward<Args>(args)...);
    }

    size_type erase(std::string_view key) {
        if (!map_) return 0;
        auto it = map_->find(key);
        if (it == map_->end()) return 0;
        map_->erase(it);
        return 1;
    }

    template <class It>
        requires(!std::convertible_to<const It &, std::string_view>)
    auto erase(It pos) {
        return map_->erase(pos);
    }

    friend bool operator==(const SortedJsonMap &lhs, const SortedJsonMap &rhs) {
        if (lhs.size() != rhs.size()) return false;
        if (lhs.empty()) return true;
        return *lhs.map_ == *rhs.map_;
    }

private:
    map_type &ensure() {
        if (!map_) map_ = new map_type();
        return *map_;
    }

    map_type *map_ = nullptr;
};

using Json = glz::generic_json<glz::num_mode::u64, SortedJsonMap>;
static_assert(sizeof(SortedJsonMap<int>) == sizeof(void *));
static_assert(sizeof(Json) <= sizeof(glz::generic_u64));
#if defined(_LIBCPP_VERSION)
static_assert(sizeof(Json) == 32);
#endif

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
    object.get_object().erase(key);
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
// are appended.
template <class J>
void json_merge(J &target, const J &source) {
    for (const auto &[key, value] : source.get_object()) target[key] = value;
}

template <class J>
bool json_equal(const J &left, const J &right) {
    if (left.is_number() && right.is_number()) {
        if (left.template holds<std::uint64_t>() && right.template holds<std::uint64_t>()) {
            return left.template get<std::uint64_t>() == right.template get<std::uint64_t>();
        }
        if (left.template holds<std::int64_t>() && right.template holds<std::int64_t>()) {
            return left.template get<std::int64_t>() == right.template get<std::int64_t>();
        }
        if (left.template holds<double>() || right.template holds<double>()) return left.as_number() == right.as_number();
        const auto unsigned_value =
            left.template holds<std::uint64_t>() ? left.template get<std::uint64_t>() : right.template get<std::uint64_t>();
        const auto signed_value =
            left.template holds<std::int64_t>() ? left.template get<std::int64_t>() : right.template get<std::int64_t>();
        return signed_value >= 0 && unsigned_value == static_cast<std::uint64_t>(signed_value);
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

// Repair emits one U+FFFD per maximal invalid subsequence.
bool valid_utf8(std::string_view text) noexcept;
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

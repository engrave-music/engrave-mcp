#include <cassert>
#include <concepts>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "api_json_internal.h"

namespace music::api {

namespace {

// Which selector forms an operand accepts besides an EID, a segment address or an OpRef.
enum struct Operand { STRICT, MEASURE, STAFF, POSITION };

struct DecodeContext {
    usize index;
    const std::string &path;
    const ValueCatalogue &catalogue;
};

Result<engrave::Ref> decode_ref_operand(const Json &input, std::string_view key, Operand kind, const DecodeContext &ctx) {
    // validate_op admits integers only for measure and staff operands, and only positive ones.
    if (input.is_number() && kind == Operand::MEASURE) return engrave::MeasureIndex{input.as<i32>() - 1};
    if (input.is_number() && kind == Operand::STAFF) return engrave::StaffIndex{input.as<i32>() - 1};
    if (kind == Operand::POSITION && detail::is_relative_position(input)) {
        auto position = detail::relative_position(input);
        if (!position) return std::unexpected(detail::invalid_error(ctx.path + "." + std::string(key) + ": " + position.error().message));
        return *position;
    }
    auto r = detail::ref(input, true, ctx.index);
    if (!r) {
        return std::unexpected(
            detail::invalid_error(ctx.path + "." + std::string(key) + ": " + r.error().message + "; expected an EID or earlier op"));
    }
    return r;
}

Result<engrave::Value> decode_property_value(const Json &object, std::string_view field, const std::string &at,
                                             const ValueCatalogue &catalogue) {
    const auto &table = field == "style" ? catalogue.styles : catalogue.properties;
    const auto &key = object.get_object().at(field).get<std::string>();
    auto found = table.find(key);
    if (found == table.end()) {
        return std::unexpected(detail::invalid_error(at + "." + std::string(field) + ": unknown descriptor; expected a catalogue entry"));
    }
    return detail::value(object.get_object().at("value"), found->second, at + ".value");
}

Operand operand_for(OpFieldKind kind) {
    switch (kind) {
        case OpFieldKind::EDIT_REF:
        case OpFieldKind::PART_REF: return Operand::STRICT;
        case OpFieldKind::MEASURE_REF: return Operand::MEASURE;
        case OpFieldKind::STAFF_REF: return Operand::STAFF;
        case OpFieldKind::ANCHOR: return Operand::POSITION;
        default: assert(false); std::unreachable();
    }
}

template <class FieldT>
Result<void> decode_field(const Json &j, const OpFieldSpec &spec, FieldT &target, const DecodeContext &ctx) {
    using T = std::remove_cvref_t<FieldT>;
    if (!spec.required && !j.get_object().contains(spec.name)) return {};

    if constexpr (std::same_as<T, engrave::Ref> || std::same_as<T, std::optional<engrave::Ref>> || std::same_as<T, engrave::MeasureEnd>) {
        auto r = decode_ref_operand(j.get_object().at(spec.name), spec.name, operand_for(spec.kind), ctx);
        if (!r) return std::unexpected(std::move(r.error()));
        if constexpr (std::same_as<T, engrave::MeasureEnd>) {
            target = engrave::MeasureEnd{std::move(*r)};
        } else {
            target = std::move(*r);
        }
    } else if constexpr (std::same_as<T, engrave::EditAddress>) {
        auto decoded = detail::edit_address(j.get_object().at(spec.name));
        if (!decoded) {
            return std::unexpected(detail::invalid_error(ctx.path + "." + std::string(spec.name) + ": " + decoded.error().message));
        }
        target = std::move(*decoded);
    } else if constexpr (std::same_as<T, engrave::EntryAddress>) {
        auto decoded = detail::edit_address(j.get_object().at(spec.name));
        if (!decoded) {
            return std::unexpected(detail::invalid_error(ctx.path + "." + std::string(spec.name) + ": " + decoded.error().message));
        }
        if (auto *relative = std::get_if<engrave::RelativePosition>(&*decoded)) {
            target = *relative;
        } else if (auto *segment = std::get_if<engrave::SegmentAddress>(&std::get<engrave::Address>(*decoded))) {
            target = *segment;
        } else {
            return std::unexpected(
                detail::invalid_error(ctx.path + "." + std::string(spec.name) + ": unsupported address; expected a segment"));
        }
    } else if constexpr (std::same_as<T, std::optional<engrave::ScoreSel>>) {
        auto decoded = detail::score(j);
        if (!decoded) return std::unexpected(std::move(decoded.error()));
        target = *decoded;
    } else if constexpr (std::same_as<T, engrave::Fraction> || std::same_as<T, std::optional<engrave::Fraction>>) {
        auto parsed = detail::fraction(j.get_object().at(spec.name));
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        target = *parsed;
    } else if constexpr (std::same_as<T, std::string>) {
        const auto &text = j.get_object().at(spec.name).get<std::string>();
        if (spec.kind == OpFieldKind::XML_LONG_TEXT) {
            if (auto ok = detail::validate_xml_text(text); !ok) {
                return std::unexpected(
                    detail::invalid_error(ctx.path + "." + std::string(spec.name) + ": invalid XML character; expected XML 1.0 text"));
            }
        }
        target = text;
    } else if constexpr (std::same_as<T, engrave::GraceKind>) {
        auto kind = detail::grace_kind(j.get_object().at(spec.name).get<std::string>());
        if (!kind) {
            return std::unexpected(
                detail::invalid_error(ctx.path + "." + std::string(spec.name) + ": invalid choice; expected a listed grace kind"));
        }
        target = *kind;
    } else if constexpr (std::same_as<T, bool> || std::same_as<T, std::optional<bool>>) {
        target = j.get_object().at(spec.name).get<bool>();
    } else if constexpr (std::same_as<T, int> || std::same_as<T, std::optional<int>>) {
        target = j.get_object().at(spec.name).as<i32>();
    } else if constexpr (std::same_as<T, std::vector<engrave::Pitch>>) {
        for (const auto &p : j.get_object().at(spec.name).get_array()) {
            target.push_back({p.at("pitch").as<i32>(), p.contains("tpc") ? std::optional(p.at("tpc").as<i32>()) : std::nullopt});
        }
    } else if constexpr (std::same_as<T, std::vector<engrave::DrumKey>>) {
        for (const auto &key : j.get_object().at(spec.name).get_array()) target.push_back({key.as<int>()});
    } else if constexpr (std::same_as<T, std::vector<engrave::Ref>>) {
        for (const auto &p : j.get_object().at(spec.name).get_array()) {
            auto r = detail::ref(p, true, ctx.index);
            if (!r) return std::unexpected(std::move(r.error()));
            target.push_back(std::move(*r));
        }
    } else if constexpr (std::same_as<T, std::vector<int>>) {
        for (const auto &n : j.get_object().at(spec.name).get_array()) target.push_back(n.as<int>());
    } else if constexpr (std::same_as<T, engrave::Value>) {
        const std::string_view descriptor = spec.kind == OpFieldKind::STYLE_VALUE ? "style" : "property";
        auto val = decode_property_value(j, descriptor, ctx.path, ctx.catalogue);
        if (!val) return std::unexpected(std::move(val.error()));
        target = std::move(*val);
    } else if constexpr (std::same_as<T, std::vector<std::pair<std::string, engrave::Value>>>) {
        for (const auto &item : j.get_object().at(spec.name).get_array()) {
            auto val = decode_property_value(item, "name", ctx.path + ".props[" + std::to_string(target.size()) + "]", ctx.catalogue);
            if (!val) return std::unexpected(std::move(val.error()));
            target.emplace_back(item.at("name").get<std::string>(), std::move(*val));
        }
    } else {
        static_assert(sizeof(T) == 0, "unsupported op field type");
    }
    return {};
}

template <auto Accessor, class OpT>
Result<void> decode_bound_field(const Json &j, const OpFieldBind<Accessor> &binding, OpT &op, const DecodeContext &ctx) {
    return decode_field(j, binding.spec, access_op_member<Accessor>(op), ctx);
}

template <class OpT>
Result<engrave::Op> decode_typed_op(const Json &j, const DecodeContext &ctx) {
    OpT op{};
    Result<void> status{};
    std::apply(
        [&](const auto &...binding) {
            std::ignore = (((status = decode_bound_field(j, binding, op, ctx)), static_cast<bool>(status)) && ...);
        },
        OpTraits<OpT>::bindings);
    if (!status) return std::unexpected(std::move(status.error()));
    return engrave::Op{std::move(op)};
}

template <std::size_t... I>
Result<engrave::Op> decode_by_name(std::string_view name, const Json &j, const DecodeContext &ctx, std::index_sequence<I...>) {
    Result<engrave::Op> out = std::unexpected(detail::invalid_error("unknown operation"));
    std::ignore = ((name == OpTraits<std::variant_alternative_t<I, engrave::Op>>::name ?
                        (out = decode_typed_op<std::variant_alternative_t<I, engrave::Op>>(j, ctx), true) :
                        false) ||
                   ...);
    return out;
}

Result<engrave::Op> decode_op(const Json &j, usize index, const ValueCatalogue &catalogue) {
    if (auto ok = detail::validate_op(j, index); !ok) return std::unexpected(std::move(ok.error()));
    const auto &name = j.at("op").get<std::string>();
    const auto path = "ops[" + std::to_string(index) + "]";
    return decode_by_name(name, j, DecodeContext{index, path, catalogue}, std::make_index_sequence<std::variant_size_v<engrave::Op>>{});
}

} // namespace

Result<engrave::Batch> decode_batch(const Json &ops, const ValueCatalogue &catalogue) {
    if (!ops.is_array() || ops.size() > 1024) {
        return std::unexpected(HostError{"InvalidArgument", "ops must be an array of at most 1024 operations"});
    }
    engrave::Batch batch;
    batch.ops.reserve(ops.size());
    for (usize i = 0; i < ops.size(); ++i) {
        auto op = decode_op(ops[i], i, catalogue);
        if (!op) {
            auto err = std::move(op.error());
            err.op_index = i;
            return std::unexpected(std::move(err));
        }
        batch.ops.push_back(std::move(*op));
    }
    return batch;
}

} // namespace music::api

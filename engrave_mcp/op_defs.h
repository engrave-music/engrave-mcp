#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#include <engrave/edit.h>

#include "json.h"

namespace music::api {

enum struct OpFieldKind {
    EDIT_REF,
    PART_REF,
    MEASURE_REF,
    STAFF_REF,
    ANCHOR,
    AT_POS,
    ENTRY_AT_POS,
    SCORE_EXCERPT,
    FRACTION,
    TIME_SIG,
    TEXT,
    XML_LONG_TEXT,
    LAYOUT_KIND,
    MEASURE_KIND,
    GRACE_KIND,
    BOOL,
    KEY_INT,
    PITCH_INT,
    TPC_INT,
    INT_1_1024,
    INT_2_1024,
    PITCH_LIST,
    DRUM_KEY_LIST,
    PART_REF_LIST,
    INT_1_1024_LIST,
    PROP_VALUE,
    STYLE_VALUE,
    NAMED_PROPS,
};

struct OpFieldSpec {
    std::string_view name;
    OpFieldKind kind;
    bool required = true;
    const char *doc_override = nullptr;
};

struct OpSpec {
    std::string_view name;
    std::span<const OpFieldSpec> fields;
    Json (*example)();
};

template <auto Accessor>
struct OpFieldBind {
    OpFieldSpec spec;
};

template <auto Accessor>
constexpr OpFieldBind<Accessor> bind_field(std::string_view name, OpFieldKind kind, bool required = true,
                                           const char *doc_override = nullptr) noexcept {
    return {{name, kind, required, doc_override}};
}

template <auto Accessor, class OpT>
constexpr decltype(auto) access_op_member(OpT &op) noexcept {
    if constexpr (std::is_member_object_pointer_v<decltype(Accessor)>) {
        return op.*Accessor;
    } else {
        return Accessor(op);
    }
}

// Deliberately no primary definition: adding an Op alternative to engrave::Op breaks compilation until described here.
template <class OpT>
struct OpTraits;

namespace detail {

inline Json first_beat_example() { return Json{{"measure", 1}, {"staff", 1}, {"voice", 1}, {"offset", "0"}}; }

} // namespace detail

template <>
struct OpTraits<engrave::SetProperty> {
    static constexpr std::string_view name = "set_property";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetProperty::target>("target", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::SetProperty::property>("property", OpFieldKind::TEXT),
                                                     bind_field<&engrave::SetProperty::value>("value", OpFieldKind::PROP_VALUE));

    static Json example() { return {{"target", "A_A"}, {"property", "visible"}, {"value", true}}; }
};

template <>
struct OpTraits<engrave::ResetProperty> {
    static constexpr std::string_view name = "reset_property";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::ResetProperty::target>("target", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::ResetProperty::property>("property", OpFieldKind::TEXT));

    static Json example() { return {{"target", "A_A"}, {"property", "visible"}}; }
};

template <>
struct OpTraits<engrave::SetStyle> {
    static constexpr std::string_view name = "set_style";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetStyle::score>("excerpt", OpFieldKind::SCORE_EXCERPT, false),
                                                     bind_field<&engrave::SetStyle::style>("style", OpFieldKind::TEXT),
                                                     bind_field<&engrave::SetStyle::value>("value", OpFieldKind::STYLE_VALUE));

    static Json example() { return {{"style", "pageWidth"}, {"value", 8.5}}; }
};

template <>
struct OpTraits<engrave::EnterNotes> {
    static constexpr std::string_view name = "enter_notes";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::EnterNotes::at>("at", OpFieldKind::AT_POS),
                                                     bind_field<&engrave::EnterNotes::duration>("duration", OpFieldKind::FRACTION),
                                                     bind_field<&engrave::EnterNotes::pitches>("pitches", OpFieldKind::PITCH_LIST));

    static Json example() {
        return {{"at", detail::first_beat_example()}, {"duration", "1/4"}, {"pitches", json_array<Json>({Json{{"pitch", 60}}})}};
    }
};

template <>
struct OpTraits<engrave::SetPitch> {
    static constexpr std::string_view name = "set_pitch";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetPitch::note>("note", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::SetPitch::pitch>("pitch", OpFieldKind::PITCH_INT),
                                                     bind_field<&engrave::SetPitch::tpc>("tpc", OpFieldKind::TPC_INT, false));

    static Json example() { return {{"note", "A_A"}, {"pitch", 60}}; }
};

template <>
struct OpTraits<engrave::SetDuration> {
    static constexpr std::string_view name = "set_duration";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetDuration::chord_rest>("chord_rest", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::SetDuration::duration>("duration", OpFieldKind::FRACTION));

    static Json example() { return {{"chord_rest", "A_A"}, {"duration", "1/4"}}; }
};

template <>
struct OpTraits<engrave::Remove> {
    static constexpr std::string_view name = "remove";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::Remove::target>("target", OpFieldKind::EDIT_REF));

    static Json example() { return {{"target", "A_A"}}; }
};

template <>
struct OpTraits<engrave::InsertMeasures> {
    static constexpr std::string_view name = "insert_measures";
    static constexpr auto bindings = std::make_tuple(
        bind_field<&engrave::InsertMeasures::before>(
            "before", OpFieldKind::MEASURE_REF, false,
            "1-based bar number, measure EID or {from_op,item}; a number names the bar when the operation runs; omit to append"),
        bind_field<&engrave::InsertMeasures::count>("count", OpFieldKind::INT_1_1024),
        bind_field<&engrave::InsertMeasures::kind>("kind", OpFieldKind::MEASURE_KIND, false));

    static Json example() { return {{"before", 2}, {"count", 2}}; }
};

template <>
struct OpTraits<engrave::RemoveMeasures> {
    static constexpr std::string_view name = "remove_measures";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::RemoveMeasures::first>("first", OpFieldKind::MEASURE_REF),
                                                     bind_field<&engrave::RemoveMeasures::last>("last", OpFieldKind::MEASURE_REF));

    static Json example() { return {{"first", 2}, {"last", 3}}; }
};

template <>
struct OpTraits<engrave::SetTimeSig> {
    static constexpr std::string_view name = "set_time_sig";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetTimeSig::measure>("measure", OpFieldKind::MEASURE_REF),
                                                     bind_field<&engrave::SetTimeSig::sig>("sig", OpFieldKind::TIME_SIG));

    static Json example() { return {{"measure", 1}, {"sig", "6/8"}}; }
};

template <>
struct OpTraits<engrave::SetKeySig> {
    static constexpr std::string_view name = "set_key_sig";
    static constexpr auto bindings = std::make_tuple(
        bind_field<&engrave::SetKeySig::measure>("measure", OpFieldKind::MEASURE_REF),
        bind_field<&engrave::SetKeySig::key>("key", OpFieldKind::KEY_INT),
        bind_field<&engrave::SetKeySig::staff>(
            "staff", OpFieldKind::STAFF_REF, false,
            "1-based staff number, staff EID or {from_op,item}; a number names the staff when the operation runs; omit for every staff"));

    static Json example() { return {{"measure", 1}, {"key", 0}}; }
};

template <>
struct OpTraits<engrave::InsertClef> {
    static constexpr std::string_view name = "insert_clef";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::InsertClef::at>("at", OpFieldKind::AT_POS),
                                                     bind_field<&engrave::InsertClef::clef>("clef", OpFieldKind::TEXT));

    static Json example() { return {{"at", detail::first_beat_example()}, {"clef", "G"}}; }
};

template <>
struct OpTraits<engrave::SetLayoutBreak> {
    static constexpr std::string_view name = "set_layout_break";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::SetLayoutBreak::measure>("measure", OpFieldKind::MEASURE_REF),
                                                     bind_field<&engrave::SetLayoutBreak::kind>("kind", OpFieldKind::LAYOUT_KIND),
                                                     bind_field<&engrave::SetLayoutBreak::on>("on", OpFieldKind::BOOL));

    static Json example() { return {{"measure", 4}, {"kind", "line"}, {"on", true}}; }
};

template <>
struct OpTraits<engrave::AddPart> {
    static constexpr std::string_view name = "add_part";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddPart::instrument_id>("instrument_id", OpFieldKind::TEXT),
                                                     bind_field<&engrave::AddPart::before>("before", OpFieldKind::PART_REF, false));

    static Json example() { return {{"instrument_id", "piano"}}; }
};

template <>
struct OpTraits<engrave::RemovePart> {
    static constexpr std::string_view name = "remove_part";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::RemovePart::part>("part", OpFieldKind::PART_REF));

    static Json example() { return {{"part", "A_A"}}; }
};

template <>
struct OpTraits<engrave::AddStaff> {
    static constexpr std::string_view name = "add_staff";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddStaff::part>("part", OpFieldKind::PART_REF));

    static Json example() { return {{"part", "A_A"}}; }
};

template <>
struct OpTraits<engrave::RemoveStaff> {
    static constexpr std::string_view name = "remove_staff";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::RemoveStaff::staff>("staff", OpFieldKind::STAFF_REF));

    static Json example() { return {{"staff", 2}}; }
};

template <>
struct OpTraits<engrave::GenerateExcerpts> {
    static constexpr std::string_view name = "generate_excerpts";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::GenerateExcerpts::parts>("parts", OpFieldKind::PART_REF_LIST));

    static Json example() { return {{"parts", json_array<Json>({"A_A"})}}; }
};

template <>
struct OpTraits<engrave::AddSpanner> {
    static constexpr std::string_view name = "add_spanner";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddSpanner::type>("type", OpFieldKind::TEXT),
                                                     bind_field<&engrave::AddSpanner::start>("start", OpFieldKind::ANCHOR),
                                                     bind_field<&engrave::AddSpanner::end>("end", OpFieldKind::ANCHOR),
                                                     bind_field<&engrave::AddSpanner::props>("props", OpFieldKind::NAMED_PROPS, false));

    static Json example() {
        return {{"type", "HairPin"}, {"start", Json{{"measure", 1}}}, {"end", Json{{"measure", 2}, {"offset", "1/2"}}}};
    }
};

template <>
struct OpTraits<engrave::AddTie> {
    static constexpr std::string_view name = "add_tie";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddTie::note>("note", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::AddTie::end_note>("end_note", OpFieldKind::EDIT_REF, false));

    static Json example() { return {{"note", "A_A"}}; }
};

template <>
struct OpTraits<engrave::AddTuplet> {
    static constexpr std::string_view name = "add_tuplet";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddTuplet::first>("first", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::AddTuplet::last>("last", OpFieldKind::EDIT_REF),
                                                     bind_field<&engrave::AddTuplet::actual>("actual", OpFieldKind::INT_1_1024, false),
                                                     bind_field<&engrave::AddTuplet::normal>("normal", OpFieldKind::INT_1_1024, false));

    static Json example() { return {{"first", "A_A"}, {"last", "A_A"}}; }
};

template <>
struct OpTraits<engrave::AddText> {
    static constexpr std::string_view name = "add_text";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::AddText::at>("at", OpFieldKind::AT_POS),
                                                     bind_field<&engrave::AddText::style>("style", OpFieldKind::TEXT),
                                                     bind_field<&engrave::AddText::text>("text", OpFieldKind::XML_LONG_TEXT));

    static Json example() { return {{"at", detail::first_beat_example()}, {"style", "staff"}, {"text", "text"}}; }
};

template <>
struct OpTraits<engrave::Add> {
    static constexpr std::string_view name = "add";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::Add::anchor>("anchor", OpFieldKind::ANCHOR),
                                                     bind_field<&engrave::Add::type>("type", OpFieldKind::TEXT),
                                                     bind_field<&engrave::Add::props>("props", OpFieldKind::NAMED_PROPS, false));

    static Json example() {
        return {{"anchor", Json{{"measure", 1}}},
                {"type", "Articulation"},
                {"props", json_array<Json>({Json{{"name", "symbol"}, {"value", "articStaccatoAbove"}}})}};
    }
};

template <>
struct OpTraits<engrave::AddSpannerToBarEnd> {
    static constexpr std::string_view name = "add_spanner_to_bar_end";
    static constexpr auto bindings =
        std::make_tuple(bind_field<&engrave::AddSpannerToBarEnd::type>("type", OpFieldKind::TEXT),
                        bind_field<&engrave::AddSpannerToBarEnd::start>("start", OpFieldKind::ANCHOR),
                        bind_field<&engrave::AddSpannerToBarEnd::end>("end", OpFieldKind::MEASURE_REF),
                        bind_field<&engrave::AddSpannerToBarEnd::props>("props", OpFieldKind::NAMED_PROPS, false));

    static Json example() { return {{"type", "Pedal"}, {"start", Json{{"measure", 1}}}, {"end", 2}}; }
};

template <>
struct OpTraits<engrave::SetRepeatBarlines> {
    static constexpr std::string_view name = "set_repeat_barlines";
    static constexpr auto bindings =
        std::make_tuple(bind_field<&engrave::SetRepeatBarlines::measure>("measure", OpFieldKind::MEASURE_REF),
                        bind_field<&engrave::SetRepeatBarlines::start>("start", OpFieldKind::BOOL, false),
                        bind_field<&engrave::SetRepeatBarlines::end>("end", OpFieldKind::BOOL, false),
                        bind_field<&engrave::SetRepeatBarlines::count>("count", OpFieldKind::INT_2_1024, false));

    static Json example() { return {{"measure", 1}, {"start", true}}; }
};

template <>
struct OpTraits<engrave::AddVolta> {
    static constexpr std::string_view name = "add_volta";
    static constexpr auto bindings =
        std::make_tuple(bind_field<&engrave::AddVolta::first_measure>("first_measure", OpFieldKind::MEASURE_REF),
                        bind_field<&engrave::AddVolta::last_measure>("last_measure", OpFieldKind::MEASURE_REF),
                        bind_field<&engrave::AddVolta::endings>("endings", OpFieldKind::INT_1_1024_LIST),
                        bind_field<&engrave::AddVolta::closed>("closed", OpFieldKind::BOOL, false));

    static Json example() { return {{"first_measure", 3}, {"last_measure", 4}, {"endings", json_array<Json>({1})}}; }
};

template <>
struct OpTraits<engrave::AddGraceChord> {
    static constexpr std::string_view name = "add_grace_chord";
    static constexpr auto bindings = std::make_tuple(
        bind_field<&engrave::AddGraceChord::principal>("principal", OpFieldKind::EDIT_REF),
        bind_field<+[](engrave::AddGraceChord &op) noexcept -> auto & { return op.grace.kind; }>("kind", OpFieldKind::GRACE_KIND),
        bind_field<+[](engrave::AddGraceChord &op) noexcept -> auto & { return op.grace.written_duration; }>("duration",
                                                                                                             OpFieldKind::FRACTION),
        bind_field<+[](engrave::AddGraceChord &op) noexcept -> auto & { return op.grace.pitches; }>("pitches", OpFieldKind::PITCH_LIST),
        bind_field<&engrave::AddGraceChord::event_end>("event_end", OpFieldKind::FRACTION, false));

    static Json example() {
        return {{"principal", "A_A"}, {"kind", "acciac"}, {"duration", "1/4"}, {"pitches", json_array<Json>({Json{{"pitch", 60}}})}};
    }
};

template <>
struct OpTraits<engrave::EnterDrums> {
    static constexpr std::string_view name = "enter_drums";
    static constexpr auto bindings = std::make_tuple(bind_field<&engrave::EnterDrums::at>("at", OpFieldKind::ENTRY_AT_POS),
                                                     bind_field<&engrave::EnterDrums::duration>("duration", OpFieldKind::FRACTION),
                                                     bind_field<&engrave::EnterDrums::keys>("keys", OpFieldKind::DRUM_KEY_LIST));

    static Json example() { return {{"at", detail::first_beat_example()}, {"duration", "1/4"}, {"keys", json_array<Json>({60})}}; }
};

// Published tools.json / runtime_info wire order (distinct from engrave::Op variant index order).
using WireOpOrder = std::tuple<engrave::SetProperty, engrave::ResetProperty, engrave::SetStyle, engrave::EnterNotes, engrave::SetPitch,
                               engrave::SetDuration, engrave::Remove, engrave::InsertMeasures, engrave::RemoveMeasures, engrave::SetTimeSig,
                               engrave::SetKeySig, engrave::InsertClef, engrave::SetLayoutBreak, engrave::AddPart, engrave::RemovePart,
                               engrave::AddStaff, engrave::RemoveStaff, engrave::GenerateExcerpts, engrave::AddSpanner, engrave::AddTie,
                               engrave::AddTuplet, engrave::AddText, engrave::Add, engrave::AddSpannerToBarEnd, engrave::SetRepeatBarlines,
                               engrave::AddVolta, engrave::AddGraceChord, engrave::EnterDrums>;

namespace detail {

template <class Tuple, std::size_t... I>
consteval auto extract_field_specs(const Tuple &bindings, std::index_sequence<I...>) {
    return std::array<OpFieldSpec, sizeof...(I)>{std::get<I>(bindings).spec...};
}

template <class OpT>
inline constexpr auto op_fields_storage =
    extract_field_specs(OpTraits<OpT>::bindings, std::make_index_sequence<std::tuple_size_v<decltype(OpTraits<OpT>::bindings)>>{});

template <class... Ops>
consteval auto build_wire_op_specs(std::tuple<Ops...> *) {
    return std::array<OpSpec, sizeof...(Ops)>{OpSpec{OpTraits<Ops>::name, op_fields_storage<Ops>, &OpTraits<Ops>::example}...};
}

template <class T, class Tuple, std::size_t... I>
consteval std::size_t tuple_count(std::index_sequence<I...>) {
    return ((std::is_same_v<T, std::tuple_element_t<I, Tuple>> ? 1u : 0u) + ...);
}

template <std::size_t... I>
consteval bool verify_op_bijection(std::index_sequence<I...>) {
    constexpr auto wire_indices = std::make_index_sequence<std::tuple_size_v<WireOpOrder>>{};
    if (std::tuple_size_v<WireOpOrder> != std::variant_size_v<engrave::Op>) return false;
    const bool all_present_once = ((tuple_count<std::variant_alternative_t<I, engrave::Op>, WireOpOrder>(wire_indices) == 1) && ...);
    if (!all_present_once) return false;
    constexpr auto specs = build_wire_op_specs(static_cast<WireOpOrder *>(nullptr));
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].name.empty()) return false;
        for (std::size_t j = i + 1; j < specs.size(); ++j) {
            if (specs[i].name == specs[j].name) return false;
        }
    }
    return true;
}

static_assert(verify_op_bijection(std::make_index_sequence<std::variant_size_v<engrave::Op>>{}));

} // namespace detail

inline constexpr auto wire_op_specs = detail::build_wire_op_specs(static_cast<WireOpOrder *>(nullptr));

constexpr std::span<const OpSpec> op_specs() noexcept { return wire_op_specs; }

constexpr const OpSpec *find_op_spec(std::string_view name) noexcept {
    for (const auto &spec : wire_op_specs) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

} // namespace music::api

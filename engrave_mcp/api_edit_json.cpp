#include "api_json_internal.h"

namespace music::api {

namespace {

Result<engrave::Op> decode_op(const Json &j, usize index) {
    if (auto ok = detail::validate_def("score_edit", "op", j); !ok) return std::unexpected(std::move(ok.error()));
    const auto &name = j.at("op").get<std::string>();
    auto ref = [&](const char *key) -> Result<engrave::Ref> { return detail::ref(j.at(key), true, index); };
    auto opt_ref = [&](const char *key) -> Result<std::optional<engrave::Ref>> {
        if (!j.contains(key)) return std::optional<engrave::Ref>{std::nullopt};
        auto r = ref(key);
        if (!r) return std::unexpected(std::move(r.error()));
        return std::optional<engrave::Ref>{std::move(*r)};
    };
    auto str = [&](const char *key) -> std::string { return j.at(key).get<std::string>(); };
    auto props = [&]() -> Result<std::vector<std::pair<std::string, engrave::Value>>> {
        std::vector<std::pair<std::string, engrave::Value>> p;
        if (j.contains("props")) {
            for (const auto &item : j.at("props").get_array()) {
                auto val = detail::value(item.at("value"));
                if (!val) return std::unexpected(std::move(val.error()));
                p.emplace_back(item.at("name").get<std::string>(), std::move(*val));
            }
        }
        return p;
    };

    if (name == "set_property") {
        auto target = ref("target");
        if (!target) return std::unexpected(std::move(target.error()));
        auto val = detail::value(j.at("value"));
        if (!val) return std::unexpected(std::move(val.error()));
        return engrave::SetProperty{std::move(*target), str("property"), std::move(*val)};
    }
    if (name == "reset_property") {
        auto target = ref("target");
        if (!target) return std::unexpected(std::move(target.error()));
        return engrave::ResetProperty{std::move(*target), str("property")};
    }
    if (name == "set_style") {
        auto sel = detail::score(j.at("score"));
        if (!sel) return std::unexpected(std::move(sel.error()));
        auto val = detail::value(j.at("value"));
        if (!val) return std::unexpected(std::move(val.error()));
        return engrave::SetStyle{*sel, str("style"), std::move(*val)};
    }
    if (name == "enter_notes") {
        auto at = detail::address(j.at("at"));
        if (!at) return std::unexpected(std::move(at.error()));
        auto duration = detail::fraction(j.at("duration"));
        if (!duration) return std::unexpected(std::move(duration.error()));
        std::vector<engrave::Pitch> pitches;
        for (const auto &p : j.at("pitches").get_array()) {
            pitches.push_back({p.at("pitch").as<i32>(), p.contains("tpc") ? std::optional(p.at("tpc").as<i32>()) : std::nullopt});
        }
        return engrave::EnterNotes{std::move(*at), *duration, std::move(pitches)};
    }
    if (name == "set_pitch") {
        auto note = ref("note");
        if (!note) return std::unexpected(std::move(note.error()));
        return engrave::SetPitch{std::move(*note), j.at("pitch").as<i32>(),
                                 j.contains("tpc") ? std::optional(j.at("tpc").as<i32>()) : std::nullopt};
    }
    if (name == "set_duration") {
        auto cr = ref("chord_rest");
        if (!cr) return std::unexpected(std::move(cr.error()));
        auto duration = detail::fraction(j.at("duration"));
        if (!duration) return std::unexpected(std::move(duration.error()));
        return engrave::SetDuration{std::move(*cr), *duration};
    }
    if (name == "remove") {
        auto target = ref("target");
        if (!target) return std::unexpected(std::move(target.error()));
        return engrave::Remove{std::move(*target)};
    }
    if (name == "insert_measures") {
        auto before = opt_ref("before");
        if (!before) return std::unexpected(std::move(before.error()));
        return engrave::InsertMeasures{std::move(*before), j.at("count").as<i32>(), json_value<std::string>(j, "kind", "Measure")};
    }
    if (name == "remove_measures") {
        auto first = ref("first");
        if (!first) return std::unexpected(std::move(first.error()));
        auto last = ref("last");
        if (!last) return std::unexpected(std::move(last.error()));
        return engrave::RemoveMeasures{std::move(*first), std::move(*last)};
    }
    if (name == "set_time_sig") {
        auto measure = ref("measure");
        if (!measure) return std::unexpected(std::move(measure.error()));
        auto sig = detail::fraction(j.at("sig"));
        if (!sig) return std::unexpected(std::move(sig.error()));
        return engrave::SetTimeSig{std::move(*measure), *sig};
    }
    if (name == "set_key_sig") {
        auto measure = ref("measure");
        if (!measure) return std::unexpected(std::move(measure.error()));
        auto staff = opt_ref("staff");
        if (!staff) return std::unexpected(std::move(staff.error()));
        return engrave::SetKeySig{std::move(*measure), j.at("key").as<i32>(), std::move(*staff)};
    }
    if (name == "insert_clef") {
        auto at = detail::address(j.at("at"));
        if (!at) return std::unexpected(std::move(at.error()));
        return engrave::InsertClef{std::move(*at), str("clef")};
    }
    if (name == "set_layout_break") {
        auto measure = ref("measure");
        if (!measure) return std::unexpected(std::move(measure.error()));
        return engrave::SetLayoutBreak{std::move(*measure), str("kind"), j.at("on").get<bool>()};
    }
    if (name == "add_part") {
        auto before = opt_ref("before");
        if (!before) return std::unexpected(std::move(before.error()));
        return engrave::AddPart{str("instrument_id"), std::move(*before)};
    }
    if (name == "remove_part") {
        auto part = ref("part");
        if (!part) return std::unexpected(std::move(part.error()));
        return engrave::RemovePart{std::move(*part)};
    }
    if (name == "add_staff") {
        auto part = ref("part");
        if (!part) return std::unexpected(std::move(part.error()));
        return engrave::AddStaff{std::move(*part)};
    }
    if (name == "remove_staff") {
        auto staff = ref("staff");
        if (!staff) return std::unexpected(std::move(staff.error()));
        return engrave::RemoveStaff{std::move(*staff)};
    }
    if (name == "generate_excerpts") {
        std::vector<engrave::Ref> parts;
        for (const auto &p : j.at("parts").get_array()) {
            auto r = detail::ref(p, true, index);
            if (!r) return std::unexpected(std::move(r.error()));
            parts.push_back(std::move(*r));
        }
        return engrave::GenerateExcerpts{std::move(parts)};
    }
    if (name == "add_spanner") {
        auto start = ref("start");
        if (!start) return std::unexpected(std::move(start.error()));
        auto end = ref("end");
        if (!end) return std::unexpected(std::move(end.error()));
        auto p = props();
        if (!p) return std::unexpected(std::move(p.error()));
        return engrave::AddSpanner{str("type"), std::move(*start), std::move(*end), std::move(*p)};
    }
    if (name == "add_tie") {
        auto note = ref("note");
        if (!note) return std::unexpected(std::move(note.error()));
        return engrave::AddTie{std::move(*note)};
    }
    if (name == "add_tuplet") {
        auto first = ref("first");
        if (!first) return std::unexpected(std::move(first.error()));
        auto last = ref("last");
        if (!last) return std::unexpected(std::move(last.error()));
        return engrave::AddTuplet{std::move(*first), std::move(*last), json_value<i32>(j, "actual", 3), json_value<i32>(j, "normal", 2)};
    }
    if (name == "add_text") {
        auto text = str("text");
        if (auto ok = detail::validate_xml_text(text); !ok) return std::unexpected(std::move(ok.error()));
        auto at = detail::address(j.at("at"));
        if (!at) return std::unexpected(std::move(at.error()));
        return engrave::AddText{std::move(*at), str("style"), std::move(text)};
    }
    if (name == "add") {
        auto anchor = ref("anchor");
        if (!anchor) return std::unexpected(std::move(anchor.error()));
        auto p = props();
        if (!p) return std::unexpected(std::move(p.error()));
        return engrave::Add{std::move(*anchor), str("type"), std::move(*p)};
    }
    return std::unexpected(detail::invalid_error("unknown operation"));
}

} // namespace

Result<engrave::Batch> decode_batch(const Json &ops) {
    if (!ops.is_array() || ops.size() > 1024) {
        return std::unexpected(HostError{"InvalidArgument", "ops must be an array of at most 1024 operations"});
    }
    engrave::Batch batch;
    batch.ops.reserve(ops.size());
    for (usize i = 0; i < ops.size(); ++i) {
        auto op = decode_op(ops[i], i);
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

#include "api_json_internal.h"

namespace music::v2 {

Result<engrave::Batch> decode_batch(const Json& ops) {
    if (!ops.is_array() || ops.size() > 1024)
        return std::unexpected(HostError{"InvalidArgument","ops must be an array of at most 1024 operations"});
    engrave::Batch batch;
    batch.ops.reserve(ops.size());
    for (std::size_t i=0; i<ops.size(); ++i) {
        try {
            const auto& j=ops.at(i);
            detail::validate_def("score_edit","op",j);
            const auto name=j.at("op").get<std::string>();
            auto ref=[&](const char* key){ return detail::ref(j.at(key),true,i); };
            auto opt_ref=[&](const char* key)->std::optional<engrave::Ref> { return j.contains(key) ? std::optional(ref(key)) : std::nullopt; };
            auto str=[&](const char* key){ return j.at(key).get<std::string>(); };
            auto props=[&] {
                std::vector<std::pair<std::string,engrave::Value>> p;
                if (j.contains("props")) for (const auto& item:j.at("props")) p.emplace_back(item.at("name").get<std::string>(),detail::value(item.at("value")));
                return p;
            };
            if (name=="set_property") batch.ops.emplace_back(engrave::SetProperty{ref("target"),str("property"),detail::value(j.at("value"))});
            else if (name=="reset_property") batch.ops.emplace_back(engrave::ResetProperty{ref("target"),str("property")});
            else if (name=="set_style") batch.ops.emplace_back(engrave::SetStyle{detail::score(j.at("score")),str("style"),detail::value(j.at("value"))});
            else if (name=="enter_notes") {
                std::vector<engrave::Pitch> pitches;
                for (const auto& p:j.at("pitches")) pitches.push_back({p.at("pitch").get<int>(),p.contains("tpc")?std::optional(p.at("tpc").get<int>()):std::nullopt});
                batch.ops.emplace_back(engrave::EnterNotes{detail::address(j.at("at")),detail::fraction(j.at("duration")),std::move(pitches)});
            } else if (name=="set_pitch") batch.ops.emplace_back(engrave::SetPitch{ref("note"),j.at("pitch").get<int>(),j.contains("tpc")?std::optional(j.at("tpc").get<int>()):std::nullopt});
            else if (name=="set_duration") batch.ops.emplace_back(engrave::SetDuration{ref("chord_rest"),detail::fraction(j.at("duration"))});
            else if (name=="remove") batch.ops.emplace_back(engrave::Remove{ref("target")});
            else if (name=="insert_measures") batch.ops.emplace_back(engrave::InsertMeasures{opt_ref("before"),j.at("count").get<int>(),j.value("kind",std::string("Measure"))});
            else if (name=="remove_measures") batch.ops.emplace_back(engrave::RemoveMeasures{ref("first"),ref("last")});
            else if (name=="set_time_sig") batch.ops.emplace_back(engrave::SetTimeSig{ref("measure"),detail::fraction(j.at("sig"))});
            else if (name=="set_key_sig") batch.ops.emplace_back(engrave::SetKeySig{ref("measure"),j.at("key").get<int>(),opt_ref("staff")});
            else if (name=="insert_clef") batch.ops.emplace_back(engrave::InsertClef{detail::address(j.at("at")),str("clef")});
            else if (name=="set_layout_break") batch.ops.emplace_back(engrave::SetLayoutBreak{ref("measure"),str("kind"),j.at("on").get<bool>()});
            else if (name=="add_part") batch.ops.emplace_back(engrave::AddPart{str("instrument_id"),opt_ref("before")});
            else if (name=="remove_part") batch.ops.emplace_back(engrave::RemovePart{ref("part")});
            else if (name=="add_staff") batch.ops.emplace_back(engrave::AddStaff{ref("part")});
            else if (name=="remove_staff") batch.ops.emplace_back(engrave::RemoveStaff{ref("staff")});
            else if (name=="generate_excerpts") { std::vector<engrave::Ref> parts; for (const auto& p:j.at("parts")) parts.push_back(detail::ref(p,true,i)); batch.ops.emplace_back(engrave::GenerateExcerpts{std::move(parts)}); }
            else if (name=="add_spanner") batch.ops.emplace_back(engrave::AddSpanner{str("type"),ref("start"),ref("end"),props()});
            else if (name=="add_tie") batch.ops.emplace_back(engrave::AddTie{ref("note")});
            else if (name=="add_tuplet") batch.ops.emplace_back(engrave::AddTuplet{ref("first"),ref("last"),j.value("actual",3),j.value("normal",2)});
            else if (name=="add_text") { auto text=str("text"); detail::validate_xml_text(text); batch.ops.emplace_back(engrave::AddText{detail::address(j.at("at")),str("style"),std::move(text)}); }
            else if (name=="add") batch.ops.emplace_back(engrave::Add{ref("anchor"),str("type"),props()});
            else detail::invalid("unknown operation");
        } catch (const detail::DecodeError& error) {
            auto e=error.error; e.op_index=i; return std::unexpected(std::move(e));
        }
    }
    return batch;
}

} // namespace music::v2

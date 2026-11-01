#pragma once
#include "schema_defs.h"
#include <engrave/engrave.h>
#include <expected>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace music::v2 {
// Decimal codecs must enforce int64/uint64 bounds after schema validation.
// EIDs use element_id_from_string, never split JSON numbers. Unknown keys and
// forward/out-of-range OpRefs fail before Document::edit is entered.
struct HostError {
    std::string code, message;
    std::optional<std::size_t> op_index;
    std::optional<std::string> document;
    std::optional<std::uint64_t> current_revision;
    std::vector<std::string> published_artifacts;
    std::vector<engrave::ExportWarning> export_warnings;
};
template<class T> using Result = std::expected<T,HostError>;
Result<engrave::Batch> decode_batch(const Json& ops);
Result<engrave::Query> decode_query(const Json& query);
Result<engrave::Ref> decode_ref(const Json&, bool allow_op = false);
Result<engrave::ScoreSel> decode_score(const Json&);
Result<std::uint64_t> decode_revision(const Json&);
Json encode(const engrave::Outline&);
Json encode(const engrave::QueryResult&);
Json encode(const engrave::EditResult&);
Json encode(const engrave::AudioTracks&);
// Position artifacts use this mapping, independent of the legacy exporter.
Json encode(const engrave::Positions&);
Json encode(const engrave::Value&);
Json encode(const engrave::PropertyRead&);
Json encode(const engrave::TypeInfo&);
Json encode(const engrave::StyleInfo&);
Json encode(const engrave::Capabilities&);
Json encode(const engrave::TemplateInfo&);
Json encode(const engrave::Diagnostic&);
Json error_envelope(const HostError&);
Json encode(const std::vector<engrave::ExportWarning>&);
HostError api_error(const engrave::Error&);

// Deliberately no primary definition: any new Op alternative breaks compilation.
template<class T> struct OpWire;
#define MUSIC_OP(TYPE, NAME) template<> struct OpWire<engrave::TYPE> { static constexpr std::string_view name=NAME; };
MUSIC_OP(SetProperty,"set_property") MUSIC_OP(ResetProperty,"reset_property")
MUSIC_OP(SetStyle,"set_style") MUSIC_OP(EnterNotes,"enter_notes")
MUSIC_OP(SetPitch,"set_pitch") MUSIC_OP(SetDuration,"set_duration") MUSIC_OP(Remove,"remove")
MUSIC_OP(InsertMeasures,"insert_measures") MUSIC_OP(RemoveMeasures,"remove_measures")
MUSIC_OP(SetTimeSig,"set_time_sig") MUSIC_OP(SetKeySig,"set_key_sig")
MUSIC_OP(InsertClef,"insert_clef") MUSIC_OP(SetLayoutBreak,"set_layout_break")
MUSIC_OP(AddPart,"add_part") MUSIC_OP(RemovePart,"remove_part") MUSIC_OP(AddStaff,"add_staff")
MUSIC_OP(RemoveStaff,"remove_staff") MUSIC_OP(GenerateExcerpts,"generate_excerpts")
MUSIC_OP(AddSpanner,"add_spanner") MUSIC_OP(AddTie,"add_tie") MUSIC_OP(AddTuplet,"add_tuplet")
MUSIC_OP(AddText,"add_text") MUSIC_OP(Add,"add")
#undef MUSIC_OP
template<std::size_t... I> constexpr auto op_names(std::index_sequence<I...>) {
    return std::array{OpWire<std::variant_alternative_t<I,engrave::Op>>::name...};
}
inline constexpr auto operation_names=op_names(std::make_index_sequence<std::variant_size_v<engrave::Op>>{});
}

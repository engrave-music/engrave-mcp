#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <engrave/document.h>
#include <engrave/edit.h>
#include <engrave/engine.h>

#include "containers.h"
#include "op_defs.h"
#include "schema_defs.h"

namespace music::api {

struct HostError {
    std::string code, message;
    std::optional<std::size_t> op_index;
    std::optional<std::string> document;
    std::optional<std::uint64_t> current_revision;
    std::vector<std::string> published_artifacts;
    std::vector<engrave::ExportWarning> export_warnings;
    std::optional<bool> poisoned;
    std::optional<std::size_t> applied;
    std::optional<std::uint64_t> revision;
    std::optional<std::vector<engrave::ElementId>> created;
    std::optional<std::vector<engrave::ElementId>> removed;
    std::optional<std::vector<engrave::ElementId>> replaced;
    std::vector<engrave::Diagnostic> diagnostics;
};

template <class T>
using Result = std::expected<T, HostError>;

struct ValueCatalogue {
    HashMap<std::string, engrave::ValueKind> properties, styles;
};

// Leaves Batch::scope unset; score_edit applies its top-level excerpt.
Result<engrave::Batch> decode_batch(const Json &ops, const ValueCatalogue &catalogue = {});
Result<engrave::Query> decode_query(const Json &query);
Result<engrave::Ref> decode_ref(const Json &);
Result<engrave::ScoreSel> decode_score(const Json &);
Result<std::uint64_t> decode_revision(const Json &);

Json encode(const engrave::Outline &);
Json encode(const engrave::QueryResult &);
Json encode(const engrave::EditResult &);
Json encode(const engrave::AudioTracks &);

Json encode(const engrave::Value &);
Json encode(const engrave::PropertyRead &);
Json encode(const engrave::TypeInfo &);
Json encode(const engrave::StyleInfo &);
Json encode(const engrave::Capabilities &);
Json encode(const engrave::TemplateInfo &);
Json encode(const engrave::Diagnostic &);

Json error_envelope(const HostError &);
Json encode(const std::vector<engrave::ExportWarning> &);
HostError api_error(const engrave::Error &);

template <std::size_t... I>
constexpr auto op_names(std::index_sequence<I...>) {
    return std::array{OpTraits<std::variant_alternative_t<I, engrave::Op>>::name...};
}

inline constexpr auto operation_names = op_names(std::make_index_sequence<std::variant_size_v<engrave::Op>>{});

} // namespace music::api

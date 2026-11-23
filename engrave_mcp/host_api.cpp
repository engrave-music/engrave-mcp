#include "host_api.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <ankerl/unordered_dense.h>

#include "api_json_internal.h"
#include "phrase.h"

namespace music::api {

namespace {
namespace fs = std::filesystem;
using detail::invalid_error;

std::string ascii_lower(std::string text) {
    for (auto &c : text) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return text;
}

std::unexpected<HostError> document_error(const engrave::Error &error, const std::string &token) {
    auto e = api_error(error);
    e.document = token;
    return std::unexpected(std::move(e));
}

std::string nonce() {
    std::random_device rd;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (i32 i = 0; i < 4; ++i) {
        const auto n = static_cast<u32>(rd());
        for (i32 j = 0; j < 8; ++j) result.push_back(hex[(n >> (j * 4)) & 15]);
    }
    return result;
}

struct AssetFile {
    fs::path source, directory, file;

    ~AssetFile() {
        std::error_code ec;
        fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove(file, ec);
        fs::remove(directory, ec); // Only the private file and its directory, never a recursive deletion.
    }
};

Result<std::shared_ptr<const AssetFile>> copy_asset(const fs::path &source) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return std::unexpected(HostError{"NotFound", "runtime asset is not a file: " + detail::path_text(source)});
    }
    auto result = std::make_shared<AssetFile>();
    result->source = source;
    const auto temp = fs::temp_directory_path(ec);
    if (ec) return std::unexpected(HostError{"Io", "cannot locate runtime asset directory: " + ec.message()});
    for (i32 attempt = 0; attempt < 16; ++attempt) {
        result->directory = temp / ("engrave-mcp-asset-" + nonce());
        if (fs::create_directory(result->directory, ec)) break;
        if (ec && ec != std::errc::file_exists) {
            return std::unexpected(HostError{"Io", "cannot create runtime asset directory: " + ec.message()});
        }
        result->directory.clear();
    }
    if (result->directory.empty()) return std::unexpected(HostError{"Io", "cannot allocate runtime asset directory"});
    result->file = result->directory / source.filename();
    if (!fs::copy_file(source, result->file, fs::copy_options::none, ec)) {
        return std::unexpected(HostError{"Io", "cannot copy runtime asset: " + ec.message()});
    }
    fs::permissions(result->file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, ec);
    if (ec) return std::unexpected(HostError{"Io", "cannot protect runtime asset copy: " + ec.message()});
    return std::shared_ptr<const AssetFile>(std::move(result));
}

Result<bool> same_asset(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    auto a_size = fs::file_size(a, ec);
    if (ec) return std::unexpected(HostError{"Io", "cannot read retained font: " + ec.message()});
    auto b_size = fs::file_size(b, ec);
    if (ec) return std::unexpected(HostError{"Io", "cannot read retained font: " + ec.message()});
    if (a_size != b_size) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) return std::unexpected(HostError{"Io", "cannot open retained font"});
    std::array<char, 65536> x{}, y{};
    while (left && right) {
        left.read(x.data(), x.size());
        right.read(y.data(), y.size());
        if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
    }
    if (left.bad() || right.bad()) return std::unexpected(HostError{"Io", "cannot compare retained fonts"});
    return true;
}

template <class F>
Json boundary(F &&fn) {
    try {
        Result<Json> out = fn();
        return out ? std::move(*out) : error_envelope(out.error());
    } catch (const std::exception &e) {
        return error_envelope({"EngineFailure", e.what()});
    } catch (...) {
        return error_envelope({"EngineFailure", "unknown engine failure"});
    }
}

Result<std::optional<u64>> revision_arg(const Json &j) {
    if (!j.contains("expected_revision")) return std::optional<u64>{std::nullopt};
    auto rev = decode_revision(j.at("expected_revision"));
    if (!rev) return std::unexpected(std::move(rev.error()));
    return std::optional<u64>{*rev};
}

Result<std::vector<std::byte>> base64(const std::string &s) {
    usize padding = 0;
    if (!s.empty() && s.back() == '=') ++padding;
    if (s.size() > 1 && s[s.size() - 2] == '=') ++padding;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s.size() % 4 || s.find_first_not_of(alphabet) < s.size() - padding) {
        return std::unexpected(invalid_error("data: invalid encoding; expected padded base64"));
    }
    const auto size = s.size() / 4 * 3 - padding;
    if (size > engrave::max_input_bytes) return std::unexpected(HostError{"ResourceLimit", "decoded score exceeds 8 MiB"});
    std::vector<std::byte> bytes;
    bytes.reserve(size);
    auto digit = [](unsigned char c) -> u32 {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : 63;
    };
    for (usize i = 0; i < s.size(); i += 4) {
        u32 x = (digit(s[i]) << 18) | (digit(s[i + 1]) << 12) | (s[i + 2] == '=' ? 0 : digit(s[i + 2]) << 6) |
                (s[i + 3] == '=' ? 0 : digit(s[i + 3]));
        bytes.push_back(static_cast<std::byte>((x >> 16) & 255));
        if (s[i + 2] != '=') bytes.push_back(static_cast<std::byte>((x >> 8) & 255));
        if (s[i + 3] != '=') bytes.push_back(static_cast<std::byte>(x & 255));
    }
    return bytes;
}

Result<engrave::Format> input_format(const std::string &s) {
    if (s == "mscz") return engrave::Format::MSCZ;
    if (s == "mscx") return engrave::Format::MSCX;
    if (s == "musicxml") return engrave::Format::MUSIC_XML;
    if (s == "mxl") return engrave::Format::MXL;
    if (s == "midi") return engrave::Format::MIDI;
    if (s == "guitarpro") return engrave::Format::GUITAR_PRO;
    if (s == "mnx") return engrave::Format::MNX;
    return std::unexpected(invalid_error("unknown input format"));
}

template <class T>
Json encoded_array(const std::vector<T> &values) {
    Json result = make_array<Json>();
    for (const auto &value : values) json_push(result, encode(value));
    return result;
}

Result<Json> describe(engrave::Engine &engine, const Json &j, std::string_view scope) {
    if (scope == "types") return Json{{"scope", scope}, {"types", encoded_array(engine.describe())}};
    if (scope == "styles") return Json{{"scope", scope}, {"styles", encoded_array(engine.describe_styles())}};
    auto info = engine.describe(j.at("type").get<std::string>());
    if (!info) return std::unexpected(api_error(info.error()));
    return Json{{"scope", scope}, {"type", encode(*info)}};
}

std::vector<std::string> json_strings(const Json &array) {
    std::vector<std::string> result;
    for (const auto &item : array.get_array()) result.push_back(item.get<std::string>());
    return result;
}

template <class Fn>
Result<Json> run_checkpoint_op(const Json &j, engrave::Document &doc, std::string token, Fn &&fn) {
    auto out = fn(doc, j.at("name").get<std::string>());
    if (!out) {
        auto e = api_error(out.error());
        e.document = std::move(token);
        return std::unexpected(std::move(e));
    }
    return Json{{"ok", true}, {"revision", std::to_string(doc.revision())}};
}
} // namespace

// Each asset has a private, read-only file in a unique OS temporary directory.
// The host owns the private copies for the current engine selection.
struct RuntimeAssets {
    std::vector<std::shared_ptr<const AssetFile>> fonts, soundfonts;
};

struct HostApi::Impl {
    std::thread::id owner = std::this_thread::get_id();
    std::string session = nonce();
    u64 next = 1;
    engrave::EngineOptions options;
    ExportLimits limits;
    std::optional<ValueCatalogue> catalogue;
    std::shared_ptr<const RuntimeAssets> assets;
    std::unique_ptr<engrave::Engine> engine; // destroyed after documents, before assets
    ankerl::unordered_dense::map<std::string, std::unique_ptr<engrave::Document>> documents;
    ankerl::unordered_dense::set<std::string> borrowed;

    Result<void> check_thread() const {
        if (std::this_thread::get_id() != owner) {
            return std::unexpected(HostError{"EngineFailure", "HostApi called outside its owner thread"});
        }
        return {};
    }

    Result<engrave::Document *> get(const std::string &token, std::optional<u64> expected) {
        auto it = documents.find(token);
        if (it == documents.end()) return std::unexpected(HostError{"NotFound", "unknown or closed document", {}, token});
        auto *doc = it->second.get();
        const auto actual = doc->revision();
        if (expected && *expected != actual) {
            return std::unexpected(HostError{"RevisionConflict", "document revision does not match expected_revision", {}, token, actual});
        }
        return doc;
    }

    Result<Json> publish(std::unique_ptr<engrave::Document> doc) {
        if (next == 0) return std::unexpected(HostError{"ResourceLimit", "document token space exhausted"});
        auto token = "doc-" + session + "-" + std::to_string(next++);
        auto outline = doc->outline();
        if (!outline) return std::unexpected(api_error(outline.error()));
        Json out = {{"ok", true},
                    {"outline", encode(*outline)},
                    {"document", token},
                    {"revision", std::to_string(doc->revision())},
                    {"diagnostics", encoded_array(doc->diagnostics())}};
        documents.emplace(token, std::move(doc));
        return out;
    }

    Result<void> capacity() const {
        if (documents.size() >= 16) return std::unexpected(HostError{"ResourceLimit", "at most 16 documents may be open"});
        return {};
    }

    engrave::EngineOptions asset_options() const {
        auto result = options;
        result.fonts.clear();
        result.soundfonts.clear();
        for (const auto &f : assets->fonts) result.fonts.push_back(f->file);
        for (const auto &f : assets->soundfonts) result.soundfonts.push_back(f->file);
        return result;
    }
};

HostApi::HostApi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

HostApi::~HostApi() {
    if (impl_ && std::this_thread::get_id() != impl_->owner) std::terminate();
}

Result<std::unique_ptr<HostApi>> HostApi::create(const engrave::EngineOptions &options, ExportLimits limits) {
    try {
        auto impl = std::make_unique<Impl>();
        auto assets = std::make_shared<RuntimeAssets>();
        for (const auto &f : options.fonts) {
            auto copied = copy_asset(f);
            if (!copied) return std::unexpected(std::move(copied.error()));
            assets->fonts.push_back(std::move(*copied));
        }
        for (const auto &f : options.soundfonts) {
            auto copied = copy_asset(f);
            if (!copied) return std::unexpected(std::move(copied.error()));
            assets->soundfonts.push_back(std::move(*copied));
        }
        impl->assets = std::move(assets);
        impl->options = options;
        impl->limits = limits;
        if (!options.resources.empty()) impl->options.resources = fs::absolute(options.resources);
        if (!options.cache_dir.empty()) impl->options.cache_dir = fs::absolute(options.cache_dir);
        auto engine = engrave::Engine::create(impl->asset_options());
        if (!engine) return std::unexpected(api_error(engine.error()));
        impl->engine = std::move(*engine);
        return std::unique_ptr<HostApi>(new HostApi(std::move(impl)));
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what()});
    }
}

Json HostApi::open(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_open", j); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = impl_->capacity(); !ok) return std::unexpected(std::move(ok.error()));
        engrave::OpenOptions options;
        if (j.contains("format")) {
            auto fmt = input_format(j.at("format").get<std::string>());
            if (!fmt) return std::unexpected(std::move(fmt.error()));
            options.format = *fmt;
        }
        if (j.contains("filename_hint")) {
            auto hint = detail::path(j.at("filename_hint"));
            if (!hint) return std::unexpected(std::move(hint.error()));
            options.filename_hint = std::move(*hint);
        }
        if (j.contains("strict_ids")) options.strict_ids = j.at("strict_ids").get<bool>();
        engrave::Result<std::unique_ptr<engrave::Document>> opened;
        if (j.contains("path")) {
            auto p = detail::path(j.at("path"));
            if (!p) return std::unexpected(std::move(p.error()));
            opened = impl_->engine->open(*p, options);
        } else {
            auto bytes = base64(j.at("data").get<std::string>());
            if (!bytes) return std::unexpected(std::move(bytes.error()));
            opened = impl_->engine->open(*bytes, options);
        }
        if (!opened) return std::unexpected(api_error(opened.error()));
        return impl_->publish(std::move(*opened));
    });
}

Json HostApi::create_document(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_create", j); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = impl_->capacity(); !ok) return std::unexpected(std::move(ok.error()));
        engrave::CreateOptions o;
        if (!j.contains("template_path")) o.template_id = json_value<std::string>(j, "template_id", "Treble_Clef");
        if (j.contains("template_path")) {
            auto tp = detail::path(j.at("template_path"));
            if (!tp) return std::unexpected(std::move(tp.error()));
            o.template_path = std::move(*tp);
        }
        o.title = json_value<std::string>(j, "title", {});
        o.subtitle = json_value<std::string>(j, "subtitle", {});
        o.composer = json_value<std::string>(j, "composer", {});
        o.lyricist = json_value<std::string>(j, "lyricist", {});
        auto doc = impl_->engine->create_document(o);
        if (!doc) return std::unexpected(api_error(doc.error()));
        return impl_->publish(std::move(*doc));
    });
}

Json HostApi::close(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_close", j); !ok) return std::unexpected(std::move(ok.error()));
        const auto token = j.at("document").get<std::string>();
        if (impl_->borrowed.contains(token)) {
            return std::unexpected(HostError{"InvalidArgument", "document is in use by with_document", {}, token});
        }
        if (!impl_->documents.erase(token)) return std::unexpected(HostError{"NotFound", "unknown or closed document", {}, token});
        return Json{{"ok", true}};
    });
}

Json HostApi::outline(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_outline", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        engrave::ScoreSel selection{};
        if (j.contains("excerpt")) {
            auto sel = decode_score(j);
            if (!sel) return std::unexpected(std::move(sel.error()));
            selection = *sel;
        }
        auto result = (*doc)->outline(selection);
        if (!result) return document_error(result.error(), token);
        return Json{{"ok", true}, {"revision", std::to_string((*doc)->revision())}, {"outline", encode(*result)}};
    });
}

Json HostApi::query(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_query", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        const auto mode = j.at("mode").get<std::string>();
        Json result = {{"ok", true}, {"mode", mode}, {"revision", std::to_string((*doc)->revision())}};
        if (mode == "elements") {
            auto q = decode_query(json_value<Json>(j, "query", make_object<Json>()));
            if (!q) return std::unexpected(std::move(q.error()));
            if (j.contains("excerpt")) {
                auto sel = detail::score(j);
                if (!sel) return std::unexpected(std::move(sel.error()));
                q->score = *sel;
            }
            auto out = (*doc)->query(*q);
            if (!out) return document_error(out.error(), token);
            json_merge(result, encode(*out));
        } else if (mode == "properties") {
            auto target = decode_ref(j.at("target"));
            if (!target) return std::unexpected(std::move(target.error()));
            auto out = (*doc)->get(*target, j.contains("props") ? json_strings(j.at("props")) : std::vector<std::string>{});
            if (!out) return document_error(out.error(), token);
            result["properties"] = encoded_array(*out);
        } else if (mode == "page_count") {
            engrave::ScoreSel selection{};
            if (j.contains("excerpt")) {
                auto sel = decode_score(j);
                if (!sel) return std::unexpected(std::move(sel.error()));
                selection = *sel;
            }
            auto out = (*doc)->page_count(selection);
            if (!out) return document_error(out.error(), token);
            result["pages"] = *out;
        } else {
            engrave::ScoreSel selection{};
            if (j.contains("excerpt")) {
                auto sel = decode_score(j);
                if (!sel) return std::unexpected(std::move(sel.error()));
                selection = *sel;
            }
            auto out = (*doc)->audio_tracks(selection);
            if (!out) return document_error(out.error(), token);
            result["audio_tracks"] = encode(*out);
        }
        return result;
    });
}

Json HostApi::read_music(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_read_music", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        auto score = detail::score(j);
        if (!score) return std::unexpected(std::move(score.error()));
        engrave::MusicRange range;
        range.score = *score;
        range.first_measure = json_value<int>(j, "measure", 1) - 1;
        auto outline = (*doc)->outline(range.score);
        if (!outline) return document_error(outline.error(), token);
        if (range.first_measure >= outline->measure_count) {
            return std::unexpected(invalid_error("measure: outside score; expected an existing bar"));
        }
        range.measure_count = json_value<int>(j, "count", std::min(4, outline->measure_count - range.first_measure));
        if (range.measure_count > outline->measure_count - range.first_measure) {
            return std::unexpected(invalid_error("count: window extends past score; expected a count within the remaining bars"));
        }
        for (auto key : {"staves", "voices"}) {
            if (!j.contains(key)) continue;
            auto &indices = std::string_view(key) == "staves" ? range.staves : range.voices;
            for (const auto &n : j.at(key).get_array()) indices.push_back(n.as<int>() - 1);
        }
        auto window = (*doc)->read_music(range);
        if (!window) return document_error(window.error(), token);
        return music_result(*window, *outline, json_value<bool>(j, "include_ids", false));
    });
}

Json HostApi::write_phrase(const Json &j, const std::atomic<bool> *cancelled) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_write_phrase", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        auto score = detail::score(j);
        if (!score) return std::unexpected(std::move(score.error()));
        auto outline = (*doc)->outline(*score);
        if (!outline) return document_error(outline.error(), token);
        auto phrase = decode_phrase(j.at("lanes"), *outline, *score);
        if (!phrase) return std::unexpected(std::move(phrase.error()));
        if (cancelled && cancelled->load()) return std::unexpected(HostError{"Cancelled", "phrase cancelled before edit"});
        auto edited = (*doc)->write_phrase(*phrase, {*expected, engrave::EditMode::Atomic});
        if (!edited || edited->rejection) {
            auto error = api_error(edited ? edited->rejection->error : edited.error());
            error.document = token;
            error.op_index.reset();
            return std::unexpected(std::move(error));
        }
        return Json{{"ok", true}, {"revision", std::to_string(edited->revision)}, {"diagnostics", encoded_array(edited->diagnostics)}};
    });
}

Json HostApi::edit(const Json &j, const std::atomic<bool> *cancelled) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_edit", j); !ok) return std::unexpected(std::move(ok.error()));
        if (!impl_->catalogue) {
            ValueCatalogue catalogue;
            for (const auto &t : impl_->engine->describe()) {
                for (const auto &p : t.props) catalogue.properties.emplace(p.name, p.kind);
            }
            for (const auto &s : impl_->engine->describe_styles()) catalogue.styles.emplace(s.name, s.kind);
            impl_->catalogue = std::move(catalogue);
        }
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        auto outline = (*doc)->outline();
        if (!outline) return std::unexpected(api_error(outline.error()));
        auto batch_res = decode_batch(j.at("ops"), *impl_->catalogue, &*outline);
        if (!batch_res) {
            auto err = std::move(batch_res.error());
            err.applied = 0;
            return std::unexpected(std::move(err));
        }
        engrave::Result<engrave::EditResult> out = engrave::EditResult{};
        try {
            if (cancelled && cancelled->load()) return std::unexpected(HostError{"Cancelled", "batch cancelled before edit"});
            out =
                (*doc)->edit(*batch_res, engrave::EditOptions{*expected, json_value<bool>(j, "atomic", true) ? engrave::EditMode::Atomic :
                                                                                                               engrave::EditMode::Prefix});
        } catch (const std::exception &e) {
            return std::unexpected(HostError{"EngineFailure", e.what(), {}, token});
        } catch (...) {
            return std::unexpected(HostError{"EngineFailure", "unknown engine failure", {}, token});
        }
        if (!out) {
            auto e = api_error(out.error());
            e.document = token;
            if (e.code == "RevisionConflict") {
                e.current_revision = (*doc)->revision();
            } else if (e.code != "Poisoned") {
                e.applied = out.error().applied_ops.size();
            }
            return std::unexpected(std::move(e));
        }
        if (out->rejection) {
            auto e = api_error(out->rejection->error);
            e.document = token;
            e.op_index = out->rejection->op_index;
            e.applied = out->applied;
            e.revision = out->revision;
            e.created = std::move(out->created);
            e.removed = std::move(out->removed);
            e.replaced = std::move(out->replaced);
            e.diagnostics = std::move(out->diagnostics);
            return std::unexpected(std::move(e));
        }
        auto result = encode(*out);
        if (!json_value<bool>(j, "detail", false)) json_erase(result, "ops");
        result["ok"] = true;
        return result;
    });
}

Json HostApi::checkpoint(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("checkpoint", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        return run_checkpoint_op(j, **doc, token, [](engrave::Document &d, std::string_view name) { return d.checkpoint(name); });
    });
}

Json HostApi::restore_checkpoint(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("restore_checkpoint", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        return run_checkpoint_op(j, **doc, token, [](engrave::Document &d, std::string_view name) { return d.restore_checkpoint(name); });
    });
}

Json HostApi::drop_checkpoint(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("drop_checkpoint", j); !ok) return std::unexpected(std::move(ok.error()));
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        return run_checkpoint_op(j, **doc, token, [](engrave::Document &d, std::string_view name) { return d.drop_checkpoint(name); });
    });
}

Json operation_catalogue(std::string_view requested) {
    Json entries = make_array<Json>();
    auto add = [&](const char *op, Json fields, Json required, Json example) {
        if (!requested.empty() && requested != op) return;
        example["op"] = op;
        json_push(entries,
                  Json{{"op", op}, {"fields", std::move(fields)}, {"required", std::move(required)}, {"example", std::move(example)}});
    };
    add("set_property",
        {{"target", "EID, layout address or {from_op,item}"},
         {"property", "string"},
         {"value", "native value of the named property or style"}},
        json_array<Json>({"target", "property", "value"}), {{"target", "A_A"}, {"property", "visible"}, {"value", true}});
    add("reset_property", {{"target", "EID, layout address or {from_op,item}"}, {"property", "string"}},
        json_array<Json>({"target", "property"}), {{"target", "A_A"}, {"property", "visible"}});
    add("set_style", {{"excerpt", "persistent element ID"}, {"style", "string"}, {"value", "native value of the named property or style"}},
        json_array<Json>({"style", "value"}), {{"style", "pageWidth"}, {"value", 8.5}});
    add("enter_notes",
        {{"at", "{measure, offset?, staff?, voice?}; measure, staff and voice are 1-based"},
         {"duration", "exact whole-note fraction string"},
         {"pitches", "array of {pitch, tpc?}"}},
        json_array<Json>({"at", "duration", "pitches"}),
        {{"at", Json{{"measure", 1}, {"staff", 1}, {"voice", 1}, {"offset", "0"}}},
         {"duration", "1/4"},
         {"pitches", json_array<Json>({Json{{"pitch", 60}}})}});
    add("set_pitch",
        {{"note", "EID, layout address or {from_op,item}"},
         {"pitch", "MIDI integer, 0 through 127"},
         {"tpc", "tonal pitch class integer, -8 through 40"}},
        json_array<Json>({"note", "pitch"}), {{"note", "A_A"}, {"pitch", 60}});
    add("set_duration", {{"chord_rest", "EID, layout address or {from_op,item}"}, {"duration", "exact whole-note fraction string"}},
        json_array<Json>({"chord_rest", "duration"}), {{"chord_rest", "A_A"}, {"duration", "1/4"}});
    add("remove", {{"target", "EID, layout address or {from_op,item}"}}, json_array<Json>({"target"}), {{"target", "A_A"}});
    add("insert_measures",
        {{"before", "EID, layout address or {from_op,item}"},
         {"count", "integer, 1 through 1024"},
         {"kind", "Measure | VBox | HBox | TBox | FBox"}},
        json_array<Json>({"count"}), {{"count", 2}});
    add("remove_measures", {{"first", "EID, layout address or {from_op,item}"}, {"last", "EID, layout address or {from_op,item}"}},
        json_array<Json>({"first", "last"}), {{"first", "A_A"}, {"last", "A_A"}});
    add("set_time_sig",
        {{"measure", "EID, layout address or {from_op,item} or 1-based position"}, {"sig", "exact whole-note fraction string"}},
        json_array<Json>({"measure", "sig"}), {{"measure", 1}, {"sig", "6/8"}});
    add("set_key_sig",
        {{"measure", "EID, layout address or {from_op,item} or 1-based position"},
         {"key", "integer, -7 through 7"},
         {"staff", "EID, layout address or {from_op,item} or 1-based position"}},
        json_array<Json>({"measure", "key"}), {{"measure", "A_A"}, {"key", 0}});
    add("insert_clef", {{"at", "{measure, offset?, staff?, voice?}; measure, staff and voice are 1-based"}, {"clef", "string"}},
        json_array<Json>({"at", "clef"}), {{"at", Json{{"measure", 1}, {"staff", 1}, {"voice", 1}, {"offset", "0"}}}, {"clef", "G"}});
    add("set_layout_break",
        {{"measure", "EID, layout address or {from_op,item} or 1-based position"},
         {"kind", "line | page | section | nobreak"},
         {"on", "boolean"}},
        json_array<Json>({"measure", "kind", "on"}), {{"measure", "A_A"}, {"kind", "page"}, {"on", true}});
    add("add_part", {{"instrument_id", "string"}, {"before", "EID, layout address or {from_op,item}"}}, json_array<Json>({"instrument_id"}),
        {{"instrument_id", "piano"}});
    add("remove_part", {{"part", "EID, layout address or {from_op,item}"}}, json_array<Json>({"part"}), {{"part", "A_A"}});
    add("add_staff", {{"part", "EID, layout address or {from_op,item}"}}, json_array<Json>({"part"}), {{"part", "A_A"}});
    add("remove_staff", {{"staff", "EID, layout address or {from_op,item} or 1-based position"}}, json_array<Json>({"staff"}),
        {{"staff", "A_A"}});
    add("generate_excerpts", {{"parts", "array of EID, layout address or {from_op,item}"}}, json_array<Json>({"parts"}),
        {{"parts", json_array<Json>({"A_A"})}});
    add("add_spanner",
        {{"type", "string"},
         {"start", "EID, layout address or {from_op,item}"},
         {"end", "EID, layout address or {from_op,item}"},
         {"props", "array of {name, value}"}},
        json_array<Json>({"type", "start", "end"}), {{"type", "HairPin"}, {"start", "A_A"}, {"end", "A_A"}});
    add("add_tie", {{"note", "EID, layout address or {from_op,item}"}, {"end_note", "EID, layout address or {from_op,item}"}},
        json_array<Json>({"note"}), {{"note", "A_A"}});
    add("add_tuplet",
        {{"first", "EID, layout address or {from_op,item}"},
         {"last", "EID, layout address or {from_op,item}"},
         {"actual", "integer, 1 through 1024"},
         {"normal", "integer, 1 through 1024"}},
        json_array<Json>({"first", "last"}), {{"first", "A_A"}, {"last", "A_A"}});
    add("add_text",
        {{"at", "{measure, offset?, staff?, voice?}; measure, staff and voice are 1-based"},
         {"style", "string"},
         {"text", "XML-compatible string"}},
        json_array<Json>({"at", "style", "text"}),
        {{"at", Json{{"measure", 1}, {"staff", 1}, {"voice", 1}, {"offset", "0"}}}, {"style", "staff"}, {"text", "text"}});
    add("add", {{"anchor", "EID, layout address or {from_op,item}"}, {"type", "string"}, {"props", "array of {name, value}"}},
        json_array<Json>({"anchor", "type"}),
        {{"anchor", "A_A"},
         {"type", "Articulation"},
         {"props", json_array<Json>({Json{{"name", "symbol"}, {"value", "articStaccatoAbove"}}})}});
    add("add_spanner_to_bar_end",
        {{"type", "string"},
         {"start", "EID, layout address or {from_op,item}"},
         {"end", "measure EID or 1-based bar"},
         {"props", "array of {name, value}"}},
        json_array<Json>({"type", "start", "end"}), {{"type", "HairPin"}, {"start", "A_A"}, {"end", "A_A"}});
    add("set_repeat_barlines",
        {{"measure", "EID, layout address or {from_op,item} or 1-based position"},
         {"start", "boolean"},
         {"end", "boolean"},
         {"count", "integer, 1 through 1024"}},
        json_array<Json>({"measure"}), {{"measure", "A_A"}});
    add("add_volta",
        {{"first_measure", "EID, layout address or {from_op,item} or 1-based position"},
         {"last_measure", "EID, layout address or {from_op,item} or 1-based position"},
         {"endings", "array of integer, 1 through 1024"},
         {"closed", "boolean"}},
        json_array<Json>({"first_measure", "last_measure", "endings"}),
        {{"first_measure", "A_A"}, {"last_measure", "A_A"}, {"endings", json_array<Json>({2})}});
    add("add_grace_chord",
        {{"principal", "EID, layout address or {from_op,item}"},
         {"kind", "acciac | appog | grace4 | grace16 | grace32 | after8 | after16 | after32"},
         {"duration", "exact whole-note fraction string"},
         {"pitches", "array of {pitch, tpc?}"},
         {"event_end", "exact whole-note fraction string"}},
        json_array<Json>({"principal", "kind", "duration", "pitches"}),
        {{"principal", "A_A"}, {"kind", "acciac"}, {"duration", "1/4"}, {"pitches", json_array<Json>({Json{{"pitch", 60}}})}});
    add("enter_drums",
        {{"at", "{measure, offset?, staff?, voice?}; measure, staff and voice are 1-based"},
         {"duration", "exact whole-note fraction string"},
         {"keys", "array of MIDI integer, 0 through 127"}},
        json_array<Json>({"at", "duration", "keys"}),
        {{"at", Json{{"measure", 1}, {"staff", 1}, {"voice", 1}, {"offset", "0"}}}, {"duration", "1/4"}, {"keys", json_array<Json>({60})}});
    return entries;
}

Json HostApi::runtime_info(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("runtime_info", j); !ok) return std::unexpected(std::move(ok.error()));
        const auto scope = json_value<std::string>(j, "scope", "overview");
        Json catalog = make_object<Json>();
        if (scope == "templates") catalog["templates"] = encoded_array(impl_->engine->templates());
        if (scope == "type" || scope == "types" || scope == "styles") {
            auto description = describe(*impl_->engine, j, scope);
            if (!description) return std::unexpected(std::move(description.error()));
            catalog = std::move(*description);
        }
        if (scope == "ops") {
            auto ops = operation_catalogue(json_value<std::string>(j, "op", {}));
            if (ops.empty()) return std::unexpected(invalid_error("op: unknown operation; expected a registered edit operation"));
            catalog["ops"] = std::move(ops);
        }
        if (scope == "instruments") {
            Json instruments = make_array<Json>();
            const auto query = ascii_lower(json_value<std::string>(j, "query", {}));
            for (const auto &i : impl_->engine->instruments()) {
                if (!query.empty() && !ascii_lower(i.id).contains(query) && !ascii_lower(i.name).contains(query) &&
                    !ascii_lower(i.group_id).contains(query)) {
                    continue;
                }
                Json drums = make_array<Json>();
                for (const auto &d : i.drums) {
                    json_push(drums, Json{{"key", d.key}, {"name", d.name}, {"line", d.line}, {"voice", d.default_voice + 1}});
                }
                json_push(instruments, Json{{"id", i.id},
                                            {"name", i.name},
                                            {"group_id", i.group_id},
                                            {"staff_count", i.staff_count},
                                            {"uses_drumset", i.uses_drumset},
                                            {"transpose", {{"diatonic", i.transpose.diatonic}, {"chromatic", i.transpose.chromatic}}},
                                            {"drums", std::move(drums)}});
            }
            catalog["instruments"] = std::move(instruments);
        }
        auto caps = impl_->engine->capabilities();
        auto opts = impl_->asset_options();
        caps.soundfonts = opts.soundfonts;
        Json fonts = make_array<Json>();
        for (const auto &path : opts.fonts) json_push(fonts, detail::path_text(path));
        Json result{{"ok", true},
                    {"tool_revision", "3"},
                    {"capabilities", encode(caps)},
                    {"fonts", fonts},
                    {"limits",
                     {{"input_bytes", engrave::max_input_bytes},
                      {"base64_chars", ((engrave::max_input_bytes + 2) / 3) * 4},
                      {"batch_ops", 1024},
                      {"query_results", engrave::max_query_results},
                      {"output_bytes", impl_->limits.output_bytes},
                      {"deadline_ms", impl_->limits.deadline.count()},
                      {"cancellation", "cooperative"},
                      {"transport_request_bytes", transport_request_limit}}}};
        json_merge(result, catalog);
        return result;
    });
}

Json HostApi::configure(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("runtime_configure", j); !ok) return std::unexpected(std::move(ok.error()));

        auto next = std::make_shared<RuntimeAssets>(*impl_->assets);
        std::shared_ptr<const AssetFile> new_font;
        if (j.contains("add_font")) {
            auto p = detail::path(j.at("add_font"));
            if (!p) return std::unexpected(std::move(p.error()));
            auto font = copy_asset(*p);
            if (!font) return std::unexpected(std::move(font.error()));
            bool exists = false;
            for (const auto &existing : impl_->assets->fonts) {
                auto same = same_asset(existing->file, (*font)->file);
                if (!same) return std::unexpected(std::move(same.error()));
                if (*same) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                new_font = std::move(*font);
                next->fonts.push_back(new_font);
            }
        }
        const bool replace_soundfonts = j.contains("soundfonts");
        std::vector<fs::path> previous_paths;
        if (replace_soundfonts) {
            next->soundfonts.clear();
            std::vector<fs::path> paths;
            for (const auto &item : j.at("soundfonts").get_array()) {
                auto p = detail::path(item);
                if (!p) return std::unexpected(std::move(p.error()));
                auto f = copy_asset(*p);
                if (!f) return std::unexpected(std::move(f.error()));
                paths.push_back((*f)->file);
                next->soundfonts.push_back(std::move(*f));
            }
            for (const auto &previous : impl_->assets->soundfonts) previous_paths.push_back(previous->file);
            auto applied = impl_->engine->set_soundfonts(paths);
            if (!applied) return std::unexpected(api_error(applied.error()));
        }
        if (new_font) {
            auto applied = impl_->engine->add_font(new_font->file);
            if (!applied) {
                if (replace_soundfonts) {
                    auto restored = impl_->engine->set_soundfonts(previous_paths);
                    if (!restored) {
                        return std::unexpected(HostError{"EngineFailure", "SoundFont rollback failed: " + restored.error().message});
                    }
                }
                return std::unexpected(api_error(applied.error()));
            }
        }
        impl_->assets = std::move(next);
        return runtime_info(make_object<Json>());
    });
}

Result<void> HostApi::with_document(std::string_view token, std::optional<u64> expected, FnRef<Result<void>(engrave::Document &)> fn) {
    const std::string key(token);
    try {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        auto doc = impl_->get(key, expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        if (!impl_->borrowed.insert(key).second) {
            return std::unexpected(HostError{"InvalidArgument", "document is already in use by with_document", {}, key});
        }

        struct Guard {
            ankerl::unordered_dense::set<std::string> &set;
            const std::string &key;

            ~Guard() { set.erase(key); }
        } guard{impl_->borrowed, key};

        return fn(**doc);
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what(), {}, key});
    } catch (...) {
        return std::unexpected(HostError{"EngineFailure", "unknown engine failure", {}, key});
    }
}

Result<RuntimeOptions> HostApi::runtime_options() const {
    try {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        RuntimeOptions result{.audio_sample_rate = impl_->options.audio_sample_rate};
        for (const auto &font : impl_->assets->fonts) result.fonts.push_back(font->file);
        for (const auto &font : impl_->assets->soundfonts) result.soundfonts.push_back({font->source, font->file});
        return result;
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what()});
    }
}

} // namespace music::api

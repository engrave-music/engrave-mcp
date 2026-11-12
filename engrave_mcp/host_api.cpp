#include "host_api.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "api_json_internal.h"

namespace music::api {

namespace {
namespace fs = std::filesystem;
using detail::invalid;

std::string nonce() {
    std::random_device rd;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (int i = 0; i < 4; ++i) {
        const auto n = static_cast<std::uint32_t>(rd());
        for (int j = 0; j < 8; ++j) result.push_back(hex[(n >> (j * 4)) & 15]);
    }
    return result;
}

struct AssetFile {
    fs::path directory, file;

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
    const auto temp = fs::temp_directory_path(ec);
    if (ec) return std::unexpected(HostError{"Io", "cannot locate runtime asset directory: " + ec.message()});
    for (int attempt = 0; attempt < 16; ++attempt) {
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

bool same_asset(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    auto a_size = fs::file_size(a, ec);
    if (ec) throw detail::DecodeError{HostError{"Io", "cannot read retained font: " + ec.message()}};
    auto b_size = fs::file_size(b, ec);
    if (ec) throw detail::DecodeError{HostError{"Io", "cannot read retained font: " + ec.message()}};
    if (a_size != b_size) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) throw detail::DecodeError{HostError{"Io", "cannot open retained font"}};
    std::array<char, 65536> x{}, y{};
    while (left && right) {
        left.read(x.data(), x.size());
        right.read(y.data(), y.size());
        if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
    }
    if (left.bad() || right.bad()) throw detail::DecodeError{HostError{"Io", "cannot compare retained fonts"}};
    return true;
}

template <class F>
Json boundary(F &&fn) {
    try {
        return fn();
    } catch (const detail::DecodeError &e) {
        return error_envelope(e.error);
    } catch (const std::exception &e) {
        return error_envelope({"EngineFailure", e.what()});
    } catch (...) {
        return error_envelope({"EngineFailure", "unknown engine failure"});
    }
}

template <class T>
T unwrap(Result<T> r) {
    if (!r) throw detail::DecodeError{std::move(r.error())};
    return std::move(*r);
}

template <class T>
T engine_result(engrave::Result<T> r) {
    if (!r) throw detail::DecodeError{api_error(r.error())};
    return std::move(*r);
}

std::optional<std::uint64_t> revision_arg(const Json &j) {
    return j.contains("expected_revision") ? std::optional(unwrap(decode_revision(j.at("expected_revision")))) : std::nullopt;
}

std::vector<std::byte> base64(const std::string &s) {
    std::size_t padding = 0;
    if (!s.empty() && s.back() == '=') ++padding;
    if (s.size() > 1 && s[s.size() - 2] == '=') ++padding;
    const auto size = s.size() / 4 * 3 - padding;
    if (size > engrave::max_input_bytes) throw detail::DecodeError{HostError{"ResourceLimit", "decoded score exceeds 8 MiB"}};
    std::vector<std::byte> bytes;
    bytes.reserve(size);
    auto digit = [](unsigned char c) -> unsigned {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : 63;
    };
    for (std::size_t i = 0; i < s.size(); i += 4) {
        unsigned x = (digit(s[i]) << 18) | (digit(s[i + 1]) << 12) | (s[i + 2] == '=' ? 0 : digit(s[i + 2]) << 6) |
                     (s[i + 3] == '=' ? 0 : digit(s[i + 3]));
        bytes.push_back(static_cast<std::byte>((x >> 16) & 255));
        if (s[i + 2] != '=') bytes.push_back(static_cast<std::byte>((x >> 8) & 255));
        if (s[i + 3] != '=') bytes.push_back(static_cast<std::byte>(x & 255));
    }
    return bytes;
}

engrave::Format input_format(const std::string &s) {
    if (s == "mscz") return engrave::Format::MSCZ;
    if (s == "mscx") return engrave::Format::MSCX;
    if (s == "musicxml") return engrave::Format::MUSIC_XML;
    if (s == "mxl") return engrave::Format::MXL;
    if (s == "midi") return engrave::Format::MIDI;
    if (s == "guitarpro") return engrave::Format::GUITAR_PRO;
    if (s == "mnx") return engrave::Format::MNX;
    invalid("unknown input format");
}

template <class T>
Json encoded_array(const std::vector<T> &values) {
    Json result = make_array<Json>();
    for (const auto &value : values) json_push(result, encode(value));
    return result;
}

std::vector<std::string> json_strings(const Json &array) {
    std::vector<std::string> result;
    for (const auto &item : array.get_array()) result.push_back(item.get<std::string>());
    return result;
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
    std::uint64_t next = 1;
    engrave::EngineOptions options;
    ExportLimits limits;
    std::shared_ptr<const RuntimeAssets> assets;
    std::unique_ptr<engrave::Engine> engine; // destroyed after documents, before assets
    std::unordered_map<std::string, std::unique_ptr<engrave::Document>> documents;
    std::unordered_set<std::string> borrowed;
    std::unordered_set<std::string> needs_recovery_check;

    void check_thread() const {
        if (std::this_thread::get_id() != owner) {
            throw detail::DecodeError{HostError{"EngineFailure", "HostApi called outside its owner thread"}};
        }
    }

    engrave::Document &get(const std::string &token, std::optional<std::uint64_t> expected) {
        auto it = documents.find(token);
        if (it == documents.end()) throw detail::DecodeError{HostError{"NotFound", "unknown or closed document", {}, token}};
        auto &doc = *it->second;
        // At this engine pin audio_tracks does not check the poison flag. Probe
        // with the public read API after a thrown edit/callback before exposing
        // the document to any subsequent call, including an export callback.
        if (needs_recovery_check.contains(token)) {
            auto health = doc.outline();
            if (!health) {
                auto error = api_error(health.error());
                error.document = token;
                throw detail::DecodeError{std::move(error)};
            }
            needs_recovery_check.erase(token);
        }
        const auto actual = doc.revision();
        if (expected && *expected != actual) {
            throw detail::DecodeError{
                HostError{"RevisionConflict", "document revision does not match expected_revision", {}, token, actual}};
        }
        return doc;
    }

    Json publish(std::unique_ptr<engrave::Document> doc) {
        if (next == 0) throw detail::DecodeError{HostError{"ResourceLimit", "document token space exhausted"}};
        auto token = "doc-" + session + "-" + std::to_string(next++);
        Json out = {{"ok", true},
                    {"document", token},
                    {"revision", std::to_string(doc->revision())},
                    {"diagnostics", encoded_array(doc->diagnostics())}};
        documents.emplace(token, std::move(doc));
        return out;
    }

    void capacity() const {
        if (documents.size() >= 16) throw detail::DecodeError{HostError{"ResourceLimit", "at most 16 documents may be open"}};
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
        for (const auto &f : options.fonts) assets->fonts.push_back(unwrap(copy_asset(f)));
        for (const auto &f : options.soundfonts) assets->soundfonts.push_back(unwrap(copy_asset(f)));
        impl->assets = std::move(assets);
        impl->options = options;
        impl->limits = limits;
        if (!options.resources.empty()) impl->options.resources = fs::absolute(options.resources);
        if (!options.cache_dir.empty()) impl->options.cache_dir = fs::absolute(options.cache_dir);
        auto engine = engrave::Engine::create(impl->asset_options());
        if (!engine) return std::unexpected(api_error(engine.error()));
        impl->engine = std::move(*engine);
        return std::unique_ptr<HostApi>(new HostApi(std::move(impl)));
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what()});
    }
}

Json HostApi::open(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_open", j);
        impl_->capacity();
        engrave::OpenOptions options;
        if (j.contains("format")) options.format = input_format(j.at("format").get<std::string>());
        if (j.contains("filename_hint")) options.filename_hint = detail::path(j.at("filename_hint"));
        if (j.contains("strict_ids")) options.strict_ids = j.at("strict_ids").get<bool>();
        auto doc = j.contains("path") ? engine_result(impl_->engine->open(detail::path(j.at("path")), options)) :
                                        engine_result(impl_->engine->open(base64(j.at("data").get<std::string>()), options));
        return impl_->publish(std::move(doc));
    });
}

Json HostApi::create_document(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_create", j);
        impl_->capacity();
        engrave::CreateOptions o;
        if (j.contains("template_id")) o.template_id = j.at("template_id").get<std::string>();
        if (j.contains("template_path")) o.template_path = detail::path(j.at("template_path"));
        o.title = json_value<std::string>(j, "title", {});
        o.subtitle = json_value<std::string>(j, "subtitle", {});
        o.composer = json_value<std::string>(j, "composer", {});
        o.lyricist = json_value<std::string>(j, "lyricist", {});
        for (const auto *value : {&o.title, &o.subtitle, &o.composer, &o.lyricist}) detail::validate_xml_text(*value);
        return impl_->publish(engine_result(impl_->engine->create_document(o)));
    });
}

Json HostApi::close(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_close", j);
        const auto token = j.at("document").get<std::string>();
        if (impl_->borrowed.contains(token)) {
            throw detail::DecodeError{HostError{"InvalidArgument", "document is in use by with_document", {}, token}};
        }
        if (!impl_->documents.erase(token)) throw detail::DecodeError{HostError{"NotFound", "unknown or closed document", {}, token}};
        impl_->needs_recovery_check.erase(token);
        return Json{{"ok", true}};
    });
}

Json HostApi::outline(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_outline", j);
        auto &doc = impl_->get(j.at("document").get<std::string>(), revision_arg(j));
        auto selection = j.contains("score") ? unwrap(decode_score(j.at("score"))) : engrave::ScoreSel{};
        auto result = doc.outline(selection);
        if (!result) {
            auto e = api_error(result.error());
            e.document = j.at("document").get<std::string>();
            throw detail::DecodeError{std::move(e)};
        }
        return Json{{"ok", true}, {"revision", std::to_string(doc.revision())}, {"outline", encode(*result)}};
    });
}

Json HostApi::query(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_query", j);
        auto &doc = impl_->get(j.at("document").get<std::string>(), revision_arg(j));
        const auto mode = j.at("mode").get<std::string>();
        Json result = {{"ok", true}, {"mode", mode}, {"revision", std::to_string(doc.revision())}};
        if (mode == "elements") {
            auto query = unwrap(decode_query(json_value<Json>(j, "query", make_object<Json>())));
            auto out = doc.query(query);
            if (!out) {
                auto e = api_error(out.error());
                e.document = j.at("document").get<std::string>();
                throw detail::DecodeError{std::move(e)};
            }
            json_merge(result, encode(*out));
        } else if (mode == "properties") {
            auto target = unwrap(decode_ref(j.at("target")));
            auto out = doc.get(target, j.contains("props") ? json_strings(j.at("props")) : std::vector<std::string>{});
            if (!out) {
                auto e = api_error(out.error());
                e.document = j.at("document").get<std::string>();
                throw detail::DecodeError{std::move(e)};
            }
            result["properties"] = encoded_array(*out);
        } else if (mode == "page_count") {
            auto selection = j.contains("score") ? unwrap(decode_score(j.at("score"))) : engrave::ScoreSel{};
            auto out = doc.page_count(selection);
            if (!out) {
                auto e = api_error(out.error());
                e.document = j.at("document").get<std::string>();
                throw detail::DecodeError{std::move(e)};
            }
            result["pages"] = *out;
        } else {
            auto selection = j.contains("score") ? unwrap(decode_score(j.at("score"))) : engrave::ScoreSel{};
            auto out = doc.audio_tracks(selection);
            if (!out) {
                auto e = api_error(out.error());
                e.document = j.at("document").get<std::string>();
                throw detail::DecodeError{std::move(e)};
            }
            result["audio_tracks"] = encode(*out);
        }
        return result;
    });
}

Json HostApi::describe(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("score_describe", j);
        auto scope = j.at("scope").get<std::string>();
        if (scope == "types") return Json{{"ok", true}, {"scope", scope}, {"types", encoded_array(impl_->engine->describe())}};
        if (scope == "type") {
            return Json{
                {"ok", true}, {"scope", scope}, {"type", encode(engine_result(impl_->engine->describe(j.at("type").get<std::string>())))}};
        }
        return Json{{"ok", true}, {"scope", scope}, {"styles", encoded_array(impl_->engine->describe_styles())}};
    });
}

Json HostApi::edit(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        // Validate the envelope separately so the batch decoder can report the
        // index of malformed operations while still decoding the whole batch
        // before looking up or mutating a document.
        auto envelope = j;
        if (envelope.is_object() && envelope.contains("ops")) envelope["ops"] = make_array<Json>();
        detail::validate_tool("score_edit", envelope);
        auto batch = unwrap(decode_batch(j.at("ops")));
        const auto expected = revision_arg(j);
        const auto token = j.at("document").get<std::string>();
        auto &doc = impl_->get(token, expected);
        auto out = [&] {
            try {
                return doc.edit(batch, expected);
            } catch (const std::exception &e) {
                impl_->needs_recovery_check.insert(token);
                throw detail::DecodeError{HostError{"EngineFailure", e.what(), {}, token}};
            } catch (...) {
                impl_->needs_recovery_check.insert(token);
                throw detail::DecodeError{HostError{"EngineFailure", "unknown engine failure", {}, token}};
            }
        }();
        if (!out) {
            auto e = api_error(out.error());
            e.document = token;
            if (e.code == "RevisionConflict") e.current_revision = doc.revision();
            throw detail::DecodeError{std::move(e)};
        }
        auto result = encode(*out);
        result["ok"] = true;
        return result;
    });
}

Json HostApi::runtime_info(const Json &j) {
    return boundary([&] {
        impl_->check_thread();
        detail::validate_tool("runtime_info", j);
        const auto action = json_value<std::string>(j, "action", "get");
        if (action == "add_font") {
            auto font = unwrap(copy_asset(detail::path(j.at("path"))));
            const bool exists = std::any_of(impl_->assets->fonts.begin(), impl_->assets->fonts.end(),
                                            [&](const auto &existing) { return same_asset(existing->file, font->file); });
            if (!exists) {
                auto next = std::make_shared<RuntimeAssets>(*impl_->assets);
                next->fonts.push_back(font);
                auto applied = impl_->engine->add_font(font->file);
                if (!applied) throw detail::DecodeError{api_error(applied.error())};
                impl_->assets = std::move(next);
            }
        } else if (action == "set_soundfonts") {
            auto next = std::make_shared<RuntimeAssets>(*impl_->assets);
            next->soundfonts.clear();
            std::vector<fs::path> paths;
            for (const auto &item : j.at("files").get_array()) {
                auto f = unwrap(copy_asset(detail::path(item)));
                paths.push_back(f->file);
                next->soundfonts.push_back(std::move(f));
            }
            auto applied = impl_->engine->set_soundfonts(paths);
            if (!applied) throw detail::DecodeError{api_error(applied.error())};
            impl_->assets = std::move(next);
        }
        auto caps = impl_->engine->capabilities();
        auto opts = impl_->asset_options();
        caps.soundfonts = opts.soundfonts;
        Json fonts = make_array<Json>();
        for (const auto &path : opts.fonts) json_push(fonts, detail::path_text(path));
        return Json{{"ok", true},
                    {"tool_revision", "2"},
                    {"capabilities", encode(caps)},
                    {"templates", encoded_array(impl_->engine->templates())},
                    {"fonts", fonts},
                    {"limits",
                     {{"input_bytes", engrave::max_input_bytes},
                      {"base64_chars", ((engrave::max_input_bytes + 2) / 3) * 4},
                      {"batch_ops", 1024},
                      {"query_results", engrave::max_query_results},
                      {"output_bytes", std::to_string(impl_->limits.output_bytes)},
                      {"deadline_ms", impl_->limits.deadline.count()},
                      {"cancellation", "cooperative"},
                      {"transport_request_bytes", transport_request_limit}}}};
    });
}

Result<void> HostApi::with_document(std::string_view token, std::optional<std::uint64_t> expected,
                                    const std::function<Result<void>(engrave::Document &)> &fn,
                                    const std::vector<std::string> *visible_paths) {
    const std::string key(token);
    bool entered = false;
    auto failure = [&](HostError error) -> Result<void> {
        if (visible_paths) error.published_artifacts = *visible_paths;
        return std::unexpected(std::move(error));
    };
    try {
        impl_->check_thread();
        auto &doc = impl_->get(key, expected);
        if (!impl_->borrowed.insert(key).second) {
            return std::unexpected(HostError{"InvalidArgument", "document is already in use by with_document", {}, key});
        }

        struct Guard {
            std::unordered_set<std::string> &set;
            const std::string &key;

            ~Guard() { set.erase(key); }
        } guard{impl_->borrowed, key};

        entered = true;
        return fn(doc);
    } catch (const detail::DecodeError &e) {
        return failure(e.error);
    } catch (const std::exception &e) {
        if (entered) impl_->needs_recovery_check.insert(key);
        return failure(HostError{"EngineFailure", e.what(), {}, key});
    } catch (...) {
        if (entered) impl_->needs_recovery_check.insert(key);
        return failure(HostError{"EngineFailure", "unknown engine failure", {}, key});
    }
}

Result<engrave::EngineOptions> HostApi::runtime_options() const {
    try {
        impl_->check_thread();
        return impl_->asset_options();
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what()});
    }
}
} // namespace music::api

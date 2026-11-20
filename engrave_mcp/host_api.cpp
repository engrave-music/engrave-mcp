#include "host_api.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>

#include <ankerl/unordered_dense.h>

#include "api_json_internal.h"

namespace music::api {

namespace {
namespace fs = std::filesystem;
using detail::invalid_error;

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
        Json out = {{"ok", true},
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
        engrave::Result<std::unique_ptr<engrave::Document>> opened = [&] -> engrave::Result<std::unique_ptr<engrave::Document>> {
            return std::unexpected(engrave::Error{engrave::ErrorCode::INVALID_ARGUMENT, ""});
        }();
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
        if (j.contains("template_id")) o.template_id = j.at("template_id").get<std::string>();
        if (j.contains("template_path")) {
            auto tp = detail::path(j.at("template_path"));
            if (!tp) return std::unexpected(std::move(tp.error()));
            o.template_path = std::move(*tp);
        }
        o.title = json_value<std::string>(j, "title", {});
        o.subtitle = json_value<std::string>(j, "subtitle", {});
        o.composer = json_value<std::string>(j, "composer", {});
        o.lyricist = json_value<std::string>(j, "lyricist", {});
        for (const auto *value : {&o.title, &o.subtitle, &o.composer, &o.lyricist}) {
            if (auto ok = detail::validate_xml_text(*value); !ok) return std::unexpected(std::move(ok.error()));
        }
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
        if (j.contains("score")) {
            auto sel = decode_score(j.at("score"));
            if (!sel) return std::unexpected(std::move(sel.error()));
            selection = *sel;
        }
        auto result = (*doc)->outline(selection);
        if (!result) {
            auto e = api_error(result.error());
            e.document = token;
            return std::unexpected(std::move(e));
        }
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
            auto out = (*doc)->query(*q);
            if (!out) {
                auto e = api_error(out.error());
                e.document = token;
                return std::unexpected(std::move(e));
            }
            json_merge(result, encode(*out));
        } else if (mode == "properties") {
            auto target = decode_ref(j.at("target"));
            if (!target) return std::unexpected(std::move(target.error()));
            auto out = (*doc)->get(*target, j.contains("props") ? json_strings(j.at("props")) : std::vector<std::string>{});
            if (!out) {
                auto e = api_error(out.error());
                e.document = token;
                return std::unexpected(std::move(e));
            }
            result["properties"] = encoded_array(*out);
        } else if (mode == "page_count") {
            engrave::ScoreSel selection{};
            if (j.contains("score")) {
                auto sel = decode_score(j.at("score"));
                if (!sel) return std::unexpected(std::move(sel.error()));
                selection = *sel;
            }
            auto out = (*doc)->page_count(selection);
            if (!out) {
                auto e = api_error(out.error());
                e.document = token;
                return std::unexpected(std::move(e));
            }
            result["pages"] = *out;
        } else {
            engrave::ScoreSel selection{};
            if (j.contains("score")) {
                auto sel = decode_score(j.at("score"));
                if (!sel) return std::unexpected(std::move(sel.error()));
                selection = *sel;
            }
            auto out = (*doc)->audio_tracks(selection);
            if (!out) {
                auto e = api_error(out.error());
                e.document = token;
                return std::unexpected(std::move(e));
            }
            result["audio_tracks"] = encode(*out);
        }
        return result;
    });
}

Json HostApi::describe(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("score_describe", j); !ok) return std::unexpected(std::move(ok.error()));
        auto scope = j.at("scope").get<std::string>();
        if (scope == "types") return Json{{"ok", true}, {"scope", scope}, {"types", encoded_array(impl_->engine->describe())}};
        if (scope == "type") {
            auto info = impl_->engine->describe(j.at("type").get<std::string>());
            if (!info) return std::unexpected(api_error(info.error()));
            return Json{{"ok", true}, {"scope", scope}, {"type", encode(*info)}};
        }
        return Json{{"ok", true}, {"scope", scope}, {"styles", encoded_array(impl_->engine->describe_styles())}};
    });
}

Json HostApi::edit(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        // Validate the envelope separately so the batch decoder can report the
        // index of malformed operations while still decoding the whole batch
        // before looking up or mutating a document.
        auto envelope = j;
        if (envelope.is_object() && envelope.contains("ops")) envelope["ops"] = make_array<Json>();
        if (auto ok = detail::validate_tool("score_edit", envelope); !ok) return std::unexpected(std::move(ok.error()));
        auto batch_res = decode_batch(j.at("ops"));
        if (!batch_res) {
            auto err = std::move(batch_res.error());
            err.applied = 0;
            return std::unexpected(std::move(err));
        }
        auto expected = revision_arg(j);
        if (!expected) return std::unexpected(std::move(expected.error()));
        const auto token = j.at("document").get<std::string>();
        auto doc = impl_->get(token, *expected);
        if (!doc) return std::unexpected(std::move(doc.error()));
        engrave::Result<engrave::EditResult> out = engrave::EditResult{};
        try {
            out = (*doc)->edit(*batch_res, *expected);
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

Json HostApi::runtime_info(const Json &j) {
    return boundary([&]() -> Result<Json> {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        if (auto ok = detail::validate_tool("runtime_info", j); !ok) return std::unexpected(std::move(ok.error()));
        const auto action = json_value<std::string>(j, "action", "get");
        if (action == "add_font") {
            auto p = detail::path(j.at("path"));
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
                auto next = std::make_shared<RuntimeAssets>(*impl_->assets);
                next->fonts.push_back(*font);
                auto applied = impl_->engine->add_font((*font)->file);
                if (!applied) return std::unexpected(api_error(applied.error()));
                impl_->assets = std::move(next);
            }
        } else if (action == "set_soundfonts") {
            auto next = std::make_shared<RuntimeAssets>(*impl_->assets);
            next->soundfonts.clear();
            std::vector<fs::path> paths;
            for (const auto &item : j.at("files").get_array()) {
                auto p = detail::path(item);
                if (!p) return std::unexpected(std::move(p.error()));
                auto f = copy_asset(*p);
                if (!f) return std::unexpected(std::move(f.error()));
                paths.push_back((*f)->file);
                next->soundfonts.push_back(std::move(*f));
            }
            auto applied = impl_->engine->set_soundfonts(paths);
            if (!applied) return std::unexpected(api_error(applied.error()));
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

Result<void> HostApi::with_document(std::string_view token, std::optional<u64> expected, FnRef<Result<void>(engrave::Document &)> fn,
                                    const std::vector<std::string> *visible_paths) {
    const std::string key(token);
    auto failure = [&](HostError error) -> Result<void> {
        if (visible_paths) error.published_artifacts = *visible_paths;
        return std::unexpected(std::move(error));
    };
    try {
        if (auto ok = impl_->check_thread(); !ok) return failure(std::move(ok.error()));
        auto doc = impl_->get(key, expected);
        if (!doc) return failure(std::move(doc.error()));
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
        return failure(HostError{"EngineFailure", e.what(), {}, key});
    } catch (...) {
        return failure(HostError{"EngineFailure", "unknown engine failure", {}, key});
    }
}

Result<engrave::EngineOptions> HostApi::runtime_options() const {
    try {
        if (auto ok = impl_->check_thread(); !ok) return std::unexpected(std::move(ok.error()));
        return impl_->asset_options();
    } catch (const std::exception &e) {
        return std::unexpected(HostError{"EngineFailure", e.what()});
    }
}

} // namespace music::api

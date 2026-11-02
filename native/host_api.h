#pragma once
#include "api_json.h"
#include "runtime_limits.h"
#include <functional>
#include <memory>
#include <thread>
namespace music::v2 {
// S2 implementation. Construct/use/destroy only on Engine's owner thread.
// Impl owns Engine first, then Documents; close all documents before Engine.
// Session nonce + monotonic counter tokens are never reused. No Document escapes
// to I/O threads; with_document is synchronous on the owner thread.
class HostApi {
public:
    static Result<std::unique_ptr<HostApi>> create(const engrave::EngineOptions&, ExportLimits = {});
    ~HostApi();
    HostApi(const HostApi&)=delete;
    HostApi& operator=(const HostApi&)=delete;
    Json open(const Json&); // Engine::open rejects native scores with mscVersion < 460
    Json create_document(const Json&);
    Json close(const Json&);
    Json outline(const Json&);
    Json query(const Json&);
    Json describe(const Json&);
    Json edit(const Json&);
    Json runtime_info(const Json&); // get, add_font(path), set_soundfonts(files)
    Result<void> with_document(std::string_view token,
        std::optional<std::uint64_t> expected_revision,
        const std::function<Result<void>(engrave::Document&)>&,
        const std::vector<std::string>* visible_paths = nullptr);
    // Current private asset paths; valid until the next owner-thread runtime change.
    Result<engrave::EngineOptions> runtime_options() const;
private:
    struct Impl;
    explicit HostApi(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}

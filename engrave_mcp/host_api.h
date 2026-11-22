#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "api_output.h"
#include "runtime_limits.h"
#include "types.h"

namespace music::api {

// S2 implementation. Construct/use/destroy only on Engine's owner thread.
// Impl owns Engine first, then Documents; close all documents before Engine.
// Session nonce + monotonic counter tokens are never reused. No Document escapes
// to I/O threads; with_document is synchronous on the owner thread.
struct HostApi {
    static Result<std::unique_ptr<HostApi>> create(const engrave::EngineOptions &, ExportLimits = {});
    ~HostApi();
    HostApi(const HostApi &) = delete;
    HostApi &operator=(const HostApi &) = delete;

    Json open(const Json &); // Engine::open rejects native scores with mscVersion < 460
    Json create_document(const Json &);
    Json close(const Json &);

    Json outline(const Json &);
    Json query(const Json &);
    Json read_music(const Json &);
    Json write_phrase(const Json &, const std::atomic<bool> *cancelled = nullptr);
    Json configure(const Json &);
    Json edit(const Json &, const std::atomic<bool> *cancelled = nullptr);
    Json checkpoint(const Json &);
    Json restore_checkpoint(const Json &);
    Json drop_checkpoint(const Json &);
    Json runtime_info(const Json &);

    Result<void> with_document(std::string_view token, std::optional<u64> expected_revision, FnRef<Result<void>(engrave::Document &)> fn,
                               const std::vector<std::string> *visible_paths = nullptr);

    // Current private asset paths; valid until the next owner-thread runtime change.
    Result<RuntimeOptions> runtime_options() const;

private:
    struct Impl;
    explicit HostApi(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace music::api

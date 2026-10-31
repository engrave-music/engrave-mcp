#pragma once

#include <engrave/engine.h>
#include <memory>

namespace music::v2 {

// S2 owns immutable copies of runtime fonts and SoundFonts and supplies the
// same bytes to Engine and workers. The lease keeps those files alive while
// captured jobs exist, including after a later runtime asset change.
struct RuntimeAssetLease;
struct RuntimeSnapshot {
    engrave::EngineOptions options;
    std::shared_ptr<const RuntimeAssetLease> asset_lease;
};

} // namespace music::v2

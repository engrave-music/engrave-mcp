#pragma once

#include <vector>

#include "global/iapplication.h"

namespace music {
// Qt-free IApplication for the headless host. Version data comes from the
// generated framework config, mirroring BaseApplication; the IoC context is the
// same process-wide one the runtime hands to EngravingProject.
class NoQtApplication final : public muse::IApplication
{
public:
    NoQtApplication() = default;
    ~NoQtApplication() override = default;

    muse::String name() const override;
    muse::String title() const override;

    bool unstable() const override;
    muse::Version version() const override;
    muse::Version fullVersion() const override;
    muse::String build() const override;
    muse::String revision() const override;

    RunMode runMode() const override;
    bool noGui() const override;

    void setup() override;
    void finish() override;
    void restart() override;

    muse::modularity::ContextPtr setupNewContext() override;
    int contextCount() const override;
    std::vector<muse::modularity::ContextPtr> contexts() const override;

    const muse::modularity::ContextPtr iocContext() const override;
    muse::modularity::ModulesIoC* ioc() const override;

    void processEvents() override;
};
}

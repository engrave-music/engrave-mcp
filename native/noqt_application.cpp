#include "noqt_application.h"

#include "muse_framework_config.h"

using namespace muse;

namespace music {
String NoQtApplication::name() const
{
#ifdef MUSE_APP_NAME
    return String::fromAscii(MUSE_APP_NAME);
#else
    return String();
#endif
}

String NoQtApplication::title() const
{
#ifdef MUSE_APP_TITLE
#ifdef MUSE_APP_UNSTABLE
    return String::fromAscii(MUSE_APP_TITLE) + u" Development";
#else
    return String::fromAscii(MUSE_APP_TITLE);
#endif
#else
    return String();
#endif
}

bool NoQtApplication::unstable() const
{
#ifdef MUSE_APP_UNSTABLE
    return true;
#else
    return false;
#endif
}

Version NoQtApplication::version() const
{
#ifdef MUSE_APP_VERSION
    static const Version v(MUSE_APP_VERSION);
#else
    static const Version v("0.0.0");
#endif
    return v;
}

Version NoQtApplication::fullVersion() const
{
    // Initialized once (thread-safe static); export workers may call this concurrently.
    static const Version fv = [this] {
        Version result(version());
#ifdef MUSE_APP_VERSION_LABEL
        const String label = String::fromAscii(MUSE_APP_VERSION_LABEL);
        if (!label.isEmpty()) {
            result.setSuffix(label);
        }
#endif
        return result;
    }();
    return fv;
}

String NoQtApplication::build() const
{
#ifdef MUSE_APP_BUILD_NUMBER
    return String::fromAscii(MUSE_APP_BUILD_NUMBER);
#else
    return String();
#endif
}

String NoQtApplication::revision() const
{
#ifdef MUSE_APP_REVISION
    return String::fromAscii(MUSE_APP_REVISION);
#else
    return String();
#endif
}

IApplication::RunMode NoQtApplication::runMode() const
{
    return RunMode::ConsoleApp;
}

bool NoQtApplication::noGui() const
{
    return true;
}

void NoQtApplication::setup()
{
}

void NoQtApplication::finish()
{
}

void NoQtApplication::restart()
{
}

modularity::ContextPtr NoQtApplication::setupNewContext()
{
    return nullptr;
}

int NoQtApplication::contextCount() const
{
    return 0;
}

std::vector<modularity::ContextPtr> NoQtApplication::contexts() const
{
    return {};
}

const modularity::ContextPtr NoQtApplication::iocContext() const
{
    // The runtime creates projects with globalCtx(); keep a single context.
    return modularity::globalCtx();
}

modularity::ModulesIoC* NoQtApplication::ioc() const
{
    return modularity::ioc(iocContext());
}

void NoQtApplication::processEvents()
{
}
}

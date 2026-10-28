// Build-time link probe for the Qt-free MuseScore audio engine and its
// encoders. It is not the export driver: it only proves that the static
// libraries resolve and that EngineController can register its services
// against the shared no-op RPC channel (the real driver is native/engine_audio.cpp).

#include <iostream>
#include <memory>

#include "global/modularity/ioc.h"

#include "engine_audio.h"

#include "audio/common/rpc/irpcchannel.h"
#include "audio/engine/internal/enginecontroller.h"

int main()
{
    const auto rpc = std::make_shared<music::NullRpcChannel>();
    muse::audio::engine::EngineController controller(rpc, muse::modularity::globalCtx());
    controller.registerExports();
    std::cout << "audio engine linked\n";
    return 0;
}

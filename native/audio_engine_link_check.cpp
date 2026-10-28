// Build-time link probe for the Qt-free MuseScore audio engine and its
// encoders. It is not the export driver: it only proves that the static
// libraries resolve and that EngineController can register its services
// against a no-op RPC channel (the real driver is a separate worker).

#include <iostream>
#include <memory>

#include "global/modularity/ioc.h"

#include "audio/common/rpc/irpcchannel.h"
#include "audio/engine/internal/enginecontroller.h"

namespace {
class NullRpcChannel final : public muse::audio::rpc::IRpcChannel
{
public:
    void setupOnMain() override {}
    void setupOnEngine() override {}

    void process() override {}

    void send(const muse::audio::rpc::Msg&, const muse::audio::rpc::Handler&) override {}
    void onMethod(muse::audio::rpc::Method, muse::audio::rpc::Handler) override {}
    void listenAll(muse::audio::rpc::Handler) override {}

    void addStream(std::shared_ptr<muse::audio::rpc::IRpcStream>) override {}
    void removeStream(muse::audio::rpc::StreamId) override {}
    void sendStream(const muse::audio::rpc::StreamMsg&) override {}
    void onStream(muse::audio::rpc::StreamId, muse::audio::rpc::StreamHandler) override {}
};
}

int main()
{
    const auto rpc = std::make_shared<NullRpcChannel>();
    muse::audio::engine::EngineController controller(rpc, muse::modularity::globalCtx());
    controller.registerExports();
    std::cout << "audio engine linked\n";
    return 0;
}

#include "noqt_filesystem.h"
#include "settings.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void expect(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}



#include "global/modularity/ioc.h"
#include "native/tests/ioc_tu2.h"

#ifdef MODULENAME
#undef MODULENAME
#endif
#define MODULENAME "ioc_contract_test"

namespace {

class IRegisteredGlobalService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IRegisteredGlobalService)
public:
    virtual ~IRegisteredGlobalService() = default;
    virtual int globalValue() const = 0;
};

class RegisteredGlobalServiceImpl : public IRegisteredGlobalService
{
public:
    int globalValue() const override { return 42; }
};

class IRegisteredContextService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IRegisteredContextService)
public:
    virtual ~IRegisteredContextService() = default;
    virtual int contextValue() const = 0;
};

class RegisteredContextServiceImpl : public IRegisteredContextService
{
public:
    int contextValue() const override { return 100; }
};

class IUnregisteredGlobalService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IUnregisteredGlobalService)
public:
    virtual ~IUnregisteredGlobalService() = default;
    virtual int dummy() const = 0;
};

class IUnregisteredContextService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IUnregisteredContextService)
public:
    virtual ~IUnregisteredContextService() = default;
    virtual int dummy() const = 0;
};

/*
 * =========================================================================
 * Kors Modularity (Qt-free IoC) Contract Documentation
 * =========================================================================
 * Upstream kors_modularity was adapted for NO_QT_SUPPORT headless operation
 * via commits f88394cf5d and ef8805ea94. This test pins the following contract:
 *
 * 1. globalCtx Singleton & Multi-TU Consistency (f88394cf5d):
 *    Under NO_QT_SUPPORT, globalContextInstance() is an inline function returning
 *    a function-local static std::shared_ptr<Context>(globalId = 0).
 *    By ISO C++ [dcl.inline], function-local statics in inline functions are
 *    guaranteed to refer to the exact same object across all translation units.
 *    globalCtx is a reference to this singleton. It is non-null, has ID 0, and
 *    is identical (both by pointer value and reference identity) across all TUs.
 *
 * 2. Context ID 0 Registry and globalIoc Mapping (ef8805ea94):
 *    Under NO_QT_SUPPORT typed IoC (IOC_CHECK_INTERFACE_TYPE), context ID 0
 *    is permitted: ioc(ctx) with ctx->id == 0 (including globalCtx and any
 *    user-created Context(0)) returns the shared context-0 ModulesContextIoC
 *    registry. Any context with id == 0 resolves the same service instance.
 *    removeIoC() refuses to delete context 0 (guard: ctx->id > 0).
 *
 * 3. Safe Null Resolution on Missing Registrations (ef8805ea94):
 *    Under NO_QT_SUPPORT, InjectBase::doResolve guards against a null IoC
 *    pointer and returns silently. If an interface is not registered,
 *    ioc->resolve<I>() returns nullptr and InjectBase::m_i remains nullptr
 *    without asserting or crashing.
 *    Caller-visible behavior: Callers must check if the injection resolved
 *    (e.g., if (!inject()) { ... }) and detect missing dependencies to log
 *    errors or fail clearly for required services.
 *
 * 4. Context Isolation and Removal:
 *    Contexts with distinct IDs (e.g., id 1 vs id 2) have independent IoC
 *    registries. removeIoC(ctx) properly removes non-zero context registries.
 *
 * 5. Registered Exports Resolve:
 *    Global exports registered via globalIoc()->registerExport<I>(module, impl)
 *    resolve properly via GlobalInject. Contextual exports registered via
 *    ioc(ctx)->registerExport<I>(module, impl) resolve properly via
 *    ContextInject for the corresponding context.
 * =========================================================================
 */
void test_ioc_contract()
{
    using namespace kors::modularity;

    // 1. globalCtx is non-null and identical across TUs
    expect(globalCtx != nullptr, "globalCtx is non-null");
    expect(globalCtx->id == 0, "globalCtx id is 0 (globalId)");
    expect(globalContextInstance() != nullptr, "globalContextInstance() is non-null");
    expect(&globalCtx == &globalContextInstance(), "globalCtx references globalContextInstance()");

    // Cross-TU verification against ioc_tu2.cpp
    const ContextPtr& tu2Ctx = ioc_test::getTu2GlobalCtx();
    const ContextPtr& tu2Inst = ioc_test::getTu2GlobalContextInstance();
    expect(tu2Ctx != nullptr, "TU2 globalCtx is non-null");
    expect(tu2Ctx->id == 0, "TU2 globalCtx id is 0");
    expect(globalCtx.get() == tu2Ctx.get(), "globalCtx instance pointer identical across TUs");
    expect(&globalCtx == &tu2Ctx, "globalCtx reference address identical across TUs");
    expect(&globalContextInstance() == &tu2Inst, "globalContextInstance() address identical across TUs");

    // 2. Context ID 0 and ioc mapping
    ModulesGlobalIoC* gIoc = globalIoc();
    expect(gIoc != nullptr, "globalIoc() is non-null");

    ModulesContextIoC* c0Ioc = ioc(globalCtx);
    expect(c0Ioc != nullptr, "ioc(globalCtx) is non-null");

    auto customCtx0 = std::make_shared<Context>(0);
    expect(ioc(customCtx0) == c0Ioc, "ioc(custom context with id 0) returns the same context-0 instance");

    // Null context returns nullptr under NO_QT_SUPPORT
    expect(ioc(ContextPtr{}) == nullptr, "ioc(null ContextPtr) returns nullptr under NO_QT_SUPPORT");

    // removeIoC refuses to remove context 0 or null
    removeIoC(globalCtx);
    expect(ioc(globalCtx) == c0Ioc, "removeIoC(globalCtx) refuses id 0");
    removeIoC(customCtx0);
    expect(ioc(customCtx0) == c0Ioc, "removeIoC(customCtx0) refuses id 0");
    removeIoC(ContextPtr{});
    expect(ioc(globalCtx) == c0Ioc, "removeIoC(null) safely ignored");

    // Context isolation for non-zero IDs
    auto ctx1 = std::make_shared<Context>(1);
    auto ctx2 = std::make_shared<Context>(2);
    ModulesContextIoC* ioc1 = ioc(ctx1);
    ModulesContextIoC* ioc2 = ioc(ctx2);
    expect(ioc1 != nullptr && ioc2 != nullptr, "non-zero context IoCs are non-null");
    expect(ioc1 != ioc2, "context 1 and context 2 are isolated instances");
    expect(ioc1 != c0Ioc && ioc2 != c0Ioc, "non-zero contexts are isolated from context 0");
    removeIoC(ctx1);
    removeIoC(ctx2);

    // 3. Registered exports resolve
    // 3a. Global export
    auto globalImpl = std::make_shared<RegisteredGlobalServiceImpl>();
    gIoc->registerExport<IRegisteredGlobalService>("contract_test", globalImpl);

    GlobalInject<IRegisteredGlobalService> globalInj;
    expect(globalInj() == globalImpl, "GlobalInject resolves registered export");
    expect(globalInj()->globalValue() == 42, "Global export is functional");
    expect(gIoc->resolve<IRegisteredGlobalService>("contract_test") == globalImpl,
           "globalIoc()->resolve returns registered export");

    // 3b. Context export (context 0)
    auto contextImpl = std::make_shared<RegisteredContextServiceImpl>();
    c0Ioc->registerExport<IRegisteredContextService>("contract_test", contextImpl);

    ContextInject<IRegisteredContextService> ctxInj(globalCtx);
    expect(ctxInj() == contextImpl, "ContextInject(globalCtx) resolves registered export");
    expect(ctxInj()->contextValue() == 100, "Context export is functional");

    ContextInject<IRegisteredContextService> ctx0Inj(customCtx0);
    expect(ctx0Inj() == contextImpl, "ContextInject(customCtx0) resolves same instance as globalCtx");

    // 4. An injection whose interface is NOT registered yields null
    //    Caller-visible behavior: doResolve returns silently without throwing or asserting,
    //    leaving m_i == nullptr. The caller must detect missing registrations and handle/log.
    GlobalInject<IUnregisteredGlobalService> unregGlobal;
    expect(unregGlobal() == nullptr, "unregistered GlobalInject yields nullptr");
    expect(unregGlobal.get() == nullptr, "unregistered GlobalInject get() is nullptr");

    // Caller can detect missing registration (e.g. to log error or fail cleanly)
    bool detectedMissingGlobal = false;
    if (!unregGlobal()) {
        detectedMissingGlobal = true; // Caller detected missing service
    }
    expect(detectedMissingGlobal, "missing global service is detectable by caller");

    ContextInject<IUnregisteredContextService> unregCtx(globalCtx);
    expect(unregCtx() == nullptr, "unregistered ContextInject(globalCtx) yields nullptr");

    // Null context injection: doResolve guards if (!ioc) return; without crashing
    ContextInject<IUnregisteredContextService> unregNull(ContextPtr{});
    expect(unregNull() == nullptr, "unregistered ContextInject(nullptr) yields nullptr safely");

    // Clean up registered exports
    gIoc->unregister<IRegisteredGlobalService>("contract_test");
    c0Ioc->unregister<IRegisteredContextService>("contract_test");
    resetAll();

    expect(gIoc->resolve<IRegisteredGlobalService>("contract_test") == nullptr,
           "unregistered global export resolves to nullptr");
    expect(c0Ioc->resolve<IRegisteredContextService>("contract_test") == nullptr,
           "unregistered context export resolves to nullptr");

    std::cout << "IoC modularity contract checks passed\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected a fresh test directory");
        const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        expect(!std::filesystem::exists(root), "Test directory already exists");
        std::filesystem::create_directories(root / "assets");
        std::ofstream(root / "assets" / "test.txt", std::ios::binary) << "resource";
        music::NativeFileSystem fs(root / "assets");
        auto asPath = [](const std::filesystem::path& p) {
            auto value = p.u8string();
            return muse::io::path_t(std::string(value.begin(), value.end()));
        };
        expect(bool(fs.readFile(":/test.txt").ret), "Resource read");
        expect(bool(fs.readFile("://test.txt").ret), "Resource read with a doubled slash");
        expect(!fs.readFile("://../outside.txt").ret, "Doubled-slash traversal rejected");
        expect(!fs.readFile(":/../outside.txt").ret, "Resource traversal rejected");
        expect(!fs.writeFile(":/test.txt", muse::ByteArray("bad", 3)), "Resource write rejected");
        expect(!fs.writeFile(asPath(root / "assets/test.txt"), muse::ByteArray("bad", 3)), "Absolute resource write rejected");
        const auto file = asPath(root / std::filesystem::u8path("测试.bin"));
        expect(bool(fs.writeFile(file, muse::ByteArray("abcdef", 6))), "Unicode write");
        auto data = fs.readFile(file);
        expect(bool(data.ret) && data.val == muse::ByteArray("abcdef", 6), "Read/write content");
        auto stream = fs.openStream(file, muse::io::OpenMode::WriteOnly);
        expect(bool(stream.ret), "Open stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("abcd", 4))), "Write stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("XY", 2), 1)), "Seek overwrite");
        expect(bool(fs.closeStream(stream.val)), "Close stream");
        expect(fs.readFile(file).val == muse::ByteArray("aXYd", 4), "Stream content");
        expect(!fs.closeStream(stream.val), "Stale stream rejected");
        const auto scan = fs.scanFiles(asPath(root), {"*.bin"}, muse::io::ScanMode::FilesInCurrentDir);
        expect(bool(scan.ret) && scan.val.size() == 1, "Scan filter");
        auto* settings = muse::Settings::instance();
        const muse::Settings::Key key("test", "value"), added("test", "added");
        settings->setDefaultValue(key, muse::Val(1));
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(2));
        settings->setDefaultValue(added, muse::Val(3));
        settings->setDescription(added, "new default");
        settings->commitTransaction();
        expect(settings->value(key).toInt() == 2 && settings->defaultValue(added).toInt() == 3,
               "Commit preserves new default metadata");
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(4));
        settings->rollbackTransaction();
        expect(settings->value(key).toInt() == 2, "Rollback");
        settings->reset();
        expect(settings->value(key).toInt() == 1, "Reset defaults");
        test_ioc_contract();
        std::cout << "native filesystem/settings checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

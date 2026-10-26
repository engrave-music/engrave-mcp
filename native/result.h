#pragma once

#include <cstdint>
#include <utility>
#include <QByteArray>
#include "global/types/bytearray.h"
#include "global/types/ret.h"
#include "global/types/string.h"

// Own the payload; do not expose the WASM malloc/header protocol to the host.
struct NativeResult {
    QByteArray bytes;
    muse::Ret status = muse::make_ok();
    uintptr_t score = 0;

    NativeResult() = default;
    NativeResult(muse::ByteArray data, muse::Ret ret = muse::make_ok())
        : bytes(reinterpret_cast<const char*>(data.constData()), static_cast<qsizetype>(data.size())), status(ret) {}
    NativeResult(QByteArray data) : bytes(std::move(data)) {}
    NativeResult(muse::String text) : NativeResult(text.toUtf8()) {}
    explicit NativeResult(uint32_t number)
        : bytes(reinterpret_cast<const char*>(&number), sizeof(number)) {}

    static NativeResult fromPointer(uintptr_t pointer) {
        NativeResult result;
        result.score = pointer;
        return result;
    }

    static NativeResult fromRet(const muse::Ret& ret) {
        if (!ret.success()) {
            return NativeResult(muse::String::fromStdString(ret.toString()).toUtf8(), ret);
        }
        return NativeResult(ret.data<muse::ByteArray>("wasm_payload", muse::ByteArray()), ret);
    }
};

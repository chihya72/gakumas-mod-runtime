#include "ModRuntimeApi.h"
#include "ModRuntimeCatalog.hpp"

#include <cstring>

namespace {
    GmrResult GMR_CALL GetModsJsonThunk(GmrOwnedBuffer* output) {
        return GakumasMod::Runtime::Catalog::GetModsJson(output);
    }

    void GMR_CALL FreeBufferThunk(void* data) {
        GakumasMod::Runtime::Catalog::FreeBuffer(data);
    }

    GmrResult GMR_CALL SetModEnabledThunk(const char* modIdUtf8, uint8_t enabled) {
        return GakumasMod::Runtime::Catalog::SetModEnabled(modIdUtf8, enabled);
    }

    void GMR_CALL WriteLogThunk(uint32_t level, const char* component, const char* messageUtf8) {
        GakumasMod::Runtime::Catalog::WriteLog(level, component, messageUtf8);
    }
}
extern "C" GmrResult GMR_CALL GmrGetRuntimeApiV1(GmrRuntimeApiV1* output) {
    if (!output || output->structSize < sizeof(uint32_t) * 2) {
        return GMR_E_INVALID_ARGUMENT;
    }

    if (output->apiVersion != 0 && output->apiVersion != GMR_API_VERSION_1) {
        return GMR_E_API_VERSION;
    }

    const GmrRuntimeApiV1 api{
        sizeof(GmrRuntimeApiV1),
        GMR_API_VERSION_1,
        &GetModsJsonThunk,
        &FreeBufferThunk,
        &SetModEnabledThunk,
        &WriteLogThunk,
    };

    const auto copySize = output->structSize < sizeof(api) ? output->structSize : sizeof(api);
    std::memcpy(output, &api, copySize);
    return GMR_OK;
}

#pragma once

#include "ModRuntimeApi.h"

namespace GakumasMod::Runtime::Catalog {
    void Refresh();
    void Clear();
    void SetReady(bool ready);
    bool IsReady();

    GmrResult GetModsJson(GmrOwnedBuffer* output);
    void FreeBuffer(void* data);
    GmrResult SetModEnabled(const char* modIdUtf8, uint8_t enabled);
    void WriteLog(uint32_t level, const char* component, const char* messageUtf8);
}

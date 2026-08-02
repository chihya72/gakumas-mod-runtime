#pragma once

#include "ModRuntimeApi.h"

namespace GakumasMod::Runtime {
    bool Initialize();
    void Shutdown();
    GmrResult SetSessionModEnabled(const char* modIdUtf8, uint8_t enabled);
}

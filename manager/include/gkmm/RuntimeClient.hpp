#pragma once

// The runtime owns this ABI; include its copy directly so a change to the
// struct cannot compile against a stale duplicate.
#include "ModRuntimeApi.h"

#include <string>

namespace GakumasModManager {
    class RuntimeClient {
    public:
        bool Connect();
        bool IsReady() const;
        bool GetModsJson(std::string& output) const;
        GmrResult SetModEnabled(const std::string& modId, bool enabled) const;
        const GmrRuntimeApiV1* Api() const;

    private:
        GmrRuntimeApiV1 api_{};
        bool connected_{false};
    };
}

#pragma once

#include "gmr_runtime_api.h"

#include <string>

namespace GakumasModManager {
    class RuntimeClient {
    public:
        bool Connect();
        bool IsReady() const;
        bool GetModsJson(std::string& output) const;
        const GmrRuntimeApiV1* Api() const;

    private:
        GmrRuntimeApiV1 api_{};
        bool connected_{false};
    };
}

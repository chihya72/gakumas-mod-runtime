#pragma once

#include "gmr_runtime_api.h"

namespace GakumasModManager {
    class RuntimeClient {
    public:
        bool Connect();
        bool IsReady() const;
        const GmrRuntimeApiV1* Api() const;

    private:
        GmrRuntimeApiV1 api_{};
        bool connected_{false};
    };
}

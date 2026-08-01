#include "gkmm/RuntimeClient.hpp"

#include <Windows.h>

namespace GakumasModManager {
    bool RuntimeClient::Connect() {
        constexpr const wchar_t* kRuntimeModules[] = {
            L"xinput1_3.dll",
            L"xinput3.dll",
        };

        for (const auto moduleName : kRuntimeModules) {
            const auto module = GetModuleHandleW(moduleName);
            if (!module) continue;
            const auto getApi = reinterpret_cast<GmrGetRuntimeApiV1Fn>(
                GetProcAddress(module, "GmrGetRuntimeApiV1"));
            if (!getApi) continue;

            GmrRuntimeApiV1 candidate{};
            candidate.structSize = sizeof(candidate);
            candidate.apiVersion = GMR_API_VERSION_1;
            if (getApi(&candidate) != GMR_OK
                || candidate.structSize < sizeof(candidate)
                || !candidate.getModsJson
                || !candidate.freeBuffer
                || !candidate.setModEnabled) {
                continue;
            }
            api_ = candidate;
            connected_ = true;
            return true;
        }
        connected_ = false;
        api_ = {};
        return false;
    }

    bool RuntimeClient::IsReady() const {
        if (!connected_ || !api_.getModsJson || !api_.freeBuffer) return false;
        GmrOwnedBuffer buffer{};
        const auto result = api_.getModsJson(&buffer);
        if (buffer.data) api_.freeBuffer(buffer.data);
        return result == GMR_OK;
    }

    bool RuntimeClient::GetModsJson(std::string& output) const {
        output.clear();
        if (!connected_ || !api_.getModsJson || !api_.freeBuffer) return false;

        GmrOwnedBuffer buffer{};
        const auto result = api_.getModsJson(&buffer);
        if (result != GMR_OK) {
            if (buffer.data) api_.freeBuffer(buffer.data);
            return false;
        }
        if (!buffer.data && buffer.size != 0) return false;

        if (buffer.data && buffer.size != 0) {
            output.assign(static_cast<const char*>(buffer.data), buffer.size);
        }
        if (buffer.data) api_.freeBuffer(buffer.data);
        return true;
    }

    const GmrRuntimeApiV1* RuntimeClient::Api() const {
        return connected_ ? &api_ : nullptr;
    }
}

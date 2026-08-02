#include "gkmm/RuntimeClient.hpp"

#include <Windows.h>

namespace GakumasModManager {
    // The runtime now hosts this code, so the API is a direct call rather than
    // a GetModuleHandle/GetProcAddress lookup.  The struct is still filled
    // through GmrGetRuntimeApiV1 and still checked: it stays the single seam
    // between the UI and the runtime, and keeps the export usable by others.
    bool RuntimeClient::Connect() {
        GmrRuntimeApiV1 candidate{};
        candidate.structSize = sizeof(candidate);
        candidate.apiVersion = GMR_API_VERSION_1;
        if (GmrGetRuntimeApiV1(&candidate) != GMR_OK
            || candidate.structSize < sizeof(candidate)
            || !candidate.getModsJson
            || !candidate.freeBuffer
            || !candidate.setModEnabled) {
            connected_ = false;
            api_ = {};
            return false;
        }
        api_ = candidate;
        connected_ = true;
        return true;
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

    GmrResult RuntimeClient::SetModEnabled(const std::string& modId, const bool enabled) const {
        if (!connected_ || !api_.setModEnabled || modId.empty()) {
            return GMR_E_INVALID_ARGUMENT;
        }
        return api_.setModEnabled(modId.c_str(), enabled ? 1 : 0);
    }

    const GmrRuntimeApiV1* RuntimeClient::Api() const {
        return connected_ ? &api_ : nullptr;
    }
}

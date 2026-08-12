#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace GakumasMod::Runtime::Detail {
    struct ReapplyRendererIdentity {
        std::string sourceName;
        void* originalMesh{};
        int sourceRootDepth{};
        std::string rendererName;
    };

    // AssetBundle can materialize a new Mesh wrapper for the same renderer after
    // a scene transition.  Keep a few recent identities instead of letting the
    // first pointer seen for that renderer permanently shadow every later load.
    inline constexpr std::size_t MaxRecentIdentitiesPerRenderer = 4;

    inline bool RememberReapplyRendererIdentity(
        std::vector<ReapplyRendererIdentity>& identities,
        const ReapplyRendererIdentity& identity) {
        const auto exact = std::find_if(
            identities.begin(), identities.end(), [&identity](const auto& existing) {
                return existing.sourceName == identity.sourceName
                    && existing.originalMesh == identity.originalMesh
                    && existing.sourceRootDepth == identity.sourceRootDepth
                    && existing.rendererName == identity.rendererName;
            });
        if (exact != identities.end()) return false;

        const auto sameRenderer = [&identity](const auto& existing) {
            return existing.sourceName == identity.sourceName
                && existing.sourceRootDepth == identity.sourceRootDepth
                && existing.rendererName == identity.rendererName;
        };
        const auto retained = static_cast<std::size_t>(std::count_if(
            identities.begin(), identities.end(), sameRenderer));
        if (retained >= MaxRecentIdentitiesPerRenderer) {
            if (const auto oldest = std::find_if(
                    identities.begin(), identities.end(), sameRenderer);
                oldest != identities.end()) {
                identities.erase(oldest);
            }
        }
        identities.push_back(identity);
        return true;
    }

    struct PendingReapplyRequest {
        std::string modId;
        std::string sourceKey;
    };

    inline bool QueuePendingReapply(
        std::vector<PendingReapplyRequest>& requests,
        const std::string& modId,
        const std::string& sourceKey) {
        const auto duplicate = std::find_if(
            requests.begin(), requests.end(), [&](const auto& request) {
                return request.modId == modId && request.sourceKey == sourceKey;
            });
        if (duplicate != requests.end()) return false;
        requests.push_back({modId, sourceKey});
        return true;
    }

    inline bool ClearPendingReapply(
        std::vector<PendingReapplyRequest>& requests,
        const std::string& modId,
        const std::string& sourceKey) {
        const auto oldSize = requests.size();
        std::erase_if(requests, [&](const auto& request) {
            return request.modId == modId && request.sourceKey == sourceKey;
        });
        return requests.size() != oldSize;
    }

    inline std::size_t ClearPendingReappliesForMod(
        std::vector<PendingReapplyRequest>& requests,
        const std::string& modId) {
        const auto oldSize = requests.size();
        std::erase_if(requests, [&](const auto& request) {
            return request.modId == modId;
        });
        return oldSize - requests.size();
    }
}

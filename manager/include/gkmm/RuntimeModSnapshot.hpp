#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace GakumasModManager {
    enum class RuntimeTargetKind {
        Costume,
        Hair,
        Unknown,
    };

    struct RuntimeModTarget {
        RuntimeTargetKind kind{RuntimeTargetKind::Unknown};
        std::string source;
        std::string masterKey;
    };

    struct RuntimeModConflict {
        std::string modId;
        std::string modName;
    };

    struct RuntimeModRecord {
        std::string id;
        std::string name;
        bool configuredEnabled{};
        bool registeredThisSession{};
        bool appliedThisSession{};
        bool restartRequired{};
        std::string manifestState;
        std::string runtimeState;
        RuntimeModTarget target;
        RuntimeModConflict conflict;
    };

    struct RuntimeModSnapshot {
        std::vector<RuntimeModRecord> mods;
        std::size_t malformedRecordCount{};
    };

    bool ParseRuntimeModSnapshot(
        std::string_view jsonText,
        RuntimeModSnapshot& output,
        std::string& errorMessage);
}

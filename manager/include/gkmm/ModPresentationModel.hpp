#pragma once

#include "RuntimeModSnapshot.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace GakumasModManager {
    enum class ModCategory {
        Costume,
        Hair,
    };

    enum class ModDisplayState {
        Enabled,
        Disabled,
        RestartToEnable,
        RestartToDisable,
        Conflict,
        EnableBlockedByConflict,
        ConfigurationError,
    };

    struct ModPresentationItem {
        std::string modId;
        std::string title;
        // Runtime-facing identifiers are carried to the game-data resolver but
        // are never rendered directly in the user-facing row.
        std::string targetSource;
        std::string targetMasterKey;
        ModCategory category{ModCategory::Costume};
        ModDisplayState state{ModDisplayState::Disabled};
        bool configuredEnabled{};
        std::string statusText;
    };

    struct ModPresentationModel {
        std::vector<ModPresentationItem> costumeMods;
        std::vector<ModPresentationItem> hairMods;
        std::size_t totalModCount{};
        std::size_t invalidModCount{};
        bool restartRequired{};
    };

    ModPresentationModel BuildModPresentationModel(const RuntimeModSnapshot& snapshot);
    std::string NormalizeHairAssetKey(std::string_view value);
    std::string RenderProbeText(const ModPresentationModel& model);
}

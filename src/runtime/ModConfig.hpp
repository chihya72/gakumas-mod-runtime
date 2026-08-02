#pragma once

#include <filesystem>
#include <optional>

namespace GakumasMod::Config {
    // Reads <game root>/gakumas-mod/config.json.
    //
    // Returns nullopt when the file is absent, unparseable, or carries no
    // usable "modManagerUi" -- every one of those means "no answer", and the
    // caller defaults to showing the UI.  A typo in this file must never
    // silently hide the manager.
    //
    // No logging and no game dependencies: this stays linkable by the offline
    // tests, which is the only place the four outcomes get exercised.
    std::optional<bool> ReadManagerUiEnabled(const std::filesystem::path& configPath);
}

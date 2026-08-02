#pragma once

#include <filesystem>

// Everything this plugin reads or writes lives under <game root>/gakumas-mod.
// It used to sit in gakumas-local/, which belongs to the localisation plugin:
// two unrelated tools sharing one folder made it impossible to tell whose
// state was whose, and uninstalling either one took the other's data with it.
//
// The runtime chdirs to the game root at startup (main.cpp), so the relative
// paths below resolve there.  Callers that build an absolute path from the
// executable's directory use kRootName instead.
namespace GakumasMod::Paths {
    inline constexpr const char* kRootName = "gakumas-mod";

    inline std::filesystem::path Root() {
        return std::filesystem::path(".") / kRootName;
    }

    inline std::filesystem::path Mods() { return Root() / "mods"; }
    inline std::filesystem::path Profiles() { return Root() / "profiles"; }
    inline std::filesystem::path RuntimeLog() { return Root() / "mod-plugin.log"; }
    inline constexpr const char* kManagerLogName = "mod-manager.log";
}

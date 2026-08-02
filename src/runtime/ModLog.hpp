#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace GakumasMod::Log {
    enum class Level { Info = 0, Warn = 1, Error = 2 };

    // Defaults to Error: a shipped plugin should not write a 500 KB trace on
    // every launch.  config.json's "logLevel" raises it; see ModConfig.hpp.
    void SetMinLevel(Level level);
    Level MinLevel();
    bool IsEnabled(Level level);

    // "info" / "warn" / "error", case-insensitive.  nullopt for anything else.
    std::optional<Level> ParseLevel(std::string_view name);

    // Always written, whatever the level.  Without it an error-free launch
    // leaves no file at all, and "no log" is indistinguishable from "the
    // plugin never loaded" -- which is the first thing anyone checks.
    void Banner(const char* msg);
    void BannerFmt(const char* fmt, ...);

    void Info(const char* msg);
    void InfoFmt(const char* fmt, ...);
    void Warn(const char* msg);
    void WarnFmt(const char* fmt, ...);
    void Error(const char* msg);
    void ErrorFmt(const char* fmt, ...);
    std::string Format(const char* fmt, ...);
}

#pragma once

namespace GakumasModManager {
    // Diagnostics.  Hidden unless config.json raises "logLevel" to info.
    void Log(const char* message);

    // Reserved for the paths that turn a feature off: these are the lines that
    // explain a missing entry or a dead page, so they must survive the default
    // error-only level.  Everything else is Log().
    void LogError(const char* message);
}

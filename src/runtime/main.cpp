#ifndef GKMS_VERSION
#define GKMS_VERSION "dev"
#endif

#include "ModConfig.hpp"
#include "ModLog.hpp"
#include "ModPaths.hpp"
#include "ModRuntime.hpp"
#include "ModRuntimeCatalog.hpp"
#include "RuntimeBootstrap.hpp"
#include "gkmm/ManagerEntry.hpp"

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    const char g_moduleAnchor{};
    std::once_flag g_startOnce;

    void PinRuntimeModule() {
        HMODULE pinnedModule{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&g_moduleAnchor),
                &pinnedModule)) {
            throw std::runtime_error("cannot pin runtime module");
        }
    }

    void SetGameWorkingDirectory() {
        std::string moduleName;
        moduleName.resize(MAX_PATH);
        moduleName.resize(GetModuleFileNameA(nullptr, moduleName.data(), MAX_PATH));

        const std::filesystem::path exePath(moduleName);
        if (exePath.filename() != "gakumas.exe") {
            GakumasMod::Log::WarnFmt("[ModAsset] Host process is not gakumas.exe: %s",
                exePath.filename().string().c_str());
            return;
        }

        std::error_code ec;
        std::filesystem::current_path(exePath.parent_path(), ec);
        if (ec) {
            GakumasMod::Log::ErrorFmt("[ModAsset] Failed to set working directory: %s",
                ec.message().c_str());
        }
    }

    void BootstrapRuntime() noexcept {
        try {
            // Before Initialize: its own lines have to obey the configured
            // level too. SetGameWorkingDirectory already ran, so the relative
            // config path resolves.
            const auto requested = GakumasMod::Config::ReadLogLevel(
                GakumasMod::Paths::Config());
            if (requested) {
                if (const auto level = GakumasMod::Log::ParseLevel(*requested)) {
                    GakumasMod::Log::SetMinLevel(*level);
                }
                else {
                    GakumasMod::Log::ErrorFmt(
                        "[ModAsset] config.json: logLevel=\"%s\" is not info/warn/error;"
                        " keeping error.",
                        requested->c_str());
                }
            }

            const char* levelName = "error";
            switch (GakumasMod::Log::MinLevel()) {
                case GakumasMod::Log::Level::Info: levelName = "info"; break;
                case GakumasMod::Log::Level::Warn: levelName = "warn"; break;
                case GakumasMod::Log::Level::Error: levelName = "error"; break;
            }
            GakumasMod::Log::BannerFmt(
                "[ModAsset] gakumas-mod-runtime %s loaded. logLevel=%s (config.json: %s)."
                " Raise it to \"info\" for a full trace.",
                GKMS_VERSION,
                levelName,
                requested ? "set" : "absent, using the default");

            GakumasMod::Runtime::Initialize();
            // Same condition the manager used to poll GetModsJson for, now
            // checked once: hooks may have failed while the catalog is still
            // readable, and the UI is useful in that case too.
            if (!GakumasMod::Runtime::Catalog::IsReady()) return;

            // Deleting the standalone manager DLL used to disable the UI and
            // leave the mods working; one binary needs a switch for that.
            const auto configured = GakumasMod::Config::ReadManagerUiEnabled(
                GakumasMod::Paths::Config());
            GakumasMod::Log::InfoFmt("[ModAsset] Manager UI enabled=%d (config.json: %s).",
                configured.value_or(true) ? 1 : 0,
                configured ? "modManagerUi" : "absent or unusable, defaulting on");
            if (configured.value_or(true)) GkmmInitialize();
        }
        catch (...) {
            OutputDebugStringA("[GakumasMod] Runtime bootstrap failed.\n");
        }
    }
}

namespace GakumasMod::Bootstrap {
    void EnsureStarted() noexcept {
        try {
            std::call_once(g_startOnce, [] {
                // This DLL installs process-wide hooks and owns detached probe
                // threads. Pin it before either starts so FreeLibrary cannot
                // leave callbacks pointing into an unloaded image.
                PinRuntimeModule();
                SetGameWorkingDirectory();
                std::thread(BootstrapRuntime).detach();
            });
        }
        catch (...) {
            // A failed pin/thread creation leaves initialization uncommitted;
            // std::call_once permits a later XInput call to retry.
            OutputDebugStringA("[GakumasMod] Could not start runtime bootstrap.\n");
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

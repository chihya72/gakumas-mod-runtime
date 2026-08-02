#include "ModConfig.hpp"
#include "ModLog.hpp"
#include "ModPaths.hpp"
#include "ModRuntime.hpp"
#include "ModRuntimeCatalog.hpp"
#include "gkmm/ManagerEntry.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>
#include <thread>

namespace {
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
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        SetGameWorkingDirectory();
        std::thread([] {
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
        }).detach();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        GkmmShutdown();
        GakumasMod::Runtime::Shutdown();
    }
    return TRUE;
}

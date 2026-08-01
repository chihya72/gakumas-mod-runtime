#include "gkmm/RuntimeClient.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
    std::atomic<bool> g_stop{false};
    std::atomic<bool> g_started{false};
    std::mutex g_logMutex;

    std::filesystem::path LogPath() {
        wchar_t modulePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(modulePath, modulePath + length).parent_path()
                / L"gakumas-local" / L"mod-manager.log";
        }
        return std::filesystem::path(L"gakumas-local") / L"mod-manager.log";
    }

    void DebugLog(const char* message) {
        SYSTEMTIME now{};
        GetLocalTime(&now);

        char line[1024]{};
        std::snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [GakumasModManager] %s\n",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds,
            message);

        OutputDebugStringA(line);

        std::lock_guard lock(g_logMutex);
        const auto path = LogPath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
        if (file) {
            file << line;
            file.flush();
        }
    }

    void BootstrapThread() {
        using namespace std::chrono_literals;
        DebugLog("xinput1_4.dll loaded; manager bootstrap thread started.");
        GakumasModManager::RuntimeClient runtime;
        for (int attempt = 0; attempt < 600 && !g_stop.load(); ++attempt) {
            if (runtime.Connect() && runtime.IsReady()) {
                DebugLog("M1 probe connected to Runtime API v1; UI hook is intentionally disabled.");
                return;
            }
            if (attempt == 0 || attempt % 100 == 0) {
                DebugLog("Waiting for a ready Runtime API.");
            }
            std::this_thread::sleep_for(100ms);
        }
        if (g_stop.load()) {
            DebugLog("Manager bootstrap stopped before Runtime API became ready.");
        }
        else {
            DebugLog("M1 probe did not find a ready Runtime API; manager entry remains disabled.");
        }
    }

    void StartBootstrap() {
        if (g_started.exchange(true)) return;
        g_stop.store(false);
        std::thread(BootstrapThread).detach();
    }
}
extern "C" bool GkmmInitialize() {
    StartBootstrap();
    return true;
}

extern "C" void GkmmShutdown() {
    g_stop.store(true);
    DebugLog("GkmmShutdown requested.");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        StartBootstrap();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true);
    }
    return TRUE;
}

#include "gkmm/RuntimeClient.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
    std::atomic<bool> g_stop{false};
    std::atomic<bool> g_started{false};

    void DebugLog(const char* message) {
        OutputDebugStringA("[GakumasModManager] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }

    void BootstrapThread() {
        using namespace std::chrono_literals;
        GakumasModManager::RuntimeClient runtime;
        for (int attempt = 0; attempt < 600 && !g_stop.load(); ++attempt) {
            if (runtime.Connect() && runtime.IsReady()) {
                DebugLog("M1 probe connected to Runtime API v1; UI hook is intentionally disabled.");
                return;
            }
            std::this_thread::sleep_for(100ms);
        }
        DebugLog("M1 probe did not find a ready Runtime API; manager entry remains disabled.");
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

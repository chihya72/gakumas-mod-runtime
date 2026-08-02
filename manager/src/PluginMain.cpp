#include "gkmm/RuntimeClient.hpp"
#include "gkmm/CampusUiProbe.hpp"
#include "gkmm/ManagerEntry.hpp"
#include "gkmm/ManagerLog.hpp"

#include "ModLog.hpp"
#include "ModPaths.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace {
    std::atomic<bool> g_stop{false};
    std::atomic<bool> g_started{false};
    std::mutex g_logMutex;

    std::filesystem::path LogPath() {
        namespace Paths = GakumasMod::Paths;
        wchar_t modulePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(modulePath, modulePath + length).parent_path()
                / Paths::kRootName / Paths::kManagerLogName;
        }
        return std::filesystem::path(Paths::kRootName) / Paths::kManagerLogName;
    }

    std::string ProcessName() {
        wchar_t modulePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return "<unknown>";

        const auto name = std::filesystem::path(modulePath, modulePath + length).filename().wstring();
        const auto byteCount = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
            nullptr, 0, nullptr, nullptr);
        if (byteCount <= 0) return "<unknown>";

        std::string result(static_cast<std::size_t>(byteCount), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                result.data(), byteCount, nullptr, nullptr) != byteCount) {
            return "<unknown>";
        }
        return result;
    }

    void WriteLog(const char* message) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        static const auto processName = ProcessName();

        char line[1024]{};
        std::snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [GakumasModManager] "
            "[pid=%lu process=%s] %s\n",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds,
            static_cast<unsigned long>(GetCurrentProcessId()),
            processName.c_str(),
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

    // The runtime calls in from its own init thread once the catalog is ready,
    // so there is nothing left to wait for: no module-load race (one DLL) and
    // no readiness race (the caller is the thing that became ready).
    void StartBootstrap() {
        if (g_started.exchange(true)) return;
        g_stop.store(false);
        GakumasModManager::RuntimeClient runtime;
        std::string snapshot;
        if (!runtime.Connect() || !runtime.GetModsJson(snapshot)) {
            GakumasModManager::LogError(
                "Runtime API v1 is not answering; manager entry remains disabled.");
            return;
        }
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "Runtime API v1 ready (%zu-byte snapshot); starting the in-game UI probe.",
            snapshot.size());
        GakumasModManager::Log(message);
        GakumasModManager::StartCampusUiProbe();
    }
}

namespace GakumasModManager {
    // The manager keeps its own file but shares the runtime's level, so one
    // config key controls both logs.
    void Log(const char* message) {
        if (!GakumasMod::Log::IsEnabled(GakumasMod::Log::Level::Info)) return;
        WriteLog(message);
    }

    void LogError(const char* message) {
        if (!GakumasMod::Log::IsEnabled(GakumasMod::Log::Level::Error)) return;
        WriteLog(message);
    }
}

extern "C" bool GkmmInitialize() {
    StartBootstrap();
    return true;
}

extern "C" void GkmmShutdown() {
    if (!g_started.load()) return;
    g_stop.store(true);
    GakumasModManager::Log("GkmmShutdown requested.");
}

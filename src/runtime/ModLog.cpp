#include "ModLog.hpp"
#include "ModPaths.hpp"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace GakumasMod::Log {
    namespace {

        std::string VFormat(const char* fmt, va_list args) {
            if (!fmt) return {};

            va_list argsCopy;
            va_copy(argsCopy, args);
            const auto size = std::vsnprintf(nullptr, 0, fmt, argsCopy);
            va_end(argsCopy);
            if (size <= 0) return {};

            std::string result(static_cast<size_t>(size) + 1, '\0');
            std::vsnprintf(result.data(), result.size(), fmt, args);
            result.resize(static_cast<size_t>(size));
            return result;
        }

        std::ofstream& Stream() {
            static std::ofstream stream;
            static bool initialized = false;
            if (!initialized) {
                initialized = true;
                std::error_code ec;
                std::filesystem::create_directories(Paths::Root(), ec);
                stream.open(Paths::RuntimeLog(), std::ios::app);
            }
            return stream;
        }

        std::atomic<Level> g_minLevel{Level::Error};

        void Write(const Level level, const char* label, const char* msg) {
            if (level < g_minLevel.load(std::memory_order_relaxed)) return;
            static std::mutex mutex;
            std::lock_guard lock(mutex);

            auto& stream = Stream();
            if (stream.is_open()) {
                stream << "[" << label << "] GakumasMod: " << (msg ? msg : "") << '\n';
                stream.flush();
            }

            char debugLine[4096]{};
            std::snprintf(debugLine, sizeof(debugLine), "[%s] GakumasMod: %s\n", label, msg ? msg : "");
            OutputDebugStringA(debugLine);
        }
    }

    void SetMinLevel(const Level level) {
        g_minLevel.store(level, std::memory_order_relaxed);
    }

    Level MinLevel() {
        return g_minLevel.load(std::memory_order_relaxed);
    }

    bool IsEnabled(const Level level) {
        return level >= g_minLevel.load(std::memory_order_relaxed);
    }

    std::optional<Level> ParseLevel(const std::string_view name) {
        std::string lowered;
        lowered.reserve(name.size());
        for (const auto c : name) {
            lowered.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        if (lowered == "info") return Level::Info;
        if (lowered == "warn") return Level::Warn;
        if (lowered == "error") return Level::Error;
        return std::nullopt;
    }

    std::string Format(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        auto result = VFormat(fmt, args);
        va_end(args);
        return result;
    }

    void Banner(const char* msg) {
        // MinLevel() as its own level: passes the filter by construction.
        Write(MinLevel(), "BOOT", msg);
    }

    void BannerFmt(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        const auto result = VFormat(fmt, args);
        va_end(args);
        Banner(result.c_str());
    }

    void Info(const char* msg) {
        Write(Level::Info, "INFO", msg);
    }

    void InfoFmt(const char* fmt, ...) {
        if (!IsEnabled(Level::Info)) return;
        va_list args;
        va_start(args, fmt);
        const auto result = VFormat(fmt, args);
        va_end(args);
        Info(result.c_str());
    }

    void Warn(const char* msg) {
        Write(Level::Warn, "WARN", msg);
    }

    void WarnFmt(const char* fmt, ...) {
        if (!IsEnabled(Level::Warn)) return;
        va_list args;
        va_start(args, fmt);
        const auto result = VFormat(fmt, args);
        va_end(args);
        Warn(result.c_str());
    }

    void Error(const char* msg) {
        Write(Level::Error, "ERROR", msg);
    }

    void ErrorFmt(const char* fmt, ...) {
        if (!IsEnabled(Level::Error)) return;
        va_list args;
        va_start(args, fmt);
        const auto result = VFormat(fmt, args);
        va_end(args);
        Error(result.c_str());
    }
}

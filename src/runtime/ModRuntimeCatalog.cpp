#include "ModRuntimeCatalog.hpp"

#include "ModLog.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace GakumasMod::Runtime::Catalog {
    namespace {
        using json = nlohmann::json;

        struct ModRecord {
            std::string id;
            std::string name;
            std::filesystem::path manifestPath;
            std::string manifestStamp;
            std::string targetKind;
            std::string targetSource;
            std::string targetKey;
            std::string targetPart;
            std::string manifestState{"invalid_manifest"};
            std::string runtimeState{"invalid"};
            std::string conflictWithId;
            std::string conflictWithName;
            int priority{0};
            bool configuredEnabled{true};
            bool registeredThisSession{false};
            bool appliedThisSession{false};
            bool restartRequired{false};
        };

        std::shared_mutex g_catalogMutex;
        std::vector<ModRecord> g_records;
        bool g_ready{false};

        std::string ToLowerAscii(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string Trim(std::string value) {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return {};
            const auto end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        std::optional<std::string> GetString(const json& object,
                                             std::initializer_list<const char*> keys) {
            if (!object.is_object()) return std::nullopt;
            for (const auto key : keys) {
                if (object.contains(key) && object[key].is_string()) {
                    return object[key].get<std::string>();
                }
            }
            return std::nullopt;
        }

        std::string GetRecordId(const json& manifest, const std::filesystem::path& manifestPath) {
            const auto id = GetString(manifest, {"id"});
            if (id && !Trim(*id).empty()) return Trim(*id);
            return manifestPath.parent_path().filename().string();
        }

        std::string GetManifestStamp(const std::filesystem::path& path) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec) return {};
            const auto time = std::filesystem::last_write_time(path, ec);
            if (ec) return {};
            return std::to_string(size) + ":"
                + std::to_string(time.time_since_epoch().count());
        }

        bool ReadJson(const std::filesystem::path& path, json& output) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return false;
            try {
                output = json::parse(input);
                return true;
            }
            catch (const json::exception&) {
                return false;
            }
        }

        bool IsInside(const std::filesystem::path& root,
                      const std::filesystem::path& candidate) {
            if (candidate.is_absolute()) return false;
            const auto rootText = root.lexically_normal().generic_string();
            const auto candidateText = candidate.lexically_normal().generic_string();
            if (rootText.empty() || candidateText.empty()) return false;
            const auto prefix = rootText.ends_with('/') ? rootText : rootText + '/';
            return candidateText.starts_with(prefix);
        }

        std::string ParseLogicalKey(const std::string& source, const std::string& part) {
            auto value = Trim(source);
            const auto lower = ToLowerAscii(value);
            if (lower.rfind("mdl_chr_", 0) == 0) {
                value = value.substr(8);
            }

            const auto suffix = "_" + ToLowerAscii(part);
            const auto lowerValue = ToLowerAscii(value);
            if (lowerValue.size() > suffix.size() && lowerValue.ends_with(suffix)) {
                value.resize(value.size() - suffix.size());
            }
            return value;
        }

        void SetInvalid(ModRecord& record, const char* state) {
            record.manifestState = state;
            record.runtimeState = "invalid";
            record.registeredThisSession = false;
        }

        ModRecord ParseManifest(const std::filesystem::path& manifestPath) {
            ModRecord record{};
            record.manifestPath = manifestPath;
            record.manifestStamp = GetManifestStamp(manifestPath);
            record.id = manifestPath.parent_path().filename().string();
            record.name = record.id;

            json manifest;
            if (!ReadJson(manifestPath, manifest) || !manifest.is_object()) {
                SetInvalid(record, "invalid_manifest");
                return record;
            }

            record.id = GetRecordId(manifest, manifestPath);
            record.name = GetString(manifest, {"name"}).value_or(record.id);
            record.name = Trim(record.name);
            if (record.name.empty()) record.name = record.id;
            if (manifest.contains("enabled") && manifest["enabled"].is_boolean()) {
                record.configuredEnabled = manifest["enabled"].get<bool>();
            }
            if (manifest.contains("priority") && manifest["priority"].is_number_integer()) {
                record.priority = manifest["priority"].get<int>();
            }

            if (!manifest.contains("replacements") || !manifest["replacements"].is_array()
                || manifest["replacements"].empty()) {
                SetInvalid(record, "invalid_manifest");
                return record;
            }

            bool targetMismatch = false;
            bool unsupportedPart = false;
            bool missingBundle = false;
            bool invalidReplacement = false;
            const auto manifestDir = manifestPath.parent_path();

            for (const auto& item : manifest["replacements"]) {
                if (!item.is_object()) {
                    invalidReplacement = true;
                    continue;
                }

                const auto source = GetString(item, {"from", "source", "target"});
                const auto partValue = GetString(item, {"part"});
                const auto bundle = GetString(item, {"bundle", "assetBundle", "assetbundle"});
                if (!source || !partValue || !bundle) {
                    invalidReplacement = true;
                    continue;
                }

                const auto part = ToLowerAscii(Trim(*partValue));
                if (part != "body" && part != "hair") {
                    unsupportedPart = true;
                }

                const auto logicalKey = ParseLogicalKey(*source, part);
                if (logicalKey.empty()) {
                    invalidReplacement = true;
                }
                else if (record.targetKey.empty()) {
                    record.targetKey = logicalKey;
                    record.targetKind = part == "body" ? "costume" : "hair";
                    record.targetPart = part;
                    record.targetSource = *source;
                }
                else if (record.targetKey != logicalKey || record.targetPart != part) {
                    targetMismatch = true;
                }

                const std::filesystem::path bundlePath(*bundle);
                const auto normalizedBundle = (manifestDir / bundlePath).lexically_normal();
                if (!IsInside(manifestDir, normalizedBundle)
                    || !std::filesystem::is_regular_file(normalizedBundle)) {
                    missingBundle = true;
                }
            }

            if (invalidReplacement || record.targetKey.empty()) {
                SetInvalid(record, "invalid_replacement");
            }
            else if (targetMismatch) {
                SetInvalid(record, "multiple_targets");
            }
            else if (unsupportedPart) {
                SetInvalid(record, "unsupported_target");
            }
            else if (missingBundle) {
                SetInvalid(record, "missing_bundle");
            }
            else {
                record.manifestState = "valid";
                record.registeredThisSession = record.configuredEnabled;
                record.runtimeState = record.configuredEnabled ? "active" : "disabled";
            }
            return record;
        }

        void ApplyConflicts(std::vector<ModRecord>& records) {
            std::unordered_map<std::string, std::vector<size_t>> groups;
            for (size_t index = 0; index < records.size(); ++index) {
                const auto& record = records[index];
                if (record.manifestState != "valid" || !record.configuredEnabled) continue;
                groups[record.targetKind + "|" + record.targetKey].push_back(index);
            }

            for (auto& [groupKey, indexes] : groups) {
                (void)groupKey;
                if (indexes.size() < 2) continue;
                std::sort(indexes.begin(), indexes.end(), [&records](size_t left, size_t right) {
                    if (records[left].priority != records[right].priority) {
                        return records[left].priority < records[right].priority;
                    }
                    return records[left].manifestPath.string() < records[right].manifestPath.string();
                });

                const auto winner = indexes.back();
                for (const auto index : indexes) {
                    if (index == winner) continue;
                    records[index].registeredThisSession = false;
                    records[index].runtimeState = "conflict_lost";
                    records[index].conflictWithId = records[winner].id;
                    records[index].conflictWithName = records[winner].name;
                }
            }
        }

        json ToJson(const ModRecord& record) {
            json output{
                {"id", record.id},
                {"name", record.name},
                {"configuredEnabled", record.configuredEnabled},
                {"registeredThisSession", record.registeredThisSession},
                {"appliedThisSession", record.appliedThisSession},
                {"restartRequired", record.restartRequired},
                {"manifestState", record.manifestState},
                {"runtimeState", record.runtimeState},
            };

            if (!record.targetKey.empty()) {
                output["target"] = {
                    {"kind", record.targetKind},
                    {"source", record.targetSource},
                    {"masterKey", record.targetKey},
                    {"part", record.targetPart},
                };
            }
            else {
                output["target"] = nullptr;
            }

            if (!record.conflictWithId.empty()) {
                output["conflict"] = {
                    {"withModId", record.conflictWithId},
                    {"withModName", record.conflictWithName},
                };
            }
            else {
                output["conflict"] = nullptr;
            }
            return output;
        }

        GmrResult PersistEnabled(ModRecord& record, bool enabled) {
            json manifest;
            if (!ReadJson(record.manifestPath, manifest) || !manifest.is_object()) {
                return GMR_E_MANIFEST_INVALID;
            }
            if (GetRecordId(manifest, record.manifestPath) != record.id) {
                return GMR_E_CONCURRENT_CHANGE;
            }
            if (GetManifestStamp(record.manifestPath) != record.manifestStamp) {
                return GMR_E_CONCURRENT_CHANGE;
            }

            manifest["enabled"] = enabled;
            const auto tempPath = record.manifestPath.wstring() + L".gmr.tmp";
            {
                std::ofstream output(std::filesystem::path(tempPath), std::ios::binary | std::ios::trunc);
                if (!output) return GMR_E_ACCESS_DENIED;
                output << manifest.dump(2, ' ', false) << '\n';
                output.flush();
                if (!output) return GMR_E_IO;
            }

            if (!MoveFileExW(tempPath.c_str(), record.manifestPath.wstring().c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const auto error = GetLastError();
                std::error_code ec;
                std::filesystem::remove(tempPath, ec);
                return error == ERROR_ACCESS_DENIED ? GMR_E_ACCESS_DENIED : GMR_E_IO;
            }

            record.manifestStamp = GetManifestStamp(record.manifestPath);
            record.configuredEnabled = enabled;
            record.restartRequired = record.configuredEnabled != record.registeredThisSession;
            if (record.manifestState == "valid") {
                if (record.restartRequired) {
                    record.runtimeState = enabled ? "pending_enable" : "pending_disable";
                }
                else {
                    record.runtimeState = enabled ? "active" : "disabled";
                }
            }
            return GMR_OK;
        }
    }

    void Refresh() {
        const auto modRoot = std::filesystem::path("./gakumas-local/local-files/mods");
        std::vector<std::filesystem::path> manifests;
        std::error_code ec;
        if (std::filesystem::exists(modRoot, ec) && !ec) {
            for (const auto& entry : std::filesystem::directory_iterator(modRoot, ec)) {
                if (ec) break;
                if (!entry.is_directory(ec) || ec) continue;
                const auto manifestPath = entry.path() / "mod.json";
                if (std::filesystem::is_regular_file(manifestPath, ec) && !ec) {
                    manifests.push_back(manifestPath);
                }
            }
        }
        std::sort(manifests.begin(), manifests.end());

        std::vector<ModRecord> records;
        records.reserve(manifests.size());
        for (const auto& manifest : manifests) {
            records.push_back(ParseManifest(manifest));
        }
        ApplyConflicts(records);

        std::unique_lock lock(g_catalogMutex);
        g_records = std::move(records);
    }

    void Clear() {
        std::unique_lock lock(g_catalogMutex);
        g_records.clear();
        g_ready = false;
    }

    void SetReady(const bool ready) {
        std::unique_lock lock(g_catalogMutex);
        g_ready = ready;
    }

    GmrResult GetModsJson(GmrOwnedBuffer* output) {
        if (!output) return GMR_E_INVALID_ARGUMENT;
        output->data = nullptr;
        output->size = 0;

        std::shared_lock lock(g_catalogMutex);
        if (!g_ready) return GMR_E_NOT_INITIALIZED;

        json root{
            {"schemaVersion", 1},
            {"mods", json::array()},
        };
        for (const auto& record : g_records) {
            root["mods"].push_back(ToJson(record));
        }
        const auto serialized = root.dump(2, ' ', false);
        auto* data = static_cast<char*>(std::malloc(serialized.size() + 1));
        if (!data) return GMR_E_INTERNAL;
        std::memcpy(data, serialized.data(), serialized.size());
        data[serialized.size()] = '\0';
        output->data = data;
        output->size = serialized.size();
        return GMR_OK;
    }

    void FreeBuffer(void* data) {
        std::free(data);
    }

    GmrResult SetModEnabled(const char* modIdUtf8, const uint8_t enabled) {
        if (!modIdUtf8 || !*modIdUtf8) return GMR_E_INVALID_ARGUMENT;

        std::unique_lock lock(g_catalogMutex);
        if (!g_ready) return GMR_E_NOT_INITIALIZED;

        ModRecord* match = nullptr;
        for (auto& record : g_records) {
            if (record.id != modIdUtf8) continue;
            if (match) return GMR_E_CONCURRENT_CHANGE;
            match = &record;
        }
        if (!match) return GMR_E_MOD_NOT_FOUND;
        if (match->configuredEnabled == (enabled != 0)) return GMR_OK;
        return PersistEnabled(*match, enabled != 0);
    }

    void WriteLog(const uint32_t level, const char* component, const char* messageUtf8) {
        const auto safeComponent = component && *component ? component : "RuntimeApi";
        const auto safeMessage = messageUtf8 && *messageUtf8 ? messageUtf8 : "";
        switch (level) {
        case 2:
            Log::ErrorFmt("[ModManager.%s] %s", safeComponent, safeMessage);
            break;
        case 1:
            Log::WarnFmt("[ModManager.%s] %s", safeComponent, safeMessage);
            break;
        default:
            Log::InfoFmt("[ModManager.%s] %s", safeComponent, safeMessage);
            break;
        }
    }
}

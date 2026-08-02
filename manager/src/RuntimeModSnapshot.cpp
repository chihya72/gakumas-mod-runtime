#include "gkmm/RuntimeModSnapshot.hpp"

#include "../../src/deps/nlohmann/json.hpp"

#include <exception>
#include <utility>

namespace GakumasModManager {
    namespace {
        using Json = nlohmann::json;

        std::string StringOr(const Json& object, const char* key, std::string fallback = {}) {
            const auto value = object.find(key);
            return value != object.end() && value->is_string()
                ? value->get<std::string>()
                : std::move(fallback);
        }

        bool BoolOr(const Json& object, const char* key, const bool fallback = false) {
            const auto value = object.find(key);
            return value != object.end() && value->is_boolean()
                ? value->get<bool>()
                : fallback;
        }

        RuntimeTargetKind ParseTargetKind(const std::string& kind) {
            if (kind == "costume") return RuntimeTargetKind::Costume;
            if (kind == "hair") return RuntimeTargetKind::Hair;
            return RuntimeTargetKind::Unknown;
        }
    }

    bool ParseRuntimeModSnapshot(
        const std::string_view jsonText,
        RuntimeModSnapshot& output,
        std::string& errorMessage) {
        output = {};
        errorMessage.clear();

        Json root;
        try {
            root = Json::parse(jsonText.begin(), jsonText.end());
        }
        catch (const std::exception&) {
            errorMessage = "Mod 状态数据不是有效的 JSON。";
            return false;
        }

        if (!root.is_object()) {
            errorMessage = "Mod 状态数据的根节点格式不正确。";
            return false;
        }

        const auto schemaVersion = root.find("schemaVersion");
        if (schemaVersion == root.end() || !schemaVersion->is_number_unsigned()
            || schemaVersion->get<unsigned>() != 1) {
            errorMessage = "Mod 状态数据版本不受支持。";
            return false;
        }

        const auto mods = root.find("mods");
        if (mods == root.end() || !mods->is_array()) {
            errorMessage = "Mod 状态数据缺少列表。";
            return false;
        }

        output.mods.reserve(mods->size());
        for (const auto& value : *mods) {
            if (!value.is_object()) {
                ++output.malformedRecordCount;
                continue;
            }

            RuntimeModRecord record;
            record.id = StringOr(value, "id");
            if (record.id.empty()) {
                ++output.malformedRecordCount;
                continue;
            }

            record.name = StringOr(value, "name", "未命名 Mod");
            if (record.name.empty()) record.name = "未命名 Mod";
            record.configuredEnabled = BoolOr(value, "configuredEnabled");
            record.registeredThisSession = BoolOr(value, "registeredThisSession");
            record.appliedThisSession = BoolOr(value, "appliedThisSession");
            record.restartRequired = BoolOr(value, "restartRequired");
            record.manifestState = StringOr(value, "manifestState", "unknown");
            record.runtimeState = StringOr(value, "runtimeState", "unknown");

            const auto target = value.find("target");
            if (target != value.end() && target->is_object()) {
                record.target.kind = ParseTargetKind(StringOr(*target, "kind"));
                record.target.source = StringOr(*target, "source");
                record.target.masterKey = StringOr(*target, "masterKey");
            }

            const auto conflict = value.find("conflict");
            if (conflict != value.end() && conflict->is_object()) {
                record.conflict.modId = StringOr(*conflict, "withModId");
                record.conflict.modName = StringOr(*conflict, "withModName");
            }

            output.mods.push_back(std::move(record));
        }
        return true;
    }
}

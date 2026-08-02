#include "gkmm/ModPresentationModel.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace GakumasModManager {
    namespace {
        ModDisplayState ResolveState(const RuntimeModRecord& record) {
            if (record.manifestState != "valid") return ModDisplayState::ConfigurationError;
            if (record.runtimeState == "conflict_enable_blocked") {
                return ModDisplayState::EnableBlockedByConflict;
            }
            if (!record.conflict.modId.empty()
                || record.runtimeState == "conflict_lost"
                || record.runtimeState == "conflict_auto_disabled") {
                return ModDisplayState::Conflict;
            }
            if (record.restartRequired) {
                return record.configuredEnabled
                    ? ModDisplayState::RestartToEnable
                    : ModDisplayState::RestartToDisable;
            }
            return record.configuredEnabled
                ? ModDisplayState::Enabled
                : ModDisplayState::Disabled;
        }

        std::string StatusText(
            const ModDisplayState state,
            const ModCategory category,
            const RuntimeModConflict& conflict) {
            switch (state) {
            case ModDisplayState::Enabled:
                return "启用中";
            case ModDisplayState::Disabled:
                return "已关闭";
            case ModDisplayState::RestartToEnable:
                return "重启后启用";
            case ModDisplayState::RestartToDisable:
                return "重启后停用";
            case ModDisplayState::ConfigurationError:
                return "配置有误";
            case ModDisplayState::Conflict: {
                const auto target = category == ModCategory::Costume ? "服装" : "发型";
                if (!conflict.modName.empty()) {
                    return "Mod 冲突：与“" + conflict.modName + "”使用同一" + target
                        + "，已自动关闭";
                }
                return "Mod 冲突：与其他 Mod 使用同一" + std::string(target)
                    + "，已自动关闭";
            }
            case ModDisplayState::EnableBlockedByConflict: {
                const auto target = category == ModCategory::Costume ? "服装" : "发型";
                if (!conflict.modName.empty()) {
                    return "无法开启：“" + conflict.modName + "”已开启同一" + target
                        + "，请先检查并关闭它";
                }
                return "无法开启：已有其他 Mod 开启同一" + std::string(target)
                    + "，请先检查并关闭它";
            }
            }
            return "状态异常";
        }

        void SortItems(std::vector<ModPresentationItem>& items) {
            std::stable_sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
                if (left.title != right.title) return left.title < right.title;
                return left.modId < right.modId;
            });
        }

        void AppendSection(
            std::string& output,
            const std::string_view title,
            const std::vector<ModPresentationItem>& items) {
            if (items.empty()) return;
            output += std::string(title) + "（" + std::to_string(items.size()) + "）\n";
            for (const auto& item : items) {
                output += "· " + item.title + "    " + item.statusText + "\n";
            }
            output += "\n";
        }
    }

    ModPresentationModel BuildModPresentationModel(const RuntimeModSnapshot& snapshot) {
        ModPresentationModel model;
        model.totalModCount = snapshot.mods.size() + snapshot.malformedRecordCount;
        model.invalidModCount = snapshot.malformedRecordCount;

        for (const auto& record : snapshot.mods) {
            ModCategory category;
            switch (record.target.kind) {
            case RuntimeTargetKind::Costume:
                category = ModCategory::Costume;
                break;
            case RuntimeTargetKind::Hair:
                category = ModCategory::Hair;
                break;
            case RuntimeTargetKind::Unknown:
                ++model.invalidModCount;
                continue;
            }

            ModPresentationItem item;
            item.modId = record.id;
            item.title = record.name;
            item.targetSource = record.target.source;
            item.targetMasterKey = record.target.masterKey;
            item.category = category;
            item.configuredEnabled = record.configuredEnabled;
            item.state = ResolveState(record);
            item.statusText = StatusText(item.state, category, record.conflict);
            model.restartRequired = model.restartRequired || record.restartRequired;

            auto& items = category == ModCategory::Costume
                ? model.costumeMods
                : model.hairMods;
            items.push_back(std::move(item));
        }

        SortItems(model.costumeMods);
        SortItems(model.hairMods);
        return model;
    }

    std::string NormalizeHairAssetKey(const std::string_view value) {
        auto first = value.begin();
        auto last = value.end();
        while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
        while (first != last
               && std::isspace(static_cast<unsigned char>(*(last - 1)))) {
            --last;
        }

        std::string normalized(first, last);
        std::transform(
            normalized.begin(),
            normalized.end(),
            normalized.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        if (const auto slash = normalized.find_last_of('/'); slash != std::string::npos) {
            normalized.erase(0, slash + 1);
        }
        constexpr std::string_view prefabSuffix = ".prefab";
        if (normalized.size() >= prefabSuffix.size()
            && normalized.compare(
                   normalized.size() - prefabSuffix.size(),
                   prefabSuffix.size(),
                   prefabSuffix) == 0) {
            normalized.resize(normalized.size() - prefabSuffix.size());
        }
        constexpr std::string_view assetPrefix = "mdl_chr_";
        if (normalized.rfind(assetPrefix, 0) == 0) {
            normalized.erase(0, assetPrefix.size());
        }

        constexpr std::string_view meshSuffix = "_hair";
        if (const auto suffix = normalized.find(meshSuffix);
            suffix != std::string::npos
            && (suffix + meshSuffix.size() == normalized.size()
                || normalized.compare(
                       suffix + meshSuffix.size(),
                       2,
                       "__") == 0)) {
            normalized.resize(suffix);
        }
        return normalized;
    }

    std::string RenderProbeText(const ModPresentationModel& model) {
        if (model.totalModCount == 0) return "暂未发现 Mod。";

        std::string output = "共 " + std::to_string(model.totalModCount) + " 个 Mod";
        output += "（服装 " + std::to_string(model.costumeMods.size());
        output += " / 发型 " + std::to_string(model.hairMods.size()) + "）\n\n";
        AppendSection(output, "服装", model.costumeMods);
        AppendSection(output, "发型", model.hairMods);

        if (model.invalidModCount != 0) {
            output += "另有 " + std::to_string(model.invalidModCount) + " 个 Mod 配置有误。\n";
        }
        if (model.restartRequired) {
            output += "\n设置将在重启游戏后生效。";
        }
        return output;
    }
}

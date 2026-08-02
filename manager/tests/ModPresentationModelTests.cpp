#include "gkmm/ModPresentationModel.hpp"
#include "gkmm/RuntimeModSnapshot.hpp"

#include "ModConfig.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    using namespace GakumasModManager;

    void Check(const bool condition, const char* expression) {
        if (!condition) throw std::runtime_error(expression);
    }

#define CHECK(expression) Check((expression), #expression)

    RuntimeModSnapshot Parse(const std::string& text) {
        RuntimeModSnapshot snapshot;
        std::string error;
        CHECK(ParseRuntimeModSnapshot(text, snapshot, error));
        CHECK(error.empty());
        return snapshot;
    }

    void TestInvalidEnvelope() {
        RuntimeModSnapshot snapshot;
        std::string error;
        CHECK(!ParseRuntimeModSnapshot("not-json", snapshot, error));
        CHECK(!error.empty());
        CHECK(!ParseRuntimeModSnapshot(R"({"schemaVersion":2,"mods":[]})", snapshot, error));
        CHECK(error == "Mod 状态数据版本不受支持。");
        CHECK(!ParseRuntimeModSnapshot(R"({"schemaVersion":1})", snapshot, error));
        CHECK(error == "Mod 状态数据缺少列表。");
    }

    void TestPresentationAndProbeRendering() {
        const auto snapshot = Parse(R"json({
          "schemaVersion": 1,
          "mods": [
            {
              "id": "costume-active",
              "name": "服装 A",
              "configuredEnabled": true,
              "registeredThisSession": true,
              "restartRequired": false,
              "manifestState": "valid",
              "runtimeState": "active",
              "target": {"kind":"costume","source":"internal-costume-source","masterKey":"costume-key"},
              "conflict": null
            },
            {
              "id": "hair-blocked",
              "name": "发型 B",
              "configuredEnabled": false,
              "registeredThisSession": false,
              "restartRequired": false,
              "manifestState": "valid",
              "runtimeState": "conflict_enable_blocked",
              "target": {"kind":"hair","source":"internal-hair-source","masterKey":"hair-key"},
              "conflict": {"withModId":"hair-active","withModName":"发型 A"}
            },
            {
              "id": "costume-conflict",
              "name": "服装 C",
              "configuredEnabled": false,
              "registeredThisSession": false,
              "restartRequired": false,
              "manifestState": "valid",
              "runtimeState": "conflict_auto_disabled",
              "target": {"kind":"costume","source":"another-source","masterKey":"another-key"},
              "conflict": {"withModId":"costume-active","withModName":"服装 A"}
            },
            {
              "id": "unsupported",
              "name": "不支持的 Mod",
              "configuredEnabled": false,
              "manifestState": "unsupported_part",
              "runtimeState": "invalid",
              "target": null,
              "conflict": null
            },
            "malformed record"
          ]
        })json");

        CHECK(snapshot.mods.size() == 4);
        CHECK(snapshot.malformedRecordCount == 1);
        CHECK(snapshot.mods[0].id == "costume-active");
        CHECK(snapshot.mods[0].target.source == "internal-costume-source");

        const auto model = BuildModPresentationModel(snapshot);
        CHECK(model.totalModCount == 5);
        CHECK(model.costumeMods.size() == 2);
        CHECK(model.hairMods.size() == 1);
        CHECK(model.invalidModCount == 2);
        CHECK(!model.restartRequired);
        CHECK(model.costumeMods[0].modId == "costume-active");
        CHECK(model.costumeMods[0].targetMasterKey == "costume-key");
        CHECK(model.costumeMods[0].targetSource == "internal-costume-source");
        CHECK(model.hairMods[0].state == ModDisplayState::EnableBlockedByConflict);
        CHECK(model.costumeMods[1].state == ModDisplayState::Conflict);

        const auto text = RenderProbeText(model);
        CHECK(text.find("共 5 个 Mod（服装 2 / 发型 1）") != std::string::npos);
        CHECK(text.find("无法开启：“发型 A”已开启同一发型，请先检查并关闭它") != std::string::npos);
        CHECK(text.find("Mod 冲突：与“服装 A”使用同一服装，已自动关闭") != std::string::npos);
        CHECK(text.find("另有 2 个 Mod 配置有误") != std::string::npos);
        CHECK(text.find("设置将在重启游戏后生效") == std::string::npos);
        CHECK(text.find("costume-active") == std::string::npos);
        CHECK(text.find("internal-costume-source") == std::string::npos);
    }

    void TestBadRecordDoesNotBlockOthers() {
        const auto snapshot = Parse(R"json({
          "schemaVersion": 1,
          "mods": [
            {"id":42,"name":"bad"},
            {
              "id":"valid",
              "name":42,
              "configuredEnabled":false,
              "manifestState":"valid",
              "runtimeState":"disabled",
              "target":{"kind":"hair"}
            }
          ]
        })json");
        CHECK(snapshot.mods.size() == 1);
        CHECK(snapshot.malformedRecordCount == 1);
        CHECK(snapshot.mods.front().name == "未命名 Mod");

        const auto model = BuildModPresentationModel(snapshot);
        CHECK(model.hairMods.size() == 1);
        CHECK(model.hairMods.front().state == ModDisplayState::Disabled);
    }

    void TestOrderingDoesNotChangeWithToggleState() {
        const auto first = Parse(R"json({
          "schemaVersion": 1,
          "mods": [
            {
              "id":"z-enabled",
              "name":"Z Mod",
              "configuredEnabled":true,
              "manifestState":"valid",
              "runtimeState":"active",
              "target":{"kind":"costume","masterKey":"z"}
            },
            {
              "id":"a-disabled",
              "name":"A Mod",
              "configuredEnabled":false,
              "manifestState":"valid",
              "runtimeState":"disabled",
              "target":{"kind":"costume","masterKey":"a"}
            }
          ]
        })json");
        const auto second = Parse(R"json({
          "schemaVersion": 1,
          "mods": [
            {
              "id":"z-enabled",
              "name":"Z Mod",
              "configuredEnabled":false,
              "manifestState":"valid",
              "runtimeState":"disabled",
              "target":{"kind":"costume","masterKey":"z"}
            },
            {
              "id":"a-disabled",
              "name":"A Mod",
              "configuredEnabled":true,
              "manifestState":"valid",
              "runtimeState":"active",
              "target":{"kind":"costume","masterKey":"a"}
            }
          ]
        })json");

        const auto firstModel = BuildModPresentationModel(first);
        const auto secondModel = BuildModPresentationModel(second);
        CHECK(firstModel.costumeMods[0].modId == "a-disabled");
        CHECK(firstModel.costumeMods[1].modId == "z-enabled");
        CHECK(secondModel.costumeMods[0].modId == "a-disabled");
        CHECK(secondModel.costumeMods[1].modId == "z-enabled");
    }

    void TestHairAssetKeyNormalization() {
        CHECK(NormalizeHairAssetKey("ttmr-hair-0002") == "ttmr-hair-0002");
        CHECK(NormalizeHairAssetKey("mdl_chr_ttmr-hair-0002") == "ttmr-hair-0002");
        CHECK(NormalizeHairAssetKey("mdl_chr_ttmr-hair-0002_hair") == "ttmr-hair-0002");
        CHECK(NormalizeHairAssetKey(
                  "Assets\\Characters\\mdl_chr_ttmr-hair-0002_hair__Geo_Hair.prefab")
              == "ttmr-hair-0002");
        CHECK(NormalizeHairAssetKey("  MDL_CHR_TTMR-HAIR-0002_HAIR  ")
              == "ttmr-hair-0002");
    }
    // Every "no answer" path has to come back nullopt so the caller keeps the
    // UI on: a broken config.json must not look like a deliberate opt-out.
    void TestManagerUiConfig() {
        const auto dir = std::filesystem::temp_directory_path() / "gkms-config-test";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "config.json";

        const auto write = [&path](const char* text) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
        };
        const auto read = [&path] {
            return GakumasMod::Config::ReadManagerUiEnabled(path);
        };

        std::filesystem::remove(path, ec);
        CHECK(!read().has_value());                       // absent

        write("{ not json");
        CHECK(!read().has_value());                       // unparseable

        write("{}");
        CHECK(!read().has_value());                       // key missing

        write("{\"modManagerUi\": \"false\"}");
        CHECK(!read().has_value());                       // wrong type, not false

        write("{\"modManagerUi\": false}");
        CHECK(read().has_value() && !*read());            // the only way to opt out

        write("{\"modManagerUi\": true}");
        CHECK(read().has_value() && *read());

        std::filesystem::remove_all(dir, ec);
    }
}

int main() {
    try {
        TestInvalidEnvelope();
        TestPresentationAndProbeRendering();
        TestBadRecordDoesNotBlockOthers();
        TestOrderingDoesNotChangeWithToggleState();
        TestHairAssetKeyNormalization();
        TestManagerUiConfig();
    }
    catch (const std::exception& failure) {
        std::cout << "ModPresentationModelTests FAILED: " << failure.what() << "\n";
        return 1;
    }
    std::cout << "ModPresentationModelTests passed\n";
    return 0;
}

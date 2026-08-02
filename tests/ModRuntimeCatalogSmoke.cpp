#include "ModRuntimeApi.h"
#include "ModRuntimeCatalog.hpp"

#include "nlohmann/json.hpp"

#include <Windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace GakumasMod::Log {
    void Info(const char*) {}
    void InfoFmt(const char*, ...) {}
    void Warn(const char*) {}
    void WarnFmt(const char*, ...) {}
    void Error(const char*) {}
    void ErrorFmt(const char*, ...) {}
    std::string Format(const char*, ...) { return {}; }
}

namespace GakumasMod::Runtime {
    GmrResult SetSessionModEnabled(const char*, uint8_t) {
        return GMR_OK;
    }
}

namespace {
    using json = nlohmann::json;

    void WriteText(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        assert(output);
        output << text;
        assert(output);
    }

    json ReadSnapshot() {
        GmrOwnedBuffer buffer{};
        assert(GakumasMod::Runtime::Catalog::GetModsJson(&buffer) == GMR_OK);
        assert(buffer.data != nullptr);
        const std::string text(static_cast<const char*>(buffer.data), buffer.size);
        GakumasMod::Runtime::Catalog::FreeBuffer(buffer.data);
        return json::parse(text);
    }

    const json& FindMod(const json& snapshot, const std::string& id) {
        for (const auto& mod : snapshot.at("mods")) {
            if (mod.at("id").get<std::string>() == id) return mod;
        }
        assert(false && "expected Mod not found");
        return snapshot.at("mods").front();
    }
}

int main() {
    wchar_t tempBuffer[MAX_PATH]{};
    const auto length = GetTempPathW(MAX_PATH, tempBuffer);
    assert(length > 0 && length < MAX_PATH);
    const auto root = std::filesystem::path(tempBuffer) / "gakumas-mod-runtime-m2-smoke";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto mods = root / "gakumas-local" / "local-files" / "mods";
    const auto bodyDir = mods / "body-enabled";
    const auto conflictDir = mods / "body-conflict";
    const auto hairDir = mods / "hair-disabled";
    const auto multiDir = mods / "multi-target";
    const auto missingDir = mods / "missing-bundle";

    const auto bodyManifest = R"json({
      "schemaVersion": 2,
      "id": "body-enabled",
      "name": "Body Enabled",
      "priority": 2,
      "enabled": true,
      "replacements": [{
        "source": "mdl_chr_ttmr-cstm-0111_body",
        "part": "body",
        "bundle": "body.bundle"
      }]
    })json";
    const auto conflictManifest = R"json({
      "schemaVersion": 2,
      "id": "body-conflict",
      "name": "Body Conflict",
      "priority": 1,
      "enabled": true,
      "replacements": [{
        "source": "mdl_chr_ttmr-cstm-0111_body",
        "part": "body",
        "bundle": "body.bundle"
      }]
    })json";
    const auto hairManifest = R"json({
      "schemaVersion": 2,
      "id": "hair-disabled",
      "name": "Hair Disabled",
      "enabled": false,
      "replacements": [{
        "source": "mdl_chr_ttmr-hair-0023_hair",
        "part": "hair",
        "bundle": "hair.bundle"
      }]
    })json";
    const auto multiManifest = R"json({
      "schemaVersion": 2,
      "id": "multi-target",
      "name": "Multi Target",
      "replacements": [
        {"source": "mdl_chr_ttmr-cstm-0111_body", "part": "body", "bundle": "a.bundle"},
        {"source": "mdl_chr_ttmr-cstm-0112_body", "part": "body", "bundle": "b.bundle"}
      ]
    })json";
    const auto missingManifest = R"json({
      "schemaVersion": 2,
      "id": "missing-bundle",
      "name": "Missing Bundle",
      "replacements": [{
        "source": "mdl_chr_ttmr-cstm-0140_body",
        "part": "body",
        "bundle": "not-found.bundle"
      }]
    })json";

    WriteText(bodyDir / "mod.json", bodyManifest);
    WriteText(bodyDir / "body.bundle", "fixture");
    WriteText(conflictDir / "mod.json", conflictManifest);
    WriteText(conflictDir / "body.bundle", "fixture");
    WriteText(hairDir / "mod.json", hairManifest);
    WriteText(hairDir / "hair.bundle", "fixture");
    WriteText(multiDir / "mod.json", multiManifest);
    WriteText(multiDir / "a.bundle", "fixture");
    WriteText(multiDir / "b.bundle", "fixture");
    WriteText(missingDir / "mod.json", missingManifest);

    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(root);
    GakumasMod::Runtime::Catalog::Refresh();
    GakumasMod::Runtime::Catalog::SetReady(true);

    const auto initial = ReadSnapshot();
    assert(initial.at("mods").size() == 5);
    assert(FindMod(initial, "body-enabled").at("configuredEnabled") == false);
    assert(FindMod(initial, "body-enabled").at("runtimeState") == "conflict_auto_disabled");
    assert(FindMod(initial, "body-enabled").at("conflict").at("withModId") == "body-conflict");
    assert(FindMod(initial, "body-conflict").at("configuredEnabled") == false);
    assert(FindMod(initial, "body-conflict").at("runtimeState") == "conflict_auto_disabled");
    assert(FindMod(initial, "body-conflict").at("conflict").at("withModId") == "body-enabled");
    assert(FindMod(initial, "hair-disabled").at("configuredEnabled") == false);
    assert(FindMod(initial, "hair-disabled").at("runtimeState") == "disabled");
    assert(FindMod(initial, "multi-target").at("manifestState") == "multiple_targets");
    assert(FindMod(initial, "missing-bundle").at("manifestState") == "missing_bundle");

    assert(GakumasMod::Runtime::Catalog::SetModEnabled("hair-disabled", 1) == GMR_OK);
    const auto afterEnable = ReadSnapshot();
    assert(FindMod(afterEnable, "hair-disabled").at("configuredEnabled") == true);
    assert(FindMod(afterEnable, "hair-disabled").at("registeredThisSession") == true);
    assert(FindMod(afterEnable, "hair-disabled").at("runtimeState") == "active");
    assert(FindMod(afterEnable, "hair-disabled").at("restartRequired") == false);

    assert(GakumasMod::Runtime::Catalog::SetModEnabled("body-enabled", 1) == GMR_OK);
    const auto afterFirstEnable = ReadSnapshot();
    assert(FindMod(afterFirstEnable, "body-enabled").at("runtimeState") == "active");
    assert(FindMod(afterFirstEnable, "body-conflict").at("runtimeState") == "disabled");
    assert(FindMod(afterFirstEnable, "body-conflict").at("conflict").is_null());

    assert(GakumasMod::Runtime::Catalog::SetModEnabled("body-conflict", 1)
           == GMR_E_TARGET_CONFLICT);
    const auto afterRejectedEnable = ReadSnapshot();
    assert(FindMod(afterRejectedEnable, "body-enabled").at("runtimeState") == "active");
    assert(FindMod(afterRejectedEnable, "body-enabled").at("configuredEnabled") == true);
    assert(FindMod(afterRejectedEnable, "body-conflict").at("configuredEnabled") == false);
    assert(FindMod(afterRejectedEnable, "body-conflict").at("runtimeState") == "conflict_enable_blocked");
    assert(FindMod(afterRejectedEnable, "body-conflict").at("conflict").at("withModId") == "body-enabled");

    assert(GakumasMod::Runtime::Catalog::SetModEnabled("body-enabled", 0) == GMR_OK);
    const auto afterWinnerDisable = ReadSnapshot();
    assert(FindMod(afterWinnerDisable, "body-enabled").at("runtimeState") == "disabled");
    assert(FindMod(afterWinnerDisable, "body-conflict").at("runtimeState") == "disabled");
    assert(FindMod(afterWinnerDisable, "body-conflict").at("conflict").is_null());

    assert(GakumasMod::Runtime::Catalog::SetModEnabled("body-conflict", 1) == GMR_OK);
    const auto afterSecondEnable = ReadSnapshot();
    assert(FindMod(afterSecondEnable, "body-conflict").at("runtimeState") == "active");
    assert(FindMod(afterSecondEnable, "body-enabled").at("runtimeState") == "disabled");

    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root, ec);
    assert(!ec);
    std::cout << "ModRuntimeCatalogSmoke passed\n";
    return 0;
}

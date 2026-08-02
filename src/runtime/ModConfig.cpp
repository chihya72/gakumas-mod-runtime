#include "ModConfig.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace GakumasMod::Config {
    std::optional<bool> ReadManagerUiEnabled(const std::filesystem::path& configPath) {
        std::ifstream input(configPath, std::ios::binary);
        if (!input) return std::nullopt;
        try {
            const auto config = nlohmann::json::parse(input);
            if (config.is_object() && config.contains("modManagerUi")
                && config["modManagerUi"].is_boolean()) {
                return config["modManagerUi"].get<bool>();
            }
        }
        catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
        return std::nullopt;
    }
}

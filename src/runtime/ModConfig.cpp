#include "ModConfig.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace GakumasMod::Config {
    namespace {
        std::optional<nlohmann::json> ReadConfig(const std::filesystem::path& configPath) {
            std::ifstream input(configPath, std::ios::binary);
            if (!input) return std::nullopt;
            try {
                auto config = nlohmann::json::parse(input);
                if (!config.is_object()) return std::nullopt;
                return config;
            }
            catch (const nlohmann::json::exception&) {
                return std::nullopt;
            }
        }
    }

    std::optional<bool> ReadManagerUiEnabled(const std::filesystem::path& configPath) {
        const auto config = ReadConfig(configPath);
        if (!config) return std::nullopt;
        if (config->contains("modManagerUi") && (*config)["modManagerUi"].is_boolean()) {
            return (*config)["modManagerUi"].get<bool>();
        }
        return std::nullopt;
    }

    std::optional<std::string> ReadLogLevel(const std::filesystem::path& configPath) {
        const auto config = ReadConfig(configPath);
        if (!config) return std::nullopt;
        if (config->contains("logLevel") && (*config)["logLevel"].is_string()) {
            return (*config)["logLevel"].get<std::string>();
        }
        return std::nullopt;
    }
}

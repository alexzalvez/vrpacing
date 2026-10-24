#ifdef _WIN32

#include "config_json.h"

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace vrpacing {
namespace {

using json = nlohmann::json;

std::filesystem::path GetConfigDir() {
    wchar_t* localAppData = nullptr;
    size_t len = 0;
    _wdupenv_s(&localAppData, &len, L"LOCALAPPDATA");
    std::filesystem::path dir = localAppData ? localAppData : L".";
    if (localAppData) {
        free(localAppData);
    }
    dir /= L"VRPacing";
    std::filesystem::create_directories(dir);
    return dir;
}

void ToJson(json& j, const RuntimeConfig& cfg) {
    j = {
        {"enabled", cfg.enabled},
        {"upscaling", {
            {"enabled", cfg.upscaling.enabled},
            {"scale", cfg.upscaling.scale},
            {"sharpness", cfg.upscaling.sharpness},
            {"radius", cfg.upscaling.radius},
            {"maxOutputPixels", cfg.upscaling.maxOutputPixels}
        }},
        {"mask", {
            {"enabled", cfg.mask.enabled},
            {"radiusX", cfg.mask.radiusX},
            {"radiusY", cfg.mask.radiusY},
            {"centerX", cfg.mask.centerX},
            {"centerY", cfg.mask.centerY},
            {"segments", cfg.mask.segments},
            {"flipWinding", cfg.mask.flipWinding}
        }},
        {"pacing", {
            {"enabled", cfg.pacing.enabled},
            {"offsetFps", cfg.pacing.offsetFps},
            {"frameGenDivisor", cfg.pacing.frameGenDivisor},
            {"afrMultiplier", cfg.pacing.afrMultiplier},
            {"hzOverride", cfg.pacing.hzOverride}
        }},
        {"logging", {
            {"level", cfg.logging.level}
        }}
    };
}

RuntimeConfig FromJson(const json& j) {
    RuntimeConfig cfg = DefaultConfig();
    cfg.enabled = j.value("enabled", cfg.enabled);

    if (j.contains("upscaling")) {
        const auto& u = j["upscaling"];
        cfg.upscaling.enabled = u.value("enabled", cfg.upscaling.enabled);
        cfg.upscaling.scale = u.value("scale", cfg.upscaling.scale);
        cfg.upscaling.sharpness = u.value("sharpness", cfg.upscaling.sharpness);
        cfg.upscaling.radius = u.value("radius", cfg.upscaling.radius);
        cfg.upscaling.maxOutputPixels = u.value("maxOutputPixels", cfg.upscaling.maxOutputPixels);
    }
    if (j.contains("mask")) {
        const auto& m = j["mask"];
        cfg.mask.enabled = m.value("enabled", cfg.mask.enabled);
        cfg.mask.radiusX = m.value("radiusX", cfg.mask.radiusX);
        cfg.mask.radiusY = m.value("radiusY", cfg.mask.radiusY);
        cfg.mask.centerX = m.value("centerX", cfg.mask.centerX);
        cfg.mask.centerY = m.value("centerY", cfg.mask.centerY);
        cfg.mask.segments = m.value("segments", cfg.mask.segments);
        cfg.mask.flipWinding = m.value("flipWinding", cfg.mask.flipWinding);
    }
    if (j.contains("pacing")) {
        const auto& p = j["pacing"];
        cfg.pacing.enabled = p.value("enabled", cfg.pacing.enabled);
        cfg.pacing.offsetFps = p.value("offsetFps", cfg.pacing.offsetFps);
        cfg.pacing.frameGenDivisor = p.value("frameGenDivisor", cfg.pacing.frameGenDivisor);
        cfg.pacing.afrMultiplier = p.value("afrMultiplier", cfg.pacing.afrMultiplier);
        cfg.pacing.hzOverride = p.value("hzOverride", cfg.pacing.hzOverride);
    }
    if (j.contains("logging")) {
        const auto& l = j["logging"];
        cfg.logging.level = l.value("level", cfg.logging.level);
    }
    return cfg;
}

} // namespace

std::string GetConfigPath() {
    return (GetConfigDir() / L"config.json").string();
}

RuntimeConfig LoadConfig() {
    const std::filesystem::path path = GetConfigDir() / L"config.json";
    if (!std::filesystem::exists(path)) {
        return DefaultConfig();
    }

    std::ifstream in(path);
    if (!in) {
        return DefaultConfig();
    }

    try {
        json j;
        in >> j;
        return FromJson(j);
    } catch (...) {
        return DefaultConfig();
    }
}

bool SaveConfig(const RuntimeConfig& cfg) {
    const std::filesystem::path path = GetConfigDir() / L"config.json";
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    json j;
    ToJson(j, cfg);
    out << j.dump(2);
    return static_cast<bool>(out);
}

} // namespace vrpacing

#endif

#include "Configuration.h"

#include "Prisma.h"

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace {
    constexpr const char* MOD_DIR = "Data/Viny Mods/Trick Death";
    constexpr const char* SETTINGS_PATH = "Data/Viny Mods/Trick Death/Settings.json";
    constexpr const char* LANGUAGE_PATH = "Data/Viny Mods/Trick Death/Language.json";

    std::unordered_map<std::string, std::string> language;

    bool LoadDocument(const char* path, rapidjson::Document& document) {
        std::ifstream stream(path);
        if (!stream.is_open()) {
            return false;
        }

        rapidjson::IStreamWrapper wrapper(stream);
        document.ParseStream(wrapper);
        return !document.HasParseError() && document.IsObject();
    }

    bool WriteDocument(const char* path, const rapidjson::Document& document) {
        std::ofstream stream(path, std::ios::trunc);
        if (!stream.is_open()) {
            logger::error("Could not write settings file: {}", path);
            return false;
        }

        rapidjson::OStreamWrapper wrapper(stream);
        rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(wrapper);
        document.Accept(writer);
        return true;
    }

    void ClampSettings() {
        Settings::Gameplay.healthPercent = std::clamp(Settings::Gameplay.healthPercent, 1, 100);
        Settings::Gameplay.magickaPercent = std::clamp(Settings::Gameplay.magickaPercent, 0, 100);
        Settings::Gameplay.staminaPercent = std::clamp(Settings::Gameplay.staminaPercent, 0, 100);
        Settings::Gameplay.invulnerabilitySeconds = std::clamp(Settings::Gameplay.invulnerabilitySeconds, 0, 30);
        Settings::UI.backgroundOpacityPercent = std::clamp(Settings::UI.backgroundOpacityPercent, 0, 100);
        Settings::UI.backgroundBlurPixels = std::clamp(Settings::UI.backgroundBlurPixels, 0, 30);
        Settings::UI.scalePercent = std::clamp(Settings::UI.scalePercent, 50, 200);
    }

    bool RenderIntSliderWithInput(const char* label, int* value, int minimum, int maximum) {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(200.0F);
        changed |= ImGui::SliderInt("##slider", value, minimum, maximum);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0F);
        changed |= ImGui::InputInt(label, value);
        const auto clamped = std::clamp(*value, minimum, maximum);
        if (clamped != *value) {
            *value = clamped;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    void ReadBool(const rapidjson::Value& object, const char* key, bool& value) {
        if (object.HasMember(key) && object[key].IsBool()) {
            value = object[key].GetBool();
        }
    }

    void ReadInt(const rapidjson::Value& object, const char* key, int& value) {
        if (object.HasMember(key) && object[key].IsInt()) {
            value = object[key].GetInt();
        }
    }
}

const char* ModMenu::GetLoc(const std::string& key, const char* fallback) {
    const auto it = language.find(key);
    return it == language.end() ? fallback : it->second.c_str();
}

void ModMenu::LoadLanguage() {
    language.clear();
    rapidjson::Document document;
    if (!LoadDocument(LANGUAGE_PATH, document)) {
        return;
    }

    for (auto category = document.MemberBegin(); category != document.MemberEnd(); ++category) {
        if (category->value.IsObject()) {
            for (auto entry = category->value.MemberBegin(); entry != category->value.MemberEnd(); ++entry) {
                if (entry->value.IsString()) {
                    language[std::string(category->name.GetString()) + "." + entry->name.GetString()] = entry->value.GetString();
                }
            }
        } else if (category->value.IsString()) {
            language[category->name.GetString()] = category->value.GetString();
        }
    }
}

void ModMenu::LoadSettings() {
    rapidjson::Document document;
    if (!LoadDocument(SETTINGS_PATH, document)) {
        ClampSettings();
        return;
    }

    if (document.HasMember("gameplay") && document["gameplay"].IsObject()) {
        const auto& gameplay = document["gameplay"];
        ReadBool(gameplay, "enabled", Settings::Gameplay.enabled);
        ReadBool(gameplay, "pauseGameWhileMenuOpen", Settings::Gameplay.pauseGameWhileMenuOpen);
        ReadInt(gameplay, "healthPercent", Settings::Gameplay.healthPercent);
        ReadInt(gameplay, "magickaPercent", Settings::Gameplay.magickaPercent);
        ReadInt(gameplay, "staminaPercent", Settings::Gameplay.staminaPercent);
        ReadInt(gameplay, "invulnerabilitySeconds", Settings::Gameplay.invulnerabilitySeconds);
    }

    if (document.HasMember("ui") && document["ui"].IsObject()) {
        const auto& ui = document["ui"];
        ReadInt(ui, "backgroundOpacityPercent", Settings::UI.backgroundOpacityPercent);
        ReadInt(ui, "backgroundBlurPixels", Settings::UI.backgroundBlurPixels);
        ReadInt(ui, "scalePercent", Settings::UI.scalePercent);
    }

    ClampSettings();
}

void ModMenu::SaveSettings() {
    std::filesystem::create_directories(MOD_DIR);
    ClampSettings();

    rapidjson::Document document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    rapidjson::Value gameplay(rapidjson::kObjectType);
    gameplay.AddMember("enabled", Settings::Gameplay.enabled, allocator);
    gameplay.AddMember("pauseGameWhileMenuOpen", Settings::Gameplay.pauseGameWhileMenuOpen, allocator);
    gameplay.AddMember("healthPercent", Settings::Gameplay.healthPercent, allocator);
    gameplay.AddMember("magickaPercent", Settings::Gameplay.magickaPercent, allocator);
    gameplay.AddMember("staminaPercent", Settings::Gameplay.staminaPercent, allocator);
    gameplay.AddMember("invulnerabilitySeconds", Settings::Gameplay.invulnerabilitySeconds, allocator);
    document.AddMember("gameplay", gameplay, allocator);

    rapidjson::Value ui(rapidjson::kObjectType);
    ui.AddMember("backgroundOpacityPercent", Settings::UI.backgroundOpacityPercent, allocator);
    ui.AddMember("backgroundBlurPixels", Settings::UI.backgroundBlurPixels, allocator);
    ui.AddMember("scalePercent", Settings::UI.scalePercent, allocator);
    document.AddMember("ui", ui, allocator);

    WriteDocument(SETTINGS_PATH, document);
}

void ModMenu::GameplayRender() {
    bool changed = false;
    changed |= ImGui::Checkbox(GetLoc("menu.enabled", "Enable Trick Death"), &Settings::Gameplay.enabled);
    changed |= ImGui::Checkbox(
        GetLoc("menu.pause_game", "Pause game while the death menu is open"),
        &Settings::Gameplay.pauseGameWhileMenuOpen);

    ImGui::Separator();
    changed |= RenderIntSliderWithInput(GetLoc("menu.health", "Respawn health (%)"), &Settings::Gameplay.healthPercent, 1, 100);
    changed |= RenderIntSliderWithInput(GetLoc("menu.magicka", "Respawn magicka (%)"), &Settings::Gameplay.magickaPercent, 0, 100);
    changed |= RenderIntSliderWithInput(GetLoc("menu.stamina", "Respawn stamina (%)"), &Settings::Gameplay.staminaPercent, 0, 100);
    changed |= RenderIntSliderWithInput(
        GetLoc("menu.invulnerability", "Invulnerability after respawn (seconds)"),
        &Settings::Gameplay.invulnerabilitySeconds,
        0,
        30);

    if (changed) {
        SaveSettings();
        Prisma::ApplyUISettings();
    }
}

void ModMenu::UIRender() {
    bool changed = false;
    changed |= RenderIntSliderWithInput(GetLoc("menu.background_opacity", "Background opacity (%)"), &Settings::UI.backgroundOpacityPercent, 0, 100);
    changed |= RenderIntSliderWithInput(GetLoc("menu.background_blur", "Background blur (px)"), &Settings::UI.backgroundBlurPixels, 0, 30);
    changed |= RenderIntSliderWithInput(GetLoc("menu.scale", "Menu scale (%)"), &Settings::UI.scalePercent, 50, 200);

    if (ImGui::Button(GetLoc("menu.reset_ui", "Reset UI settings"))) {
        Settings::UI = {};
        changed = true;
    }

    if (changed) {
        SaveSettings();
        Prisma::ApplyUISettings();
    }
}

void ModMenu::Register() {
    LoadLanguage();
    LoadSettings();

    if (!SKSEMenuFramework::IsInstalled()) {
        logger::warn("SKSE Menu Framework not found. Settings will use saved values/defaults.");
        return;
    }

    SKSEMenuFramework::SetSection(GetLoc("menu.section", "Trick Death"));
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.gameplay", "Gameplay"), GameplayRender);
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.ui", "UI"), UIRender);
}

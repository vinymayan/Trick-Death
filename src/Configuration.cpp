#include "Configuration.h"

#include "APIDebugMenu.h"
#include "CheckpointManager.h"
#include "CurrentSaveManager.h"
#include "IntegrationEvents.h"
#include "Prisma.h"
#include "RespawnPolicyManager.h"
#include "TextManager.h"

#include <ClibUtil/editorID.hpp>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace {
    constexpr const char* MOD_DIR = "Data/Viny Mods/Trick Death";
    constexpr const char* LEGACY_SETTINGS_PATH = "Data/Viny Mods/Trick Death/Settings.json";
    constexpr const char* GAMEPLAY_SETTINGS_PATH = "Data/Viny Mods/Trick Death/GameplaySettings.json";
    constexpr const char* UI_SETTINGS_PATH = "Data/Viny Mods/Trick Death/UISettings.json";
    constexpr const char* LEGACY_LANGUAGE_PATH = "Data/Viny Mods/Trick Death/Language.json";
    constexpr const char* LOCALIZATION_DIR = "Data/Viny Mods/Trick Death/Localization";

    std::unordered_map<std::string, std::string> language;
    std::unordered_map<std::string, std::vector<std::string>> languageLists;

    struct GlobalOption {
        RE::FormID formID{ 0 };
        std::string label;
    };

    std::vector<GlobalOption> globalOptions;
    std::map<std::string, std::string> globalSearchText;

    const RE::TESFile* GetMasterFile(RE::TESForm* form) {
        if (!form) {
            return nullptr;
        }
        const auto formID = form->GetFormID();
        const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return nullptr;
        }
        if (modIndex == 0xFE) {
            const auto lightIndex = static_cast<std::uint16_t>((formID >> 12) & 0xFFF);
            return dataHandler->LookupLoadedLightModByIndex(lightIndex);
        }
        return dataHandler->LookupLoadedModByIndex(modIndex);
    }

    std::string NormalizeFormID(RE::TESForm* form) {
        if (!form) {
            return {};
        }
        const auto formID = form->GetFormID();
        const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
        if (modIndex == 0xFF) {
            return fmt::format("{:X}", formID);
        }
        const auto file = GetMasterFile(form);
        if (!file) {
            return fmt::format("{:X}", formID);
        }
        const auto localID = formID & 0x00FFFFFF;
        if (modIndex == 0xFE) {
            return fmt::format("{}|{:X}", file->GetFilename(), localID & 0xFFF);
        }
        return fmt::format("{}|{:X}", file->GetFilename(), localID);
    }

    RE::FormID FormIDFromString(std::string_view text) {
        if (text.empty()) {
            return 0;
        }
        const std::string value(text);
        if (const auto form = RE::TESForm::LookupByEditorID(value)) {
            return form->GetFormID();
        }
        const auto separator = value.find('|');
        try {
            if (separator != std::string::npos) {
                const auto plugin = value.substr(0, separator);
                const auto localID = static_cast<RE::FormID>(
                    std::stoul(value.substr(separator + 1), nullptr, 16));
                const auto dataHandler = RE::TESDataHandler::GetSingleton();
                return dataHandler ? dataHandler->LookupFormID(localID, plugin) : 0;
            }
            return static_cast<RE::FormID>(std::stoul(value, nullptr, 16));
        } catch (...) {
            return 0;
        }
    }

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
        const auto clampNumeric = [](Settings::NumericValueSetting& setting, int minimum, int maximum) {
            setting.flatValue = std::clamp(setting.flatValue, minimum, maximum);
            setting.source = static_cast<Settings::ValueSource>(
                std::clamp(static_cast<int>(setting.source), 0, 2));
        };
        clampNumeric(Settings::Gameplay.healthPercent, 1, 100);
        clampNumeric(Settings::Gameplay.magickaPercent, 0, 100);
        clampNumeric(Settings::Gameplay.staminaPercent, 0, 100);
        clampNumeric(Settings::Gameplay.invulnerabilitySeconds, 0, 30);
        Settings::Gameplay.defeatPose = std::clamp(
            Settings::Gameplay.defeatPose,
            static_cast<int>(Settings::DefeatPose::kBleedout),
            static_cast<int>(Settings::DefeatPose::kRagdoll));
        Settings::UI.backgroundOpacityPercent = std::clamp(Settings::UI.backgroundOpacityPercent, 0, 100);
        Settings::UI.backgroundBlurPixels = std::clamp(Settings::UI.backgroundBlurPixels, 0, 30);
        Settings::UI.scalePercent = std::clamp(Settings::UI.scalePercent, 50, 200);
        Settings::UI.titleTextSizePercent = std::clamp(Settings::UI.titleTextSizePercent, 50, 200);
        Settings::UI.backgroundTextSizePercent = std::clamp(Settings::UI.backgroundTextSizePercent, 50, 200);
        for (auto* style : {
                 &Settings::UI.respawnHere,
                 &Settings::UI.lastSleep,
                 &Settings::UI.lastCheckpoint,
                 &Settings::UI.reloadSave }) {
            style->textSizePercent = std::clamp(style->textSizePercent, 50, 200);
            style->buttonScalePercent = std::clamp(style->buttonScalePercent, 50, 200);
        }
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

    bool RenderStringInput(const char* label, std::string& value) {
        char buffer[128]{};
        strcpy_s(buffer, value.c_str());
        ImGui::SetNextItemWidth(300.0F);
        if (!ImGui::InputText(label, buffer, sizeof(buffer))) {
            return false;
        }
        value = buffer;
        return true;
    }

    bool DrawGlobalDropdown(const char* label, RE::FormID& selectedFormID) {
        const auto selected = std::ranges::find_if(globalOptions, [selectedFormID](const auto& option) {
            return option.formID == selectedFormID;
        });
        const auto unresolvedPreview = selectedFormID == 0 ? std::string{} :
            fmt::format("Unresolved [{:08X}]", selectedFormID);
        const auto preview = selected != globalOptions.end() ? selected->label.c_str() :
            selectedFormID == 0 ? ModMenu::GetLoc("menu.value_global_none", "None") :
                                  unresolvedPreview.c_str();

        bool changed = false;
        ImGui::SetNextItemWidth(360.0F);
        if (!ImGui::BeginCombo(label, preview)) {
            return false;
        }

        auto& search = globalSearchText[label];
        char searchBuffer[256]{};
        strcpy_s(searchBuffer, search.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText("##global_search", searchBuffer, sizeof(searchBuffer))) {
            search = searchBuffer;
        }
        ImGui::Separator();

        const auto lower = [](std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        };
        const auto filter = lower(search);
        if (ImGui::Selectable(ModMenu::GetLoc("menu.value_global_none", "None"), selectedFormID == 0)) {
            selectedFormID = 0;
            search.clear();
            changed = true;
        }
        for (const auto& option : globalOptions) {
            if (!filter.empty() && lower(option.label).find(filter) == std::string::npos) {
                continue;
            }
            const bool isSelected = option.formID == selectedFormID;
            if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                selectedFormID = option.formID;
                search.clear();
                changed = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
        return changed;
    }

    bool DrawNumericValueSetting(
        const char* label,
        Settings::NumericValueSetting& setting,
        int minimum,
        int maximum)
    {
        bool changed = false;
        ImGui::PushID(label);
        const char* sources[] = {
            ModMenu::GetLoc("menu.value_source_flat", "Flat"),
            ModMenu::GetLoc("menu.value_source_global", "Global"),
            ModMenu::GetLoc("menu.value_source_actor_value", "Actor Value")
        };
        int source = static_cast<int>(setting.source);
        if (ImGui::Combo(ModMenu::GetLoc("menu.value_source", "Value source"), &source, sources, 3)) {
            setting.source = static_cast<Settings::ValueSource>(std::clamp(source, 0, 2));
            changed = true;
        }

        switch (setting.source) {
        case Settings::ValueSource::kGlobal:
            changed |= DrawGlobalDropdown(
                ModMenu::GetLoc("menu.value_global", "Global value"),
                setting.global);
            break;
        case Settings::ValueSource::kActorValue:
            changed |= RenderStringInput(
                ModMenu::GetLoc("menu.value_actor_value", "Actor Value"),
                setting.actorValue);
            break;
        case Settings::ValueSource::kFlat:
        default:
            changed |= RenderIntSliderWithInput(label, &setting.flatValue, minimum, maximum);
            break;
        }

        const auto resolved = Settings::ResolveNumericValue(
            setting,
            RE::PlayerCharacter::GetSingleton(),
            minimum,
            maximum);
        ImGui::TextDisabled(
            "%s: %d",
            ModMenu::GetLoc("menu.value_resolved", "Resolved value"),
            resolved);
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

    void ReadGlobalReference(
        const rapidjson::Value& object,
        Settings::NumericValueSetting& setting)
    {
        if (object.HasMember("globalEditorID") && object["globalEditorID"].IsString()) {
            if (const auto form = RE::TESForm::LookupByEditorID<RE::TESGlobal>(
                    object["globalEditorID"].GetString())) {
                setting.global = form->GetFormID();
                return;
            }
        }
        if (!object.HasMember("global")) {
            return;
        }
        const auto& global = object["global"];
        if (global.IsString()) {
            setting.global = FormIDFromString(global.GetString());
        } else if (global.IsUint()) {
            setting.global = global.GetUint();
        }
    }

    void ReadNumericValueSetting(
        const rapidjson::Value& gameplay,
        const char* key,
        const char* legacyKey,
        Settings::NumericValueSetting& setting)
    {
        if (!gameplay.HasMember(key) || !gameplay[key].IsObject()) {
            ReadInt(gameplay, legacyKey, setting.flatValue);
            return;
        }
        const auto& object = gameplay[key];
        ReadInt(object, "flatValue", setting.flatValue);
        if (object.HasMember("source") && object["source"].IsInt()) {
            setting.source = static_cast<Settings::ValueSource>(
                std::clamp(object["source"].GetInt(), 0, 2));
        }
        if (object.HasMember("actorValue") && object["actorValue"].IsString()) {
            setting.actorValue = object["actorValue"].GetString();
        }
        ReadGlobalReference(object, setting);
    }

    rapidjson::Value MakeNumericValueSetting(
        const Settings::NumericValueSetting& setting,
        rapidjson::Document::AllocatorType& allocator)
    {
        rapidjson::Value object(rapidjson::kObjectType);
        object.AddMember("source", static_cast<int>(setting.source), allocator);
        object.AddMember("flatValue", setting.flatValue, allocator);
        rapidjson::Value actorValue;
        actorValue.SetString(setting.actorValue.c_str(), allocator);
        object.AddMember("actorValue", actorValue, allocator);

        if (const auto form = RE::TESForm::LookupByID<RE::TESGlobal>(setting.global)) {
            const auto normalized = NormalizeFormID(form);
            rapidjson::Value global;
            global.SetString(normalized.c_str(), allocator);
            object.AddMember("global", global, allocator);
            try {
                const auto editorID = clib_util::editorID::get_editorID(form);
                if (!editorID.empty()) {
                    rapidjson::Value editorIDValue;
                    editorIDValue.SetString(editorID.c_str(), allocator);
                    object.AddMember("globalEditorID", editorIDValue, allocator);
                }
            } catch (...) {
                logger::warn("Could not read the EditorID for Global {:08X} while saving settings.", setting.global);
            }
        } else {
            object.AddMember("global", setting.global, allocator);
        }
        return object;
    }

    void ReadActionStyle(
        const rapidjson::Value& actions,
        const char* key,
        Settings::ActionStyle& style)
    {
        if (!actions.HasMember(key) || !actions[key].IsObject()) {
            return;
        }
        const auto& object = actions[key];
        ReadInt(object, "textSizePercent", style.textSizePercent);
        ReadInt(object, "buttonScalePercent", style.buttonScalePercent);
    }

    void ReadGameplaySettings(const rapidjson::Value& gameplay) {
        ReadBool(gameplay, "enabled", Settings::Gameplay.enabled);
        ReadBool(gameplay, "pauseGameWhileMenuOpen", Settings::Gameplay.pauseGameWhileMenuOpen);
        if (gameplay.HasMember("defeatPose") && gameplay["defeatPose"].IsInt()) {
            Settings::Gameplay.defeatPose = gameplay["defeatPose"].GetInt();
        } else {
            bool legacyRagdoll = false;
            ReadBool(gameplay, "useRagdollInsteadOfBleedout", legacyRagdoll);
            if (legacyRagdoll) {
                Settings::Gameplay.defeatPose = static_cast<int>(Settings::DefeatPose::kRagdoll);
            }
        }
        ReadNumericValueSetting(
            gameplay, "health", "healthPercent", Settings::Gameplay.healthPercent);
        ReadNumericValueSetting(
            gameplay, "magicka", "magickaPercent", Settings::Gameplay.magickaPercent);
        ReadNumericValueSetting(
            gameplay, "stamina", "staminaPercent", Settings::Gameplay.staminaPercent);
        ReadNumericValueSetting(
            gameplay, "invulnerability", "invulnerabilitySeconds", Settings::Gameplay.invulnerabilitySeconds);
    }

    void ReadUISettings(const rapidjson::Value& ui) {
        ReadInt(ui, "backgroundOpacityPercent", Settings::UI.backgroundOpacityPercent);
        ReadInt(ui, "backgroundBlurPixels", Settings::UI.backgroundBlurPixels);
        ReadInt(ui, "scalePercent", Settings::UI.scalePercent);
        ReadInt(ui, "titleTextSizePercent", Settings::UI.titleTextSizePercent);
        ReadInt(ui, "backgroundTextSizePercent", Settings::UI.backgroundTextSizePercent);
        if (ui.HasMember("actions") && ui["actions"].IsObject()) {
            const auto& actions = ui["actions"];
            ReadActionStyle(actions, "respawn_here", Settings::UI.respawnHere);
            ReadActionStyle(actions, "respawn_last_sleep", Settings::UI.lastSleep);
            ReadActionStyle(actions, "respawn_checkpoint", Settings::UI.lastCheckpoint);
            ReadActionStyle(actions, "reload_save", Settings::UI.reloadSave);
        }
    }

    void AddActionStyle(
        rapidjson::Value& actions,
        const char* key,
        const Settings::ActionStyle& style,
        rapidjson::Document::AllocatorType& allocator)
    {
        rapidjson::Value object(rapidjson::kObjectType);
        object.AddMember("textSizePercent", style.textSizePercent, allocator);
        object.AddMember("buttonScalePercent", style.buttonScalePercent, allocator);
        rapidjson::Value name;
        name.SetString(key, allocator);
        actions.AddMember(name, object, allocator);
    }

    void FlattenLocalization(
        const rapidjson::Value& value,
        const std::string& parentKey)
    {
        if (value.IsObject()) {
            for (auto entry = value.MemberBegin(); entry != value.MemberEnd(); ++entry) {
                const std::string key = parentKey.empty() ?
                    entry->name.GetString() : parentKey + "." + entry->name.GetString();
                FlattenLocalization(entry->value, key);
            }
        } else if (value.IsString()) {
            language[parentKey] = value.GetString();
        } else if (value.IsArray()) {
            std::vector<std::string> values;
            for (const auto& item : value.GetArray()) {
                if (item.IsString()) {
                    values.emplace_back(item.GetString());
                }
            }
            if (!values.empty()) {
                languageLists[parentKey] = std::move(values);
            }
        }
    }

    bool LoadLocalizationFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        auto content = buffer.str();
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }
        rapidjson::Document document;
        document.Parse(content.c_str());
        if (document.HasParseError() || !document.IsObject()) {
            logger::warn("Could not parse Trick Death localization file '{}'.", path.string());
            return false;
        }
        FlattenLocalization(document, "");
        return true;
    }
}

int Settings::ResolveNumericValue(
    const NumericValueSetting& setting,
    RE::Actor* actor,
    int minimum,
    int maximum)
{
    float value = static_cast<float>(setting.flatValue);
    switch (setting.source) {
    case ValueSource::kGlobal:
        if (const auto global = RE::TESForm::LookupByID<RE::TESGlobal>(setting.global)) {
            value = global->value;
        }
        break;
    case ValueSource::kActorValue:
        if (actor) {
            const auto actorValue = RE::ActorValueList::LookupActorValueByName(
                setting.actorValue.c_str());
            if (actorValue != RE::ActorValue::kNone) {
                if (const auto owner = actor->AsActorValueOwner()) {
                    value = owner->GetActorValue(actorValue);
                }
            }
        }
        break;
    case ValueSource::kFlat:
    default:
        break;
    }

    if (!std::isfinite(value)) {
        value = static_cast<float>(setting.flatValue);
    }
    return std::clamp(static_cast<int>(value), minimum, maximum);
}

void ModMenu::RefreshGlobalList() {
    globalOptions.clear();
    const auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("Could not populate Global settings list: TESDataHandler is unavailable.");
        return;
    }

    const auto& forms = dataHandler->GetFormArray<RE::TESGlobal>();
    globalOptions.reserve(forms.size());
    for (const auto form : forms) {
        if (!form || form->IsDeleted() || form->IsIgnored()) {
            continue;
        }
        try {
            const auto editorID = clib_util::editorID::get_editorID(form);
            const auto displayName = editorID.empty() ? "Global" : editorID;
            globalOptions.push_back({
                form->GetFormID(),
                fmt::format("{} [{:08X}]", displayName, form->GetFormID())
            });
        } catch (const std::exception& error) {
            logger::warn(
                "Could not add Global {:08X} to the settings list: {}",
                form->GetFormID(),
                error.what());
        } catch (...) {
            logger::warn(
                "Could not add Global {:08X} to the settings list due to an unknown error.",
                form->GetFormID());
        }
    }
    std::ranges::sort(globalOptions, {}, &GlobalOption::label);
    logger::info("Loaded {} Global forms for typed gameplay settings.", globalOptions.size());
}

const char* ModMenu::GetLoc(const std::string& key, const char* fallback) {
    const auto it = language.find(key);
    return it == language.end() ? fallback : it->second.c_str();
}

void ModMenu::LoadLanguage() {
    language.clear();
    languageLists.clear();

    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (std::filesystem::exists(LOCALIZATION_DIR, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(LOCALIZATION_DIR, error)) {
            if (!error && entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(files);
    for (const auto& path : files) {
        LoadLocalizationFile(path);
    }

    if (language.empty() && languageLists.empty()) {
        LoadLocalizationFile(LEGACY_LANGUAGE_PATH);
    }
    logger::info(
        "Loaded Trick Death localization: files={}, strings={}, lists={}.",
        files.size(),
        language.size(),
        languageLists.size());
}

std::vector<std::string> ModMenu::GetLocList(const std::string& key) {
    const auto it = languageLists.find(key);
    return it == languageLists.end() ? std::vector<std::string>{} : it->second;
}

void ModMenu::LoadSettings() {
    rapidjson::Document gameplayDocument;
    bool loadedGameplay = false;
    if (LoadDocument(GAMEPLAY_SETTINGS_PATH, gameplayDocument)) {
        ReadGameplaySettings(gameplayDocument);
        loadedGameplay = true;
    }

    rapidjson::Document uiDocument;
    bool loadedUI = false;
    if (LoadDocument(UI_SETTINGS_PATH, uiDocument)) {
        ReadUISettings(uiDocument);
        loadedUI = true;
    }

    if (!loadedGameplay || !loadedUI) {
        rapidjson::Document legacyDocument;
        if (LoadDocument(LEGACY_SETTINGS_PATH, legacyDocument)) {
            if (!loadedGameplay && legacyDocument.HasMember("gameplay") &&
                legacyDocument["gameplay"].IsObject()) {
                ReadGameplaySettings(legacyDocument["gameplay"]);
            }
            if (!loadedUI && legacyDocument.HasMember("ui") && legacyDocument["ui"].IsObject()) {
                ReadUISettings(legacyDocument["ui"]);
            }
        }
    }

    ClampSettings();
}

void ModMenu::SaveSettings() {
    SaveGameplaySettings();
    SaveUISettings();
}

void ModMenu::SaveGameplaySettings() {
    std::filesystem::create_directories(MOD_DIR);
    ClampSettings();

    rapidjson::Document document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    document.AddMember("enabled", Settings::Gameplay.enabled, allocator);
    document.AddMember("pauseGameWhileMenuOpen", Settings::Gameplay.pauseGameWhileMenuOpen, allocator);
    document.AddMember("defeatPose", Settings::Gameplay.defeatPose, allocator);
    document.AddMember("healthPercent", Settings::Gameplay.healthPercent.flatValue, allocator);
    document.AddMember("magickaPercent", Settings::Gameplay.magickaPercent.flatValue, allocator);
    document.AddMember("staminaPercent", Settings::Gameplay.staminaPercent.flatValue, allocator);
    document.AddMember(
        "invulnerabilitySeconds",
        Settings::Gameplay.invulnerabilitySeconds.flatValue,
        allocator);
    document.AddMember(
        "health",
        MakeNumericValueSetting(Settings::Gameplay.healthPercent, allocator),
        allocator);
    document.AddMember(
        "magicka",
        MakeNumericValueSetting(Settings::Gameplay.magickaPercent, allocator),
        allocator);
    document.AddMember(
        "stamina",
        MakeNumericValueSetting(Settings::Gameplay.staminaPercent, allocator),
        allocator);
    document.AddMember(
        "invulnerability",
        MakeNumericValueSetting(Settings::Gameplay.invulnerabilitySeconds, allocator),
        allocator);

    WriteDocument(GAMEPLAY_SETTINGS_PATH, document);
}

void ModMenu::SaveUISettings() {
    std::filesystem::create_directories(MOD_DIR);
    ClampSettings();

    rapidjson::Document document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    document.AddMember("backgroundOpacityPercent", Settings::UI.backgroundOpacityPercent, allocator);
    document.AddMember("backgroundBlurPixels", Settings::UI.backgroundBlurPixels, allocator);
    document.AddMember("scalePercent", Settings::UI.scalePercent, allocator);
    document.AddMember("titleTextSizePercent", Settings::UI.titleTextSizePercent, allocator);
    document.AddMember("backgroundTextSizePercent", Settings::UI.backgroundTextSizePercent, allocator);
    rapidjson::Value actions(rapidjson::kObjectType);
    AddActionStyle(actions, "respawn_here", Settings::UI.respawnHere, allocator);
    AddActionStyle(actions, "respawn_last_sleep", Settings::UI.lastSleep, allocator);
    AddActionStyle(actions, "respawn_checkpoint", Settings::UI.lastCheckpoint, allocator);
    AddActionStyle(actions, "reload_save", Settings::UI.reloadSave, allocator);
    document.AddMember("actions", actions, allocator);

    WriteDocument(UI_SETTINGS_PATH, document);
}

void ModMenu::GameplayRender() {
    bool changed = false;
    changed |= ImGui::Checkbox(GetLoc("menu.enabled", "Enable Trick Death"), &Settings::Gameplay.enabled);
    changed |= ImGui::Checkbox(
        GetLoc("menu.pause_game", "Pause game while the death menu is open"),
        &Settings::Gameplay.pauseGameWhileMenuOpen);
    const char* defeatPoses[] = {
        GetLoc("menu.pose_bleedout", "Bleedout"),
        GetLoc("menu.pose_ragdoll", "Ragdoll")
    };
    changed |= ImGui::Combo(
        GetLoc("menu.defeat_pose", "Defeat pose"),
        &Settings::Gameplay.defeatPose,
        defeatPoses,
        static_cast<int>(std::size(defeatPoses)));

    ImGui::Separator();
    changed |= DrawNumericValueSetting(
        GetLoc("menu.health", "Respawn health (%)"),
        Settings::Gameplay.healthPercent,
        1,
        100);
    changed |= DrawNumericValueSetting(
        GetLoc("menu.magicka", "Respawn magicka (%)"),
        Settings::Gameplay.magickaPercent,
        0,
        100);
    changed |= DrawNumericValueSetting(
        GetLoc("menu.stamina", "Respawn stamina (%)"),
        Settings::Gameplay.staminaPercent,
        0,
        100);
    changed |= DrawNumericValueSetting(
        GetLoc("menu.invulnerability", "Invulnerability after respawn (seconds)"),
        Settings::Gameplay.invulnerabilitySeconds,
        0,
        30);

    if (changed) {
        SaveGameplaySettings();
        Prisma::ApplyUISettings();
    }
}

void ModMenu::UIRender() {
    bool changed = false;
    changed |= RenderIntSliderWithInput(GetLoc("menu.background_opacity", "Background opacity (%)"), &Settings::UI.backgroundOpacityPercent, 0, 100);
    changed |= RenderIntSliderWithInput(GetLoc("menu.background_blur", "Background blur (px)"), &Settings::UI.backgroundBlurPixels, 0, 30);
    changed |= RenderIntSliderWithInput(GetLoc("menu.scale", "Menu scale (%)"), &Settings::UI.scalePercent, 50, 200);
    changed |= RenderIntSliderWithInput(
        GetLoc("menu.title_text_size", "Defeated title text size (%)"),
        &Settings::UI.titleTextSizePercent,
        50,
        200);
    changed |= RenderIntSliderWithInput(
        GetLoc("menu.background_text_size", "Death message text size (%)"),
        &Settings::UI.backgroundTextSizePercent,
        50,
        200);

    const auto renderActionStyle = [&](const char* label, Settings::ActionStyle& style) {
        if (!ImGui::CollapsingHeader(label)) {
            return;
        }
        ImGui::PushID(label);
        ImGui::Indent();
        changed |= RenderIntSliderWithInput(
            GetLoc("menu.action_text_size", "Option text size (%)"),
            &style.textSizePercent,
            50,
            200);
        changed |= RenderIntSliderWithInput(
            GetLoc("menu.action_button_scale", "Button scale (%)"),
            &style.buttonScalePercent,
            50,
            200);
        ImGui::Unindent();
        ImGui::PopID();
    };
    renderActionStyle(GetLoc("menu.style_respawn_here", "Respawn here style"), Settings::UI.respawnHere);
    renderActionStyle(GetLoc("menu.style_last_sleep", "Last sleep style"), Settings::UI.lastSleep);
    renderActionStyle(GetLoc("menu.style_checkpoint", "Checkpoint style"), Settings::UI.lastCheckpoint);
    renderActionStyle(GetLoc("menu.style_reload", "Reload save style"), Settings::UI.reloadSave);

    if (ImGui::Button(GetLoc("menu.reset_ui", "Reset UI settings"))) {
        Settings::UI = {};
        changed = true;
    }

    if (changed) {
        SaveUISettings();
        Prisma::ApplyUISettings();
    }
}

void ModMenu::DiagnosticsRender() {
    const auto evaluation = RespawnPolicyManager::Evaluate();
    const auto sleep = CheckpointManager::GetLastSleepInfo();
    const auto checkpoint = CheckpointManager::GetCheckpointInfo();
    const auto policies = RespawnPolicyManager::GetPolicies();
    const auto overrides = TextManager::GetOverrides();
    const auto variables = TextManager::GetVariables();
    const auto lastEvent = IntegrationEvents::GetLastEventDescription();
    const auto currentSave = CurrentSaveManager::GetCurrentSaveName();

    ImGui::Text("Available respawns: 0x%02X", evaluation.availableMask);
    ImGui::Text("Blocked respawns: 0x%02X", evaluation.blockedMask);
    ImGui::Text("Trick Death disabled here: %s", evaluation.trickDeathDisabled ? "yes" : "no");
    ImGui::Text("Reload Save target: %s", currentSave.empty() ? "unavailable" : currentSave.c_str());
    ImGui::Separator();
    ImGui::Text(
        "Last sleep: %s | marker=%08X | cell=%08X | location=%08X",
        sleep.active ? sleep.name.c_str() : "unavailable",
        sleep.markerFormID,
        sleep.cellFormID,
        sleep.locationFormID);
    ImGui::Text(
        "External checkpoint: %s | marker=%08X | owner=%08X | cell=%08X | location=%08X",
        checkpoint.active ? checkpoint.name.c_str() : "unavailable",
        checkpoint.markerFormID,
        checkpoint.ownerFormID,
        checkpoint.cellFormID,
        checkpoint.locationFormID);
    ImGui::Separator();
    ImGui::Text("Policies: %zu", policies.size());
    for (std::size_t index = 0; index < policies.size(); ++index) {
        const auto& policy = policies[index];
        ImGui::BulletText(
            "#%zu owner=%08X area=%08X blocked=0x%02X persistent=%s",
            index + 1,
            policy.ownerFormID,
            policy.areaFormID,
            policy.blockedMask,
            policy.persistent ? "yes" : "no");
    }
    ImGui::Text("Text overrides: %zu | variables: %zu", overrides.size(), variables.size());
    for (const auto& item : overrides) {
        ImGui::BulletText(
            "%s owner=%08X priority=%d persistent=%s",
            item.slot.c_str(),
            item.ownerFormID,
            item.priority,
            item.persistent ? "yes" : "no");
    }
    ImGui::Separator();
    ImGui::TextWrapped("Last lifecycle event: %s", lastEvent.c_str());

}

void ModMenu::Register(bool loadSettings) {
    LoadLanguage();
    if (loadSettings) {
        RefreshGlobalList();
        LoadSettings();
    }

    if (!SKSEMenuFramework::IsInstalled()) {
        logger::warn("SKSE Menu Framework not found. Settings will use saved values/defaults.");
        return;
    }

    SKSEMenuFramework::SetSection(GetLoc("menu.section", "Trick Death"));
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.gameplay", "Gameplay"), GameplayRender);
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.ui", "UI"), UIRender);
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.diagnostics", "Diagnostics"), DiagnosticsRender);
    SKSEMenuFramework::AddSectionItem(GetLoc("menu.api_debug", "Debug"), APIDebugMenu::Render);
}

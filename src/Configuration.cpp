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

    struct ResourceOption {
        RE::FormID formID{ 0 };
        std::string editorID;
        std::string normalizedFormID;
        std::string name;
        std::string pluginName;
        std::string formType;
        std::string label;
        std::string searchText;
    };

    struct ResourcePickerState {
        std::string search;
        int pluginIndex{ 0 };
        int typeIndex{ 0 };
    };

    std::vector<GlobalOption> globalOptions;
    std::vector<ResourceOption> resourceOptions;
    std::vector<std::string> resourcePlugins{ "All" };
    std::vector<std::string> resourceTypes{ "All" };
    std::map<std::string, std::string> globalSearchText;
    std::map<std::string, ResourcePickerState> resourcePickerStates;
    std::map<std::string, std::string> searchableComboFilters;

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
        for (auto* cost : {
                 &Settings::Gameplay.respawnHereCost,
                 &Settings::Gameplay.lastCheckpointCost,
                 &Settings::Gameplay.lastSleepCost }) {
            cost->quantity = std::clamp(cost->quantity, 1, 999999);
            cost->action = static_cast<Settings::ResourceAction>(
                std::clamp(static_cast<int>(cost->action), 0, 1));
        }
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

    std::string ToLower(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    void SetFixedComboPopupWidth(const float width) {
        const auto* style = ImGui::GetStyle();
        const float paddingY = style ? style->WindowPadding.y : 8.0F;
        const float maxHeight = ImGui::GetTextLineHeightWithSpacing() * 12.0F + paddingY * 2.0F;
        ImGui::SetNextWindowSizeConstraints({ width, 0.0F }, { width, maxHeight });
    }

    bool DrawSearchableStringCombo(
        const char* label,
        const std::string& stateKey,
        int& selectedIndex,
        const std::vector<std::string>& items,
        const float popupWidth)
    {
        if (items.empty()) {
            return false;
        }

        selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(items.size()) - 1);
        ImGui::SetNextItemWidth(-1.0F);
        SetFixedComboPopupWidth(popupWidth);
        if (!ImGui::BeginCombo(label, items[static_cast<std::size_t>(selectedIndex)].c_str())) {
            return false;
        }

        auto& filter = searchableComboFilters[stateKey];
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        char searchBuffer[256]{};
        strcpy_s(searchBuffer, filter.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        const auto searchLabel = fmt::format(
            "{}##{}_search",
            ModMenu::GetLoc("menu.resource_filter_placeholder", "Filter..."),
            stateKey);
        if (ImGui::InputText(searchLabel.c_str(), searchBuffer, sizeof(searchBuffer))) {
            filter = searchBuffer;
        }
        ImGui::Separator();

        const auto search = ToLower(filter);
        bool changed = false;
        bool anyVisible = false;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (!search.empty() && ToLower(items[index]).find(search) == std::string::npos) {
                continue;
            }
            anyVisible = true;
            if (ImGui::Selectable(items[index].c_str(), selectedIndex == static_cast<int>(index))) {
                selectedIndex = static_cast<int>(index);
                filter.clear();
                changed = true;
            }
        }
        if (!anyVisible) {
            ImGui::TextDisabled("%s", ModMenu::GetLoc("menu.resource_no_results", "No matching options."));
        }
        ImGui::EndCombo();
        return changed;
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
        SetFixedComboPopupWidth(360.0F);
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

        const auto filter = ToLower(search);
        if (ImGui::Selectable(ModMenu::GetLoc("menu.value_global_none", "None"), selectedFormID == 0)) {
            selectedFormID = 0;
            search.clear();
            changed = true;
        }
        for (const auto& option : globalOptions) {
            if (!filter.empty() && ToLower(option.label).find(filter) == std::string::npos) {
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

    bool DrawResourceDropdown(
        const char* label,
        const char* stateKey,
        RE::FormID& selectedFormID)
    {
        const auto selected = std::ranges::find_if(resourceOptions, [selectedFormID](const auto& option) {
            return option.formID == selectedFormID;
        });
        const auto unresolvedPreview = selectedFormID == 0 ? std::string{} :
            fmt::format("Unresolved [{:08X}]", selectedFormID);
        const auto preview = selected != resourceOptions.end() ? selected->label.c_str() :
            selectedFormID == 0 ? ModMenu::GetLoc("menu.resource_none", "None") :
                                  unresolvedPreview.c_str();

        bool changed = false;
        ImGui::SetNextItemWidth(360.0F);
        SetFixedComboPopupWidth(520.0F);
        if (!ImGui::BeginCombo(label, preview)) {
            return false;
        }

        auto& pickerState = resourcePickerStates[stateKey];
        char searchBuffer[256]{};
        strcpy_s(searchBuffer, pickerState.search.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        const auto searchLabel = fmt::format(
            "{}##resource_search",
            ModMenu::GetLoc("menu.resource_filter_placeholder", "Filter..."));
        if (ImGui::InputText(searchLabel.c_str(), searchBuffer, sizeof(searchBuffer))) {
            pickerState.search = searchBuffer;
        }

        DrawSearchableStringCombo(
            ModMenu::GetLoc("menu.resource_filter_plugin", "Plugin"),
            std::string(stateKey) + ":plugin",
            pickerState.pluginIndex,
            resourcePlugins,
            420.0F);
        DrawSearchableStringCombo(
            ModMenu::GetLoc("menu.resource_filter_type", "Form type"),
            std::string(stateKey) + ":type",
            pickerState.typeIndex,
            resourceTypes,
            420.0F);
        ImGui::Separator();

        pickerState.pluginIndex = std::clamp(
            pickerState.pluginIndex,
            0,
            static_cast<int>(resourcePlugins.size()) - 1);
        pickerState.typeIndex = std::clamp(
            pickerState.typeIndex,
            0,
            static_cast<int>(resourceTypes.size()) - 1);
        const auto filter = ToLower(pickerState.search);
        const auto& pluginFilter = resourcePlugins[static_cast<std::size_t>(pickerState.pluginIndex)];
        const auto& typeFilter = resourceTypes[static_cast<std::size_t>(pickerState.typeIndex)];
        if (ImGui::Selectable(ModMenu::GetLoc("menu.resource_none", "None"), selectedFormID == 0)) {
            selectedFormID = 0;
            pickerState.search.clear();
            changed = true;
        }

        std::vector<const ResourceOption*> visibleRows;
        visibleRows.reserve(resourceOptions.size());
        for (const auto& option : resourceOptions) {
            if (pickerState.pluginIndex != 0 && option.pluginName != pluginFilter) {
                continue;
            }
            if (pickerState.typeIndex != 0 && option.formType != typeFilter) {
                continue;
            }
            if (!filter.empty() && option.searchText.find(filter) == std::string::npos) {
                continue;
            }
            visibleRows.push_back(&option);
        }

        ImGui::Text(
            "%s: %zu",
            ModMenu::GetLoc("menu.resource_available", "Available"),
            visibleRows.size());
        if (visibleRows.empty()) {
            ImGui::TextDisabled("%s", ModMenu::GetLoc("menu.resource_no_results", "No matching options."));
        } else {
            auto* clipper = ImGui::ImGuiListClipperManager::Create();
            ImGui::ImGuiListClipperManager::Begin(clipper, static_cast<int>(visibleRows.size()), 0.0F);
            while (ImGui::ImGuiListClipperManager::Step(clipper)) {
                for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
                    const auto& option = *visibleRows[static_cast<std::size_t>(row)];
                    const bool isSelected = option.formID == selectedFormID;
                    if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                        selectedFormID = option.formID;
                        pickerState.search.clear();
                        changed = true;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::ImGuiListClipperManager::End(clipper);
            ImGui::ImGuiListClipperManager::Destroy(clipper);
        }
        ImGui::EndCombo();
        return changed;
    }

    bool DrawResourceCost(const char* label, Settings::RespawnResourceCost& cost) {
        if (!ImGui::CollapsingHeader(label)) {
            return false;
        }

        bool changed = false;
        ImGui::PushID(label);
        ImGui::Indent();
        changed |= ImGui::Checkbox(ModMenu::GetLoc("menu.resource_enabled", "Enable resource cost"), &cost.enabled);
        if (cost.enabled) {
            changed |= DrawResourceDropdown(
                ModMenu::GetLoc("menu.resource_form", "Resource"),
                label,
                cost.resource);
            changed |= RenderIntSliderWithInput(
                ModMenu::GetLoc("menu.resource_quantity", "Required quantity"),
                &cost.quantity,
                1,
                999999);
            const char* actions[] = {
                ModMenu::GetLoc("menu.resource_action_spend", "Spend"),
                ModMenu::GetLoc("menu.resource_action_use", "Use")
            };
            int action = static_cast<int>(cost.action);
            if (ImGui::Combo(
                    ModMenu::GetLoc("menu.resource_action", "Resource action"),
                    &action,
                    actions,
                    static_cast<int>(std::size(actions)))) {
                cost.action = static_cast<Settings::ResourceAction>(std::clamp(action, 0, 1));
                changed = true;
            }

            auto form = RE::TESForm::LookupByID(cost.resource);
            auto resource = form ? form->As<RE::TESBoundObject>() : nullptr;
            auto player = RE::PlayerCharacter::GetSingleton();
            if (resource && player) {
                ImGui::TextDisabled(
                    "%s: %d",
                    ModMenu::GetLoc("menu.resource_owned", "Currently owned"),
                    std::max(player->GetItemCount(resource), 0));
            } else if (cost.resource != 0) {
                ImGui::TextDisabled("%s", ModMenu::GetLoc("menu.resource_unresolved", "Resource is not currently resolved"));
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
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

    void ReadResourceCost(
        const rapidjson::Value& costs,
        const char* key,
        Settings::RespawnResourceCost& cost)
    {
        if (!costs.HasMember(key) || !costs[key].IsObject()) {
            return;
        }
        const auto& object = costs[key];
        ReadBool(object, "enabled", cost.enabled);
        ReadInt(object, "quantity", cost.quantity);
        if (object.HasMember("action") && object["action"].IsInt()) {
            cost.action = static_cast<Settings::ResourceAction>(
                std::clamp(object["action"].GetInt(), 0, 1));
        }

        cost.resource = 0;
        if (object.HasMember("resourceEditorID") && object["resourceEditorID"].IsString()) {
            if (const auto form = RE::TESForm::LookupByEditorID(object["resourceEditorID"].GetString())) {
                if (form->As<RE::TESBoundObject>()) {
                    cost.resource = form->GetFormID();
                    return;
                }
            }
        }
        if (object.HasMember("resource")) {
            const auto& resource = object["resource"];
            if (resource.IsString()) {
                cost.resource = FormIDFromString(resource.GetString());
            } else if (resource.IsUint()) {
                cost.resource = resource.GetUint();
            }
        }
    }

    rapidjson::Value MakeResourceCost(
        const Settings::RespawnResourceCost& cost,
        rapidjson::Document::AllocatorType& allocator)
    {
        rapidjson::Value object(rapidjson::kObjectType);
        object.AddMember("enabled", cost.enabled, allocator);
        object.AddMember("quantity", cost.quantity, allocator);
        object.AddMember("action", static_cast<int>(cost.action), allocator);

        const auto form = RE::TESForm::LookupByID(cost.resource);
        if (form && form->As<RE::TESBoundObject>()) {
            const auto normalized = NormalizeFormID(form);
            rapidjson::Value resource;
            resource.SetString(normalized.c_str(), allocator);
            object.AddMember("resource", resource, allocator);
            try {
                const auto editorID = clib_util::editorID::get_editorID(form);
                if (!editorID.empty()) {
                    rapidjson::Value editorIDValue;
                    editorIDValue.SetString(editorID.c_str(), allocator);
                    object.AddMember("resourceEditorID", editorIDValue, allocator);
                }
            } catch (...) {
                logger::warn("Could not read the EditorID for resource {:08X} while saving settings.", cost.resource);
            }
        } else {
            object.AddMember("resource", cost.resource, allocator);
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
        if (gameplay.HasMember("respawnCosts") && gameplay["respawnCosts"].IsObject()) {
            const auto& costs = gameplay["respawnCosts"];
            ReadResourceCost(costs, "respawn_here", Settings::Gameplay.respawnHereCost);
            ReadResourceCost(costs, "respawn_checkpoint", Settings::Gameplay.lastCheckpointCost);
            ReadResourceCost(costs, "respawn_last_sleep", Settings::Gameplay.lastSleepCost);
        }
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

void ModMenu::RefreshResourceList() {
    resourceOptions.clear();
    resourcePlugins.assign(1, "All");
    resourceTypes.assign(1, "All");
    resourcePickerStates.clear();
    searchableComboFilters.clear();
    const auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("Could not populate respawn resource list: TESDataHandler is unavailable.");
        return;
    }

    const auto append = [&]<class T>(const char* typeName) {
        const auto& forms = dataHandler->GetFormArray<T>();
        for (const auto form : forms) {
            if (!form || form->IsDeleted() || form->IsIgnored()) {
                continue;
            }
            try {
                const auto editorID = clib_util::editorID::get_editorID(form);
                std::string name;
                if (const auto fullName = form->template As<RE::TESFullName>()) {
                    name = fullName->fullName.c_str();
                }
                const auto plugin = form->GetFile(0) ? form->GetFile(0)->GetFilename() : "Dynamic";
                const auto normalizedFormID = NormalizeFormID(form);
                std::string label;
                if (!editorID.empty() && !normalizedFormID.empty()) {
                    label = fmt::format("{} ({})", editorID, normalizedFormID);
                } else if (!editorID.empty()) {
                    label = editorID;
                } else if (!normalizedFormID.empty()) {
                    label = normalizedFormID;
                } else {
                    label = fmt::format("{:08X}", form->GetFormID());
                }
                if (!name.empty() && name != editorID) {
                    label += " - " + name;
                }

                const std::string pluginName(plugin);
                const std::string type = typeName;
                std::string searchText = ToLower(label);
                searchText += ' ';
                searchText += ToLower(name);
                searchText += ' ';
                searchText += ToLower(editorID);
                searchText += ' ';
                searchText += ToLower(normalizedFormID);
                searchText += ' ';
                searchText += ToLower(pluginName);
                searchText += ' ';
                searchText += ToLower(type);

                resourceOptions.push_back(ResourceOption{
                    form->GetFormID(),
                    editorID,
                    normalizedFormID,
                    name,
                    pluginName,
                    type,
                    std::move(label),
                    std::move(searchText)
                });
                resourcePlugins.push_back(pluginName);
                resourceTypes.push_back(type);
            } catch (const std::exception& error) {
                logger::warn(
                    "Could not add resource {:08X} to the settings list: {}",
                    form->GetFormID(),
                    error.what());
            } catch (...) {
                logger::warn(
                    "Could not add resource {:08X} to the settings list due to an unknown error.",
                    form->GetFormID());
            }
        }
    };

    append.template operator()<RE::TESObjectARMO>("Armor");
    append.template operator()<RE::TESObjectWEAP>("Weapon");
    append.template operator()<RE::TESAmmo>("Ammo");
    append.template operator()<RE::AlchemyItem>("Alchemy Item");
    append.template operator()<RE::IngredientItem>("Ingredient");
    append.template operator()<RE::ScrollItem>("Scroll");
    append.template operator()<RE::TESObjectBOOK>("Book");
    append.template operator()<RE::TESObjectMISC>("Misc Item");
    append.template operator()<RE::TESKey>("Key");
    append.template operator()<RE::TESSoulGem>("Soul Gem");
    append.template operator()<RE::TESObjectLIGH>("Light");
    append.template operator()<RE::BGSApparatus>("Apparatus");

    std::ranges::sort(resourceOptions, {}, &ResourceOption::label);
    std::ranges::sort(resourcePlugins.begin() + 1, resourcePlugins.end());
    resourcePlugins.erase(
        std::unique(resourcePlugins.begin() + 1, resourcePlugins.end()),
        resourcePlugins.end());
    std::ranges::sort(resourceTypes.begin() + 1, resourceTypes.end());
    resourceTypes.erase(
        std::unique(resourceTypes.begin() + 1, resourceTypes.end()),
        resourceTypes.end());
    logger::info("Loaded {} inventory forms for respawn resource settings.", resourceOptions.size());
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
    rapidjson::Value respawnCosts(rapidjson::kObjectType);
    respawnCosts.AddMember(
        "respawn_here",
        MakeResourceCost(Settings::Gameplay.respawnHereCost, allocator),
        allocator);
    respawnCosts.AddMember(
        "respawn_checkpoint",
        MakeResourceCost(Settings::Gameplay.lastCheckpointCost, allocator),
        allocator);
    respawnCosts.AddMember(
        "respawn_last_sleep",
        MakeResourceCost(Settings::Gameplay.lastSleepCost, allocator),
        allocator);
    document.AddMember("respawnCosts", respawnCosts, allocator);

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

    ImGui::Separator();
    ImGui::TextUnformatted(GetLoc("menu.respawn_costs", "Respawn resource costs"));
    changed |= DrawResourceCost(
        GetLoc("menu.cost_respawn_here", "Respawn Here resource cost"),
        Settings::Gameplay.respawnHereCost);
    changed |= DrawResourceCost(
        GetLoc("menu.cost_checkpoint", "Last Checkpoint resource cost"),
        Settings::Gameplay.lastCheckpointCost);
    changed |= DrawResourceCost(
        GetLoc("menu.cost_last_sleep", "Last Sleep resource cost"),
        Settings::Gameplay.lastSleepCost);

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
        RefreshResourceList();
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

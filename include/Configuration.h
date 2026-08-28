#pragma once

#include "SKSEMCP/SKSEMenuFramework.hpp"

#include <string>
#include <vector>

namespace Settings {
    enum class DefeatPose : int {
        kBleedout = 0,
        kRagdoll = 1
    };

    enum class ValueSource : int {
        kFlat = 0,
        kGlobal = 1,
        kActorValue = 2
    };

    struct NumericValueSetting {
        int flatValue = 0;
        RE::FormID global = 0;
        std::string actorValue;
        ValueSource source = ValueSource::kFlat;
    };

    struct GameplaySettings {
        bool enabled = true;
        bool pauseGameWhileMenuOpen = true;
        int defeatPose = static_cast<int>(DefeatPose::kBleedout);
        NumericValueSetting healthPercent{ 50, 0, "Health", ValueSource::kFlat };
        NumericValueSetting magickaPercent{ 50, 0, "Magicka", ValueSource::kFlat };
        NumericValueSetting staminaPercent{ 50, 0, "Stamina", ValueSource::kFlat };
        NumericValueSetting invulnerabilitySeconds{ 3, 0, "Block", ValueSource::kFlat };
    };

    struct ActionStyle {
        int textSizePercent = 100;
        int buttonScalePercent = 100;
    };

    struct UISettings {
        int backgroundOpacityPercent = 100;
        int backgroundBlurPixels = 0;
        int scalePercent = 100;
        int titleTextSizePercent = 100;
        int backgroundTextSizePercent = 100;
        ActionStyle respawnHere;
        ActionStyle lastSleep;
        ActionStyle lastCheckpoint;
        ActionStyle reloadSave;
    };

    inline GameplaySettings Gameplay;
    inline UISettings UI;

    int ResolveNumericValue(
        const NumericValueSetting& setting,
        RE::Actor* actor,
        int minimum,
        int maximum);
}

namespace ModMenu {
    void Register(bool loadSettings = true);
    void GameplayRender();
    void UIRender();
    void DiagnosticsRender();
    void RefreshGlobalList();
    void LoadSettings();
    void SaveSettings();
    void SaveGameplaySettings();
    void SaveUISettings();
    void LoadLanguage();
    const char* GetLoc(const std::string& key, const char* fallback);
    std::vector<std::string> GetLocList(const std::string& key);
}

#pragma once

#include "SKSEMCP/SKSEMenuFramework.hpp"

namespace Settings {
    enum class DefeatPose : int {
        kBleedout = 0,
        kRagdoll = 1,
        kPersistentRagdoll = 2
    };

    struct GameplaySettings {
        bool enabled = true;
        bool pauseGameWhileMenuOpen = true;
        int defeatPose = static_cast<int>(DefeatPose::kBleedout);
        int healthPercent = 50;
        int magickaPercent = 50;
        int staminaPercent = 50;
        int invulnerabilitySeconds = 3;
    };

    struct UISettings {
        int backgroundOpacityPercent = 100;
        int backgroundBlurPixels = 0;
        int scalePercent = 100;
    };

    inline GameplaySettings Gameplay;
    inline UISettings UI;
}

namespace ModMenu {
    void Register();
    void GameplayRender();
    void UIRender();
    void DiagnosticsRender();
    void LoadSettings();
    void SaveSettings();
    void LoadLanguage();
    const char* GetLoc(const std::string& key, const char* fallback);
}

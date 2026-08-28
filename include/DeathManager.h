#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace DeathManager {
    struct DebugInfo {
        bool damageProtectionActive{ false };
        bool ghostCaptured{ false };
        bool playerWasGhost{ false };
        float protectedHealth{ 0.0F };
        std::int64_t protectionRemainingMilliseconds{ 0 };
        std::string state;
        std::string physicalCause;
        std::string presentationCause;
        std::string backgroundTemplate;
    };

    void CaptureAppliedPlayerDamage(
        RE::PlayerCharacter* player,
        RE::Actor* attacker);
    void MarkPlayerFallDamage();
    void HandlePlayerHitEvent(const RE::TESHitEvent& event);
    bool TryInterceptDeath(
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        bool ragdollInstantRequested);
    void HandlePlayerAnimationEvent(
        std::string_view eventName,
        std::string_view payload,
        std::uintptr_t graphSource);
    void RepairBlockedPlayerHealth(RE::PlayerCharacter* player);
    bool IsDamageBlocked();
    bool IsMenuOpen();
    std::string GetBackgroundText();
    DebugInfo GetDebugInfo();
    bool DebugSelectDeathTextCause(std::string_view cause);
    void HandleUIAction(std::string_view action);
    void OnGameLoadFinished(bool success);
    void Reset();
}

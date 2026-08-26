#pragma once

#include <string_view>

namespace DeathManager {
    void LogHealthDamageHookSnapshot(
        std::string_view phase,
        std::uint64_t damageSequence,
        std::uintptr_t callerOffset,
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        float rawDamage,
        float forwardedDamage);
    void LogKillHookSnapshot(
        std::uintptr_t callerOffset,
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        float damage,
        bool sendEvent,
        bool ragdollInstantRequested);
    void LogLethalHitTraceSnapshot(
        std::string_view phase,
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        float damage);
    void CaptureAppliedPlayerDamage(
        std::uint64_t damageSequence,
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        float rawDamage);
    void MarkPlayerFallDamage(
        float fallDistance,
        float calculatedDamage,
        bool moveFinishSource);
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
    void HandleUIAction(std::string_view action);
    void Reset();
}

#pragma once

#include <string_view>

namespace DeathManager {
    bool TryInterceptDeath(RE::PlayerCharacter* player, RE::Actor* attacker);
    void HandlePlayerAnimationEvent(std::string_view eventName);
    bool IsDamageBlocked();
    bool IsMenuOpen();
    void HandleUIAction(std::string_view action);
    void Reset();
}

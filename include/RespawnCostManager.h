#pragma once

#include "Configuration.h"
#include "RespawnTypes.h"

namespace RespawnCostManager {
    struct Status {
        bool configured{ false };
        bool resourceValid{ true };
        bool affordable{ true };
        int owned{ 0 };
        int required{ 0 };
    };

    const Settings::RespawnResourceCost* GetCost(Respawn::Option option);
    RE::TESBoundObject* ResolveResource(const Settings::RespawnResourceCost& cost);
    Status GetStatus(Respawn::Option option, RE::PlayerCharacter* player = nullptr);
    bool Apply(Respawn::Option option, RE::PlayerCharacter* player);
}

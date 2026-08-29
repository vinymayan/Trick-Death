#include "RespawnCostManager.h"

namespace RespawnCostManager {
    const Settings::RespawnResourceCost* GetCost(Respawn::Option option) {
        switch (option) {
        case Respawn::Option::Here:
            return &Settings::Gameplay.respawnHereCost;
        case Respawn::Option::LastCheckpoint:
            return &Settings::Gameplay.lastCheckpointCost;
        case Respawn::Option::LastSleep:
            return &Settings::Gameplay.lastSleepCost;
        default:
            return nullptr;
        }
    }

    RE::TESBoundObject* ResolveResource(const Settings::RespawnResourceCost& cost) {
        const auto form = RE::TESForm::LookupByID(cost.resource);
        return form ? form->As<RE::TESBoundObject>() : nullptr;
    }

    Status GetStatus(Respawn::Option option, RE::PlayerCharacter* player) {
        Status status;
        const auto cost = GetCost(option);
        if (!cost || !cost->enabled) {
            return status;
        }

        status.configured = true;
        status.required = std::max(cost->quantity, 1);
        const auto resource = ResolveResource(*cost);
        status.resourceValid = resource != nullptr;
        if (!resource) {
            status.affordable = false;
            return status;
        }

        if (!player) {
            player = RE::PlayerCharacter::GetSingleton();
        }
        status.owned = player ? std::max(player->GetItemCount(resource), 0) : 0;
        status.affordable = status.owned >= status.required;
        return status;
    }

    bool Apply(Respawn::Option option, RE::PlayerCharacter* player) {
        const auto cost = GetCost(option);
        if (!cost || !cost->enabled) {
            return true;
        }

        const auto status = GetStatus(option, player);
        const auto resource = ResolveResource(*cost);
        if (!player || !resource || !status.affordable) {
            return false;
        }

        if (cost->action == Settings::ResourceAction::kUse) {
            const auto equipManager = RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                return false;
            }
            equipManager->EquipObject(
                player,
                resource,
                nullptr,
                static_cast<std::uint32_t>(status.required),
                nullptr,
                true,
                false,
                true,
                true);
            return true;
        }

        player->RemoveItem(
            resource,
            status.required,
            RE::ITEM_REMOVE_REASON::kRemove,
            nullptr,
            nullptr);
        return true;
    }
}

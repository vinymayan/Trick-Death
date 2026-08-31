#pragma once

#include "Configuration.h"
#include "RespawnTypes.h"

namespace PlayerLootManager {
    inline constexpr std::uint32_t RECORD_TYPE = 0x4C4F4F54;  // LOOT
    inline constexpr std::uint32_t RECORD_VERSION = 1;

    struct Status {
        bool configured{ false };
        bool containerValid{ true };
    };

    struct PreparedDrop {
        RE::NiPointer<RE::TESObjectREFR> container;
        bool needed{ false };
        bool failed{ false };
    };

    const Settings::PlayerLootDropSetting* GetSetting(Respawn::Option option);
    Status GetStatus(Respawn::Option option);
    void CaptureDeathLocation(RE::PlayerCharacter* player);
    void ClearDeathLocation();
    void HandleNewDeath();
    PreparedDrop Prepare(Respawn::Option option, RE::PlayerCharacter* player);
    bool Commit(PreparedDrop& prepared, Respawn::Option option, RE::PlayerCharacter* player);
    void Cancel(PreparedDrop& prepared);

    void RegisterEvents();
    void PruneEmptyContainers();
    std::size_t GetTrackedContainerCount();

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

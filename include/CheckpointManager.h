#pragma once

#include <cstdint>
#include <string>

namespace CheckpointManager {
    struct DestinationInfo {
        bool active{ false };
        RE::FormID markerFormID{ 0 };
        RE::FormID ownerFormID{ 0 };
        RE::FormID cellFormID{ 0 };
        RE::FormID locationFormID{ 0 };
        std::uint32_t blockedRespawns{ 0 };
        std::string name;
    };

    inline constexpr std::uint32_t LAST_SLEEP_RECORD = 0x534C5054;  // SLPT
    inline constexpr std::uint32_t CHECKPOINT_RECORD = 0x43504B54;  // CPKT
    inline constexpr std::uint32_t RECORD_VERSION = 1;

    void RegisterEvents();
    void CaptureAfterSleep();
    bool SetCheckpoint(
        RE::TESObjectREFR* anchor,
        RE::TESForm* owner,
        std::string name,
        std::uint32_t blockedRespawns = 0);
    bool ClearCheckpoint(RE::TESForm* owner);

    bool HasLastSleep();
    bool HasCheckpoint();
    bool MovePlayerToLastSleep();
    bool MovePlayerToCheckpoint();
    DestinationInfo GetLastSleepInfo();
    DestinationInfo GetCheckpointInfo();
    RE::TESObjectREFR* GetLastSleepMarker();
    RE::TESObjectREFR* GetCheckpointMarker();

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

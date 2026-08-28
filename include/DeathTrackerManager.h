#pragma once

#include <cstdint>

namespace DeathTrackerManager {
    inline constexpr std::uint32_t RECORD_TYPE = 0x44544352;  // DTCR
    inline constexpr std::uint32_t RECORD_VERSION = 1;
    inline constexpr const char* GRAPH_VARIABLE = "DeathTracker";

    std::int32_t GetCount();
    std::int32_t RecordDeath(RE::PlayerCharacter* player = nullptr);
    void SetCountForDebug(std::int32_t count);
    bool SyncGraph(RE::PlayerCharacter* player = nullptr);
    void ScheduleGraphSync(std::uint32_t attempt = 0);

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

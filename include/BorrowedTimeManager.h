#pragma once

#include <cstdint>

namespace BorrowedTimeManager {
    inline constexpr std::uint32_t RECORD_TYPE = 0x4254494D;  // BTIM
    inline constexpr std::uint32_t RECORD_VERSION = 1;

    std::int32_t GetConsecutiveSuccesses();
    std::int32_t RecordSuccess();
    void ResetAfterSleep();

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

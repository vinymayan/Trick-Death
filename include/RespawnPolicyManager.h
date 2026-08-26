#pragma once

#include "RespawnTypes.h"

#include <cstdint>
#include <vector>

namespace RespawnPolicyManager {
    struct PolicyEntry {
        RE::FormID ownerFormID{ 0 };
        RE::FormID areaFormID{ 0 };
        std::uint32_t blockedMask{ 0 };
        bool persistent{ false };
    };

    struct Evaluation {
        std::uint32_t availableMask{ 0 };
        std::uint32_t blockedMask{ 0 };
        bool trickDeathDisabled{ false };
    };

    inline constexpr std::uint32_t RECORD_TYPE = 0x5253504E;  // RSPN
    inline constexpr std::uint32_t RECORD_VERSION = 1;

    bool SetPolicy(RE::TESForm* owner, RE::TESForm* area, std::uint32_t blockedMask, bool persistent);
    bool ClearPolicy(RE::TESForm* owner, RE::TESForm* area);
    std::size_t ClearPolicies(RE::TESForm* owner);
    Evaluation Evaluate();
    std::vector<PolicyEntry> GetPolicies();

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TextManager {
    struct OverrideInfo {
        RE::FormID ownerFormID{ 0 };
        std::string slot;
        std::string textTemplate;
        std::int32_t priority{ 0 };
        std::uint64_t sequence{ 0 };
        bool persistent{ false };
    };

    struct VariableInfo {
        RE::FormID ownerFormID{ 0 };
        std::string key;
        std::string value;
        std::uint64_t sequence{ 0 };
        bool persistent{ false };
    };

    inline constexpr std::uint32_t RECORD_TYPE = 0x54455854;  // TEXT
    inline constexpr std::uint32_t RECORD_VERSION = 1;

    bool IsValidSlot(std::string_view slot);
    bool SetOverride(
        RE::TESForm* owner,
        std::string slot,
        std::string textTemplate,
        std::int32_t priority,
        bool persistent);
    bool SetVariable(RE::TESForm* owner, std::string key, std::string value, bool persistent);
    std::size_t ClearOwner(RE::TESForm* owner);

    void SetRuntimeVariable(std::string key, std::string value);
    void ClearRuntimeVariables();
    std::string ResolveSlot(std::string_view slot, std::string_view fallback);
    std::string ResolveTemplate(std::string_view textTemplate);
    std::vector<OverrideInfo> GetOverrides();
    std::vector<VariableInfo> GetVariables();

    void Save(SKSE::SerializationInterface* serialization);
    bool LoadRecord(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        std::uint32_t version,
        std::uint32_t length);
    void Revert();
}

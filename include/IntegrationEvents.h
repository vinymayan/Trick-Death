#pragma once

#include "RespawnTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace IntegrationEvents {
    enum class Type : std::uint32_t {
        LastSleepChanged = 0x544401,
        CheckpointChanged = 0x544402,
        RespawnSelected = 0x544403,
        RespawnCompleted = 0x544404
    };

    struct EventData {
        std::uint32_t structSize{ sizeof(EventData) };
        Type type{ Type::LastSleepChanged };
        Respawn::Option option{ Respawn::Option::None };
        RE::FormID markerFormID{ 0 };
        RE::FormID ownerFormID{ 0 };
        std::array<char, 256> name{};
    };

    void SendLastSleepChanged(RE::FormID markerFormID, std::string_view name);
    void SendCheckpointChanged(RE::FormID markerFormID, RE::FormID ownerFormID, std::string_view name);
    void SendRespawnSelected(Respawn::Option option);
    void SendRespawnCompleted(Respawn::Option option);
    std::string GetLastEventDescription();
}

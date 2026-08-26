#pragma once

#include <cstdint>

namespace RE {
    class TESForm;
    class TESObjectREFR;
}

namespace TRICK_DEATH_API {
    inline constexpr std::uint32_t API_VERSION = 1;

    enum RespawnMask : std::uint32_t {
        kRespawnHere = 1U << 0,
        kLastSleep = 1U << 1,
        kLastCheckpoint = 1U << 2,
        kLoadLastSave = 1U << 3,
        kDisableTrickDeath = 1U << 4
    };

    enum MessageType : std::uint32_t {
        kLastSleepChanged = 0x544401,
        kCheckpointChanged = 0x544402,
        kRespawnSelected = 0x544403,
        kRespawnCompleted = 0x544404
    };

    struct EventData {
        std::uint32_t structSize{ sizeof(EventData) };
        MessageType type{ kLastSleepChanged };
        std::uint32_t option{ 0 };
        std::uint32_t markerFormID{ 0 };
        std::uint32_t ownerFormID{ 0 };
        char name[256]{};
    };

    struct CheckpointRequest {
        std::uint32_t structSize{ sizeof(CheckpointRequest) };
        RE::TESObjectREFR* anchor{ nullptr };
        RE::TESForm* owner{ nullptr };
        const char* name{ nullptr };
        std::uint32_t blockedRespawns{ 0 };
    };

    class ITrickDeath1 {
    public:
        virtual ~ITrickDeath1() = default;
        virtual std::uint32_t GetVersion() const noexcept = 0;
        virtual bool SetCheckpoint(const CheckpointRequest& request) noexcept = 0;
        virtual bool ClearCheckpoint(RE::TESForm* owner) noexcept = 0;
        virtual bool HasCheckpoint() const noexcept = 0;
        virtual bool HasLastSleep() const noexcept = 0;
        virtual bool SetRespawnPolicy(
            RE::TESForm* owner,
            RE::TESForm* area,
            std::uint32_t blockedMask,
            bool persistent) noexcept = 0;
        virtual bool ClearRespawnPolicy(RE::TESForm* owner, RE::TESForm* area) noexcept = 0;
        virtual bool SetTextOverride(
            RE::TESForm* owner,
            const char* slot,
            const char* textTemplate,
            std::int32_t priority,
            bool persistent) noexcept = 0;
        virtual bool SetTextVariable(
            RE::TESForm* owner,
            const char* key,
            const char* value,
            bool persistent) noexcept = 0;
        virtual std::uint32_t ClearTextOverrides(RE::TESForm* owner) noexcept = 0;
        virtual std::uint32_t GetAvailableRespawns() const noexcept = 0;
    };
}

extern "C" __declspec(dllexport) TRICK_DEATH_API::ITrickDeath1* GetTrickDeathAPI(
    std::uint32_t version = TRICK_DEATH_API::API_VERSION) noexcept;

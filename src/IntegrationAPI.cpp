#include "TrickDeathAPI.h"

#include "CheckpointManager.h"
#include "RespawnPolicyManager.h"
#include "TextManager.h"

namespace {
    class TrickDeathAPIImplementation final : public TRICK_DEATH_API::ITrickDeath1 {
    public:
        std::uint32_t GetVersion() const noexcept override {
            return TRICK_DEATH_API::API_VERSION;
        }

        bool SetCheckpoint(const TRICK_DEATH_API::CheckpointRequest& request) noexcept override {
            if (request.structSize < sizeof(TRICK_DEATH_API::CheckpointRequest)) {
                return false;
            }
            return CheckpointManager::SetCheckpoint(
                request.anchor,
                request.owner,
                request.name ? request.name : "",
                request.blockedRespawns);
        }

        bool ClearCheckpoint(RE::TESForm* owner) noexcept override {
            return CheckpointManager::ClearCheckpoint(owner);
        }

        bool HasCheckpoint() const noexcept override {
            return CheckpointManager::HasCheckpoint();
        }

        bool HasLastSleep() const noexcept override {
            return CheckpointManager::HasLastSleep();
        }

        bool SetRespawnPolicy(
            RE::TESForm* owner,
            RE::TESForm* area,
            std::uint32_t blockedMask,
            bool persistent) noexcept override
        {
            return RespawnPolicyManager::SetPolicy(owner, area, blockedMask, persistent);
        }

        bool ClearRespawnPolicy(RE::TESForm* owner, RE::TESForm* area) noexcept override {
            return RespawnPolicyManager::ClearPolicy(owner, area);
        }

        bool SetTextOverride(
            RE::TESForm* owner,
            const char* slot,
            const char* textTemplate,
            std::int32_t priority,
            bool persistent) noexcept override
        {
            return slot && textTemplate &&
                TextManager::SetOverride(owner, slot, textTemplate, priority, persistent);
        }

        bool SetTextVariable(
            RE::TESForm* owner,
            const char* key,
            const char* value,
            bool persistent) noexcept override
        {
            return key && value && TextManager::SetVariable(owner, key, value, persistent);
        }

        std::uint32_t ClearTextOverrides(RE::TESForm* owner) noexcept override {
            return static_cast<std::uint32_t>(TextManager::ClearOwner(owner));
        }

        std::uint32_t GetAvailableRespawns() const noexcept override {
            return RespawnPolicyManager::Evaluate().availableMask;
        }
    };
}

extern "C" __declspec(dllexport) TRICK_DEATH_API::ITrickDeath1* GetTrickDeathAPI(
    std::uint32_t version) noexcept
{
    static TrickDeathAPIImplementation implementation;
    return version == TRICK_DEATH_API::API_VERSION ? std::addressof(implementation) : nullptr;
}

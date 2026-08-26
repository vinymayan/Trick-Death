#include "PapyrusAPI.h"

#include "CheckpointManager.h"
#include "RespawnPolicyManager.h"
#include "TextManager.h"

namespace {
    constexpr auto SCRIPT_NAME = "TrickDeathNative"sv;

    bool SetCheckpoint(
        RE::StaticFunctionTag*,
        RE::TESObjectREFR* anchor,
        RE::TESForm* owner,
        RE::BSFixedString name,
        std::uint32_t blockedRespawns)
    {
        return CheckpointManager::SetCheckpoint(anchor, owner, name.c_str(), blockedRespawns);
    }

    bool SetCheckpointAtPlayer(
        RE::StaticFunctionTag*,
        RE::TESForm* owner,
        RE::BSFixedString name,
        std::uint32_t blockedRespawns)
    {
        return CheckpointManager::SetCheckpoint(
            RE::PlayerCharacter::GetSingleton(),
            owner,
            name.c_str(),
            blockedRespawns);
    }

    bool ClearCheckpoint(RE::StaticFunctionTag*, RE::TESForm* owner) {
        return CheckpointManager::ClearCheckpoint(owner);
    }

    bool HasCheckpoint(RE::StaticFunctionTag*) {
        return CheckpointManager::HasCheckpoint();
    }

    bool HasLastSleep(RE::StaticFunctionTag*) {
        return CheckpointManager::HasLastSleep();
    }

    bool SetRespawnPolicy(
        RE::StaticFunctionTag*,
        RE::TESForm* owner,
        RE::TESForm* area,
        std::uint32_t blockedMask,
        bool persistent)
    {
        return RespawnPolicyManager::SetPolicy(owner, area, blockedMask, persistent);
    }

    bool ClearRespawnPolicy(RE::StaticFunctionTag*, RE::TESForm* owner, RE::TESForm* area) {
        return RespawnPolicyManager::ClearPolicy(owner, area);
    }

    std::uint32_t ClearRespawnPolicies(RE::StaticFunctionTag*, RE::TESForm* owner) {
        return static_cast<std::uint32_t>(RespawnPolicyManager::ClearPolicies(owner));
    }

    bool SetTextOverride(
        RE::StaticFunctionTag*,
        RE::TESForm* owner,
        RE::BSFixedString slot,
        RE::BSFixedString textTemplate,
        std::int32_t priority,
        bool persistent)
    {
        return TextManager::SetOverride(
            owner,
            slot.c_str(),
            textTemplate.c_str(),
            priority,
            persistent);
    }

    bool SetTextVariable(
        RE::StaticFunctionTag*,
        RE::TESForm* owner,
        RE::BSFixedString key,
        RE::BSFixedString value,
        bool persistent)
    {
        return TextManager::SetVariable(owner, key.c_str(), value.c_str(), persistent);
    }

    std::uint32_t ClearTextOverrides(RE::StaticFunctionTag*, RE::TESForm* owner) {
        return static_cast<std::uint32_t>(TextManager::ClearOwner(owner));
    }

    std::uint32_t GetAvailableRespawns(RE::StaticFunctionTag*) {
        return RespawnPolicyManager::Evaluate().availableMask;
    }
}

bool PapyrusAPI::Register(RE::BSScript::IVirtualMachine* virtualMachine) {
    if (!virtualMachine) {
        return false;
    }
    virtualMachine->RegisterFunction("SetCheckpoint", SCRIPT_NAME, SetCheckpoint);
    virtualMachine->RegisterFunction("SetCheckpointAtPlayer", SCRIPT_NAME, SetCheckpointAtPlayer);
    virtualMachine->RegisterFunction("ClearCheckpoint", SCRIPT_NAME, ClearCheckpoint);
    virtualMachine->RegisterFunction("HasCheckpoint", SCRIPT_NAME, HasCheckpoint);
    virtualMachine->RegisterFunction("HasLastSleep", SCRIPT_NAME, HasLastSleep);
    virtualMachine->RegisterFunction("SetRespawnPolicy", SCRIPT_NAME, SetRespawnPolicy);
    virtualMachine->RegisterFunction("ClearRespawnPolicy", SCRIPT_NAME, ClearRespawnPolicy);
    virtualMachine->RegisterFunction("ClearRespawnPolicies", SCRIPT_NAME, ClearRespawnPolicies);
    virtualMachine->RegisterFunction("SetTextOverride", SCRIPT_NAME, SetTextOverride);
    virtualMachine->RegisterFunction("SetTextVariable", SCRIPT_NAME, SetTextVariable);
    virtualMachine->RegisterFunction("ClearTextOverrides", SCRIPT_NAME, ClearTextOverrides);
    virtualMachine->RegisterFunction("GetAvailableRespawns", SCRIPT_NAME, GetAvailableRespawns);
    logger::info("Registered TrickDeathNative Papyrus functions.");
    return true;
}

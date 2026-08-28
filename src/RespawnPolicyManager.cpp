#include "RespawnPolicyManager.h"

#include "CheckpointManager.h"
#include "CurrentSaveManager.h"

#include <algorithm>
#include <mutex>

namespace {
    constexpr std::uint32_t MAX_POLICIES = 1024;
    std::mutex policyLock;
    std::vector<RespawnPolicyManager::PolicyEntry> policies;

    bool IsValidArea(RE::TESForm* area) {
        return !area || area->As<RE::TESObjectCELL>() || area->As<RE::BGSLocation>();
    }

    bool LocationIncludes(RE::FormID childID, RE::FormID expectedID) {
        auto location = childID ? RE::TESForm::LookupByID<RE::BGSLocation>(childID) : nullptr;
        for (std::uint32_t depth = 0; location && depth < 64; ++depth) {
            if (location->GetFormID() == expectedID) {
                return true;
            }
            location = location->parentLoc;
        }
        return false;
    }

    bool MatchesArea(
        const RespawnPolicyManager::PolicyEntry& policy,
        RE::FormID cellFormID,
        RE::FormID locationFormID)
    {
        if (policy.areaFormID == 0) {
            return true;
        }
        if (RE::TESForm::LookupByID<RE::TESObjectCELL>(policy.areaFormID)) {
            return cellFormID == policy.areaFormID;
        }
        if (RE::TESForm::LookupByID<RE::BGSLocation>(policy.areaFormID)) {
            return LocationIncludes(locationFormID, policy.areaFormID);
        }
        return false;
    }

    bool IsBlocked(
        const std::vector<RespawnPolicyManager::PolicyEntry>& snapshot,
        Respawn::Option option,
        RE::FormID cellFormID,
        RE::FormID locationFormID,
        bool globalOnly = false)
    {
        return std::ranges::any_of(snapshot, [&](const auto& policy) {
            return Respawn::Contains(policy.blockedMask, option) &&
                (!globalOnly || policy.areaFormID == 0) &&
                MatchesArea(policy, cellFormID, locationFormID);
        });
    }

    template <class T>
    bool ReadValue(SKSE::SerializationInterface* serialization, T& value) {
        return serialization->ReadRecordData(value) == sizeof(value);
    }
}

bool RespawnPolicyManager::SetPolicy(
    RE::TESForm* owner,
    RE::TESForm* area,
    std::uint32_t blockedMask,
    bool persistent)
{
    if (!owner || !IsValidArea(area)) {
        logger::warn("Rejected respawn policy with a missing owner or invalid area form.");
        return false;
    }
    blockedMask &= Respawn::POLICY_MASK;
    const auto ownerID = owner->GetFormID();
    const auto areaID = area ? area->GetFormID() : 0;
    std::scoped_lock lock(policyLock);
    const auto it = std::ranges::find_if(policies, [&](const auto& value) {
        return value.ownerFormID == ownerID && value.areaFormID == areaID;
    });
    if (blockedMask == 0) {
        if (it != policies.end()) {
            policies.erase(it);
        }
        return true;
    }
    PolicyEntry updated{ ownerID, areaID, blockedMask, persistent };
    if (it == policies.end()) {
        policies.push_back(updated);
    } else {
        *it = updated;
    }
    return true;
}

bool RespawnPolicyManager::ClearPolicy(RE::TESForm* owner, RE::TESForm* area) {
    if (!owner || !IsValidArea(area)) {
        return false;
    }
    const auto ownerID = owner->GetFormID();
    const auto areaID = area ? area->GetFormID() : 0;
    std::scoped_lock lock(policyLock);
    const auto previousSize = policies.size();
    std::erase_if(policies, [&](const auto& value) {
        return value.ownerFormID == ownerID && value.areaFormID == areaID;
    });
    return policies.size() != previousSize;
}

std::size_t RespawnPolicyManager::ClearPolicies(RE::TESForm* owner) {
    if (!owner) {
        return 0;
    }
    const auto ownerID = owner->GetFormID();
    std::scoped_lock lock(policyLock);
    const auto previousSize = policies.size();
    std::erase_if(policies, [&](const auto& value) { return value.ownerFormID == ownerID; });
    return previousSize - policies.size();
}

RespawnPolicyManager::Evaluation RespawnPolicyManager::Evaluate() {
    std::vector<PolicyEntry> snapshot;
    {
        std::scoped_lock lock(policyLock);
        snapshot = policies;
    }

    Evaluation result;
    auto player = RE::PlayerCharacter::GetSingleton();
    auto currentCell = player ? player->GetParentCell() : nullptr;
    auto currentLocation = currentCell ? currentCell->GetLocation() : nullptr;
    const auto currentCellID = currentCell ? currentCell->GetFormID() : 0;
    const auto currentLocationID = currentLocation ? currentLocation->GetFormID() : 0;
    const auto sleep = CheckpointManager::GetLastSleepInfo();
    const auto checkpoint = CheckpointManager::GetCheckpointInfo();
    const auto checkpointRestrictions = checkpoint.active ? checkpoint.blockedRespawns : 0;

    result.trickDeathDisabled = IsBlocked(
        snapshot,
        Respawn::Option::DisableTrickDeath,
        currentCellID,
        currentLocationID);

    const bool hereBlocked = IsBlocked(
        snapshot,
        Respawn::Option::Here,
        currentCellID,
        currentLocationID) ||
        Respawn::Contains(checkpointRestrictions, Respawn::Option::Here);
    const bool sleepBlocked = !sleep.active ||
        Respawn::Contains(checkpointRestrictions, Respawn::Option::LastSleep) ||
        Respawn::Contains(sleep.blockedRespawns, Respawn::Option::LastSleep) ||
        IsBlocked(snapshot, Respawn::Option::LastSleep, sleep.cellFormID, sleep.locationFormID);
    const bool checkpointBlocked = !checkpoint.active ||
        Respawn::Contains(checkpointRestrictions, Respawn::Option::LastCheckpoint) ||
        IsBlocked(snapshot, Respawn::Option::LastCheckpoint, checkpoint.cellFormID, checkpoint.locationFormID);
    const bool reloadBlocked = !CurrentSaveManager::HasCurrentSave() ||
        Respawn::Contains(checkpointRestrictions, Respawn::Option::ReloadSave) ||
        IsBlocked(snapshot, Respawn::Option::ReloadSave, 0, 0, true);

    const auto setAvailability = [&](Respawn::Option option, bool blocked) {
        if (blocked) {
            result.blockedMask |= Respawn::ToMask(option);
        } else {
            result.availableMask |= Respawn::ToMask(option);
        }
    };
    setAvailability(Respawn::Option::Here, hereBlocked);
    setAvailability(Respawn::Option::LastSleep, sleepBlocked);
    setAvailability(Respawn::Option::LastCheckpoint, checkpointBlocked);
    setAvailability(Respawn::Option::ReloadSave, reloadBlocked);
    if (result.trickDeathDisabled) {
        result.blockedMask |= Respawn::ToMask(Respawn::Option::DisableTrickDeath);
        result.availableMask = 0;
    }
    return result;
}

std::vector<RespawnPolicyManager::PolicyEntry> RespawnPolicyManager::GetPolicies() {
    std::scoped_lock lock(policyLock);
    return policies;
}

void RespawnPolicyManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization || !serialization->OpenRecord(RECORD_TYPE, RECORD_VERSION)) {
        return;
    }
    std::scoped_lock lock(policyLock);
    const auto count = static_cast<std::uint32_t>(std::ranges::count_if(
        policies,
        [](const auto& entry) { return entry.persistent; }));
    serialization->WriteRecordData(count);
    for (const auto& policy : policies) {
        if (!policy.persistent) {
            continue;
        }
        const std::uint8_t persistent = 1;
        serialization->WriteRecordData(policy.ownerFormID);
        serialization->WriteRecordData(policy.areaFormID);
        serialization->WriteRecordData(policy.blockedMask);
        serialization->WriteRecordData(persistent);
    }
}

bool RespawnPolicyManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || type != RECORD_TYPE || version != RECORD_VERSION) {
        return false;
    }
    std::uint32_t count = 0;
    if (!ReadValue(serialization, count) || count > MAX_POLICIES) {
        logger::warn("Ignored invalid v1 respawn-policy record.");
        return true;
    }
    std::vector<PolicyEntry> loaded;
    loaded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        RE::FormID savedOwner = 0;
        RE::FormID savedArea = 0;
        PolicyEntry policy;
        std::uint8_t persistent = 0;
        if (!ReadValue(serialization, savedOwner) ||
            !ReadValue(serialization, savedArea) ||
            !ReadValue(serialization, policy.blockedMask) ||
            !ReadValue(serialization, persistent)) {
            logger::warn("Stopped reading a truncated respawn-policy record.");
            break;
        }
        if (!savedOwner || !serialization->ResolveFormID(savedOwner, policy.ownerFormID)) {
            continue;
        }
        if (savedArea && !serialization->ResolveFormID(savedArea, policy.areaFormID)) {
            continue;
        }
        policy.blockedMask &= Respawn::POLICY_MASK;
        policy.persistent = persistent != 0;
        if (policy.blockedMask != 0) {
            loaded.push_back(policy);
        }
    }
    std::scoped_lock lock(policyLock);
    policies = std::move(loaded);
    return true;
}

void RespawnPolicyManager::Revert() {
    std::scoped_lock lock(policyLock);
    policies.clear();
}

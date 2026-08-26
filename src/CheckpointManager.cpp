#include "CheckpointManager.h"

#include "IntegrationEvents.h"
#include "RespawnTypes.h"

#include <algorithm>
#include <mutex>

namespace {
    constexpr RE::FormID XMARKER_FORM_ID = 0x0000003B;
    constexpr std::uint32_t MAX_NAME_LENGTH = 512;

    std::mutex destinationLock;
    CheckpointManager::DestinationInfo lastSleep;
    CheckpointManager::DestinationInfo checkpoint;

    RE::TESObjectREFR* ResolveMarker(const CheckpointManager::DestinationInfo& destination) {
        return destination.active && destination.markerFormID ?
            RE::TESForm::LookupByID<RE::TESObjectREFR>(destination.markerFormID) : nullptr;
    }

    std::string GetDestinationName(RE::TESObjectREFR* anchor, std::string requestedName) {
        if (!requestedName.empty()) {
            return requestedName.substr(0, MAX_NAME_LENGTH);
        }
        if (auto cell = anchor ? anchor->GetParentCell() : nullptr) {
            if (auto location = cell->GetLocation()) {
                if (const auto fullName = location->GetFullName(); fullName && fullName[0] != '\0') {
                    return fullName;
                }
            }
            if (const auto fullName = cell->GetFullName(); fullName && fullName[0] != '\0') {
                return fullName;
            }
        }
        return "Unnamed destination";
    }

    bool UpdateDestination(
        CheckpointManager::DestinationInfo& destination,
        RE::TESObjectREFR* anchor,
        RE::TESForm* owner,
        std::string name,
        std::uint32_t blockedRespawns)
    {
        if (!anchor || !anchor->GetParentCell()) {
            return false;
        }

        auto marker = destination.markerFormID ?
            RE::TESForm::LookupByID<RE::TESObjectREFR>(destination.markerFormID) : nullptr;
        if (marker) {
            marker->MoveTo(anchor);
        } else {
            auto markerBase = RE::TESForm::LookupByID<RE::TESBoundObject>(XMARKER_FORM_ID);
            if (!markerBase) {
                logger::error("Could not resolve the vanilla XMarker form.");
                return false;
            }
            marker = anchor->PlaceObjectAtMe(markerBase, true).get();
            if (!marker) {
                logger::error("Could not create a Trick Death destination marker.");
                return false;
            }
        }

        marker->SetPosition(anchor->GetPosition());
        marker->SetAngle(anchor->GetAngle());
        const auto cell = anchor->GetParentCell();
        const auto location = cell ? cell->GetLocation() : nullptr;
        destination.active = true;
        destination.markerFormID = marker->GetFormID();
        destination.ownerFormID = owner ? owner->GetFormID() : 0;
        destination.cellFormID = cell ? cell->GetFormID() : 0;
        destination.locationFormID = location ? location->GetFormID() : 0;
        destination.blockedRespawns = blockedRespawns & Respawn::ACTION_MASK;
        destination.name = GetDestinationName(anchor, std::move(name));
        return true;
    }

    template <class T>
    bool ReadValue(SKSE::SerializationInterface* serialization, T& value) {
        return serialization->ReadRecordData(value) == sizeof(value);
    }

    bool WriteString(SKSE::SerializationInterface* serialization, const std::string& value) {
        const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), MAX_NAME_LENGTH));
        return serialization->WriteRecordData(length) &&
            (length == 0 || serialization->WriteRecordData(value.data(), length));
    }

    bool ReadString(SKSE::SerializationInterface* serialization, std::string& value) {
        std::uint32_t length = 0;
        if (!ReadValue(serialization, length) || length > MAX_NAME_LENGTH) {
            return false;
        }
        value.resize(length);
        return length == 0 || serialization->ReadRecordData(value.data(), length) == length;
    }

    bool SaveDestination(
        SKSE::SerializationInterface* serialization,
        std::uint32_t type,
        const CheckpointManager::DestinationInfo& destination)
    {
        if (!serialization->OpenRecord(type, CheckpointManager::RECORD_VERSION)) {
            return false;
        }
        const std::uint8_t active = destination.active ? 1 : 0;
        return serialization->WriteRecordData(active) &&
            serialization->WriteRecordData(destination.markerFormID) &&
            serialization->WriteRecordData(destination.ownerFormID) &&
            serialization->WriteRecordData(destination.cellFormID) &&
            serialization->WriteRecordData(destination.locationFormID) &&
            serialization->WriteRecordData(destination.blockedRespawns) &&
            WriteString(serialization, destination.name);
    }

    bool ResolveSavedForm(
        SKSE::SerializationInterface* serialization,
        RE::FormID saved,
        RE::FormID& resolved)
    {
        resolved = 0;
        return saved == 0 || serialization->ResolveFormID(saved, resolved);
    }

    bool LoadDestination(
        SKSE::SerializationInterface* serialization,
        CheckpointManager::DestinationInfo& destination)
    {
        std::uint8_t active = 0;
        CheckpointManager::DestinationInfo loaded;
        RE::FormID marker = 0;
        RE::FormID owner = 0;
        RE::FormID cell = 0;
        RE::FormID location = 0;
        if (!ReadValue(serialization, active) ||
            !ReadValue(serialization, marker) ||
            !ReadValue(serialization, owner) ||
            !ReadValue(serialization, cell) ||
            !ReadValue(serialization, location) ||
            !ReadValue(serialization, loaded.blockedRespawns) ||
            !ReadString(serialization, loaded.name)) {
            return false;
        }
        if (!ResolveSavedForm(serialization, marker, loaded.markerFormID) ||
            !ResolveSavedForm(serialization, owner, loaded.ownerFormID) ||
            !ResolveSavedForm(serialization, cell, loaded.cellFormID) ||
            !ResolveSavedForm(serialization, location, loaded.locationFormID)) {
            return false;
        }
        loaded.active = active != 0 && loaded.markerFormID != 0;
        loaded.blockedRespawns &= Respawn::ACTION_MASK;
        destination = std::move(loaded);
        return true;
    }

    class SleepEventSink final : public RE::BSTEventSink<RE::TESSleepStopEvent> {
    public:
        static SleepEventSink* GetSingleton() {
            static SleepEventSink singleton;
            return std::addressof(singleton);
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESSleepStopEvent* event,
            RE::BSTEventSource<RE::TESSleepStopEvent>*) override
        {
            if (event && !event->interrupted) {
                SKSE::GetTaskInterface()->AddTask([] { CheckpointManager::CaptureAfterSleep(); });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

void CheckpointManager::RegisterEvents() {
    if (auto source = RE::ScriptEventSourceHolder::GetSingleton()) {
        source->AddEventSink(SleepEventSink::GetSingleton());
    }
}

void CheckpointManager::CaptureAfterSleep() {
    auto player = RE::PlayerCharacter::GetSingleton();
    DestinationInfo updated;
    {
        std::scoped_lock lock(destinationLock);
        if (!UpdateDestination(lastSleep, player, nullptr, {}, 0)) {
            logger::warn("Could not update the last-sleep destination.");
            return;
        }
        updated = lastSleep;
    }
    logger::info("Last-sleep destination updated: {:08X}", updated.markerFormID);
    IntegrationEvents::SendLastSleepChanged(updated.markerFormID, updated.name);
}

bool CheckpointManager::SetCheckpoint(
    RE::TESObjectREFR* anchor,
    RE::TESForm* owner,
    std::string name,
    std::uint32_t blockedRespawns)
{
    if (!owner) {
        logger::warn("Rejected external checkpoint without an owner form.");
        return false;
    }
    DestinationInfo updated;
    {
        std::scoped_lock lock(destinationLock);
        if (!UpdateDestination(checkpoint, anchor, owner, std::move(name), blockedRespawns)) {
            return false;
        }
        updated = checkpoint;
    }
    logger::info(
        "External checkpoint overwritten: marker={:08X}, owner={:08X}, name='{}'.",
        updated.markerFormID,
        updated.ownerFormID,
        updated.name);
    IntegrationEvents::SendCheckpointChanged(updated.markerFormID, updated.ownerFormID, updated.name);
    return true;
}

bool CheckpointManager::ClearCheckpoint(RE::TESForm* owner) {
    if (!owner) {
        return false;
    }
    {
        std::scoped_lock lock(destinationLock);
        if (!checkpoint.active || checkpoint.ownerFormID != owner->GetFormID()) {
            return false;
        }
        const auto reusableMarker = checkpoint.markerFormID;
        checkpoint = {};
        checkpoint.markerFormID = reusableMarker;
    }
    IntegrationEvents::SendCheckpointChanged(0, owner->GetFormID(), {});
    return true;
}

bool CheckpointManager::HasLastSleep() {
    std::scoped_lock lock(destinationLock);
    return ResolveMarker(lastSleep) != nullptr;
}

bool CheckpointManager::HasCheckpoint() {
    std::scoped_lock lock(destinationLock);
    return ResolveMarker(checkpoint) != nullptr;
}

bool CheckpointManager::MovePlayerToLastSleep() {
    std::scoped_lock lock(destinationLock);
    auto player = RE::PlayerCharacter::GetSingleton();
    auto marker = ResolveMarker(lastSleep);
    if (!player || !marker) {
        return false;
    }
    player->MoveTo(marker);
    return true;
}

bool CheckpointManager::MovePlayerToCheckpoint() {
    std::scoped_lock lock(destinationLock);
    auto player = RE::PlayerCharacter::GetSingleton();
    auto marker = ResolveMarker(checkpoint);
    if (!player || !marker) {
        return false;
    }
    player->MoveTo(marker);
    return true;
}

CheckpointManager::DestinationInfo CheckpointManager::GetLastSleepInfo() {
    std::scoped_lock lock(destinationLock);
    auto result = lastSleep;
    result.active = ResolveMarker(lastSleep) != nullptr;
    return result;
}

CheckpointManager::DestinationInfo CheckpointManager::GetCheckpointInfo() {
    std::scoped_lock lock(destinationLock);
    auto result = checkpoint;
    result.active = ResolveMarker(checkpoint) != nullptr;
    return result;
}

RE::TESObjectREFR* CheckpointManager::GetLastSleepMarker() {
    std::scoped_lock lock(destinationLock);
    return ResolveMarker(lastSleep);
}

RE::TESObjectREFR* CheckpointManager::GetCheckpointMarker() {
    std::scoped_lock lock(destinationLock);
    return ResolveMarker(checkpoint);
}

void CheckpointManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization) {
        return;
    }
    std::scoped_lock lock(destinationLock);
    if (!SaveDestination(serialization, LAST_SLEEP_RECORD, lastSleep)) {
        logger::error("Failed to serialize the last-sleep destination.");
    }
    if (!SaveDestination(serialization, CHECKPOINT_RECORD, checkpoint)) {
        logger::error("Failed to serialize the external checkpoint.");
    }
}

bool CheckpointManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || version != RECORD_VERSION ||
        (type != LAST_SLEEP_RECORD && type != CHECKPOINT_RECORD)) {
        return false;
    }
    std::scoped_lock lock(destinationLock);
    auto& destination = type == LAST_SLEEP_RECORD ? lastSleep : checkpoint;
    if (!LoadDestination(serialization, destination)) {
        logger::warn("Ignored invalid v1 Trick Death destination record {:08X}.", type);
    }
    return true;
}

void CheckpointManager::Revert() {
    std::scoped_lock lock(destinationLock);
    lastSleep = {};
    checkpoint = {};
}

#include "CheckpointManager.h"

namespace {
    constexpr RE::FormID XMARKER_FORM_ID = 0x0000003B;
    constexpr std::uint32_t CHECKPOINT_RECORD = 0x43504B54;  // CPKT
    constexpr std::uint32_t CHECKPOINT_VERSION = 1;

    RE::FormID checkpointFormID = 0;

    RE::TESObjectREFR* ResolveCheckpoint() {
        return checkpointFormID ? RE::TESForm::LookupByID<RE::TESObjectREFR>(checkpointFormID) : nullptr;
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
    if (!player || !player->GetParentCell()) {
        return;
    }

    auto marker = ResolveCheckpoint();
    if (marker) {
        marker->MoveTo(player);
    } else {
        auto markerBase = RE::TESForm::LookupByID<RE::TESBoundObject>(XMARKER_FORM_ID);
        if (!markerBase) {
            logger::error("Could not resolve the vanilla XMarker form.");
            return;
        }
        marker = player->PlaceObjectAtMe(markerBase, true).get();
        if (!marker) {
            logger::error("Could not create the sleep checkpoint marker.");
            return;
        }
        checkpointFormID = marker->GetFormID();
    }

    marker->SetPosition(player->GetPosition());
    marker->SetAngle(player->GetAngle());
    logger::info("Sleep checkpoint updated: {:08X}", checkpointFormID);
}

bool CheckpointManager::HasCheckpoint() {
    return ResolveCheckpoint() != nullptr;
}

bool CheckpointManager::MovePlayerToCheckpoint() {
    auto player = RE::PlayerCharacter::GetSingleton();
    auto checkpoint = ResolveCheckpoint();
    if (!player || !checkpoint) {
        return false;
    }
    player->MoveTo(checkpoint);
    return true;
}

void CheckpointManager::Save(SKSE::SerializationInterface* serialization) {
    if (serialization && serialization->OpenRecord(CHECKPOINT_RECORD, CHECKPOINT_VERSION)) {
        serialization->WriteRecordData(checkpointFormID);
    }
}

void CheckpointManager::Load(SKSE::SerializationInterface* serialization) {
    checkpointFormID = 0;
    if (!serialization) {
        return;
    }

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    while (serialization->GetNextRecordInfo(type, version, length)) {
        if (type != CHECKPOINT_RECORD || version != CHECKPOINT_VERSION || length != sizeof(RE::FormID)) {
            continue;
        }

        RE::FormID savedID = 0;
        if (serialization->ReadRecordData(savedID) != sizeof(savedID)) {
            continue;
        }
        RE::FormID resolvedID = 0;
        if (savedID && serialization->ResolveFormID(savedID, resolvedID)) {
            checkpointFormID = resolvedID;
        }
    }
}

void CheckpointManager::Revert(SKSE::SerializationInterface*) {
    checkpointFormID = 0;
}

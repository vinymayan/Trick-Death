#include "DeathTrackerManager.h"

#include "DelayedDispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace {
    std::atomic_int32_t deathCount{ 0 };
    std::atomic_bool syncScheduled{ false };

    template <class T>
    bool ReadValue(SKSE::SerializationInterface* serialization, T& value) {
        return serialization && serialization->ReadRecordData(value) == sizeof(value);
    }
}

std::int32_t DeathTrackerManager::GetCount() {
    return deathCount.load();
}

std::int32_t DeathTrackerManager::RecordDeath(RE::PlayerCharacter* player) {
    auto current = deathCount.load();
    while (current < std::numeric_limits<std::int32_t>::max() &&
           !deathCount.compare_exchange_weak(current, current + 1)) {
    }
    const auto updated = deathCount.load();
    const bool synced = SyncGraph(player);
    if (!synced) {
        ScheduleGraphSync();
    }
    return updated;
}

void DeathTrackerManager::SetCountForDebug(std::int32_t count) {
    deathCount.store(std::max<std::int32_t>(0, count));
    if (!SyncGraph()) {
        ScheduleGraphSync();
    }
}

bool DeathTrackerManager::SyncGraph(RE::PlayerCharacter* player) {
    player = player ? player : RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) {
        return false;
    }
    const auto value = deathCount.load();
    const bool set = player->SetGraphVariableInt(GRAPH_VARIABLE, value);
    std::int32_t confirmed = -1;
    const bool read = player->GetGraphVariableInt(GRAPH_VARIABLE, confirmed);
    return set && read && confirmed == value;
}

void DeathTrackerManager::ScheduleGraphSync(std::uint32_t attempt) {
    constexpr std::uint32_t MAX_ATTEMPTS = 20;
    if (attempt > MAX_ATTEMPTS) {
        logger::warn(
            "DeathTracker could not synchronize graph variable '{}' after {} attempts.",
            GRAPH_VARIABLE,
            MAX_ATTEMPTS);
        return;
    }

    bool expected = false;
    if (!syncScheduled.compare_exchange_strong(expected, true)) {
        return;
    }
    Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(100), [attempt] {
        SKSE::GetTaskInterface()->AddTask([attempt] {
            syncScheduled.store(false);
            if (!DeathTrackerManager::SyncGraph()) {
                DeathTrackerManager::ScheduleGraphSync(attempt + 1);
            }
        });
    });
}

void DeathTrackerManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization || !serialization->OpenRecord(RECORD_TYPE, RECORD_VERSION)) {
        return;
    }
    const auto value = deathCount.load();
    serialization->WriteRecordData(value);
}

bool DeathTrackerManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || type != RECORD_TYPE) {
        return false;
    }
    if (version != RECORD_VERSION) {
        logger::warn("Ignored unsupported DeathTracker record version {}.", version);
        return true;
    }
    std::int32_t loaded = 0;
    if (!ReadValue(serialization, loaded)) {
        logger::warn("Ignored truncated DeathTracker record.");
        return true;
    }
    deathCount.store(std::max<std::int32_t>(0, loaded));
    logger::info("Loaded DeathTracker={} from co-save.", deathCount.load());
    return true;
}

void DeathTrackerManager::Revert() {
    deathCount.store(0);
    syncScheduled.store(false);
}

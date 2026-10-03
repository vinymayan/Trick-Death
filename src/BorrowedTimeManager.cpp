#include "BorrowedTimeManager.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace {
    std::atomic_int32_t consecutiveSuccesses{ 0 };
}

std::int32_t BorrowedTimeManager::GetConsecutiveSuccesses() {
    return consecutiveSuccesses.load();
}

std::int32_t BorrowedTimeManager::RecordSuccess() {
    auto current = consecutiveSuccesses.load();
    while (current < std::numeric_limits<std::int32_t>::max() &&
           !consecutiveSuccesses.compare_exchange_weak(current, current + 1)) {
    }
    return consecutiveSuccesses.load();
}

void BorrowedTimeManager::ResetAfterSleep() {
    const auto previous = consecutiveSuccesses.exchange(0);
    logger::info("Borrowed Time streak reset after sleep: previous={}", previous);
}

void BorrowedTimeManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization || !serialization->OpenRecord(RECORD_TYPE, RECORD_VERSION)) {
        return;
    }
    const auto value = consecutiveSuccesses.load();
    serialization->WriteRecordData(value);
}

bool BorrowedTimeManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || type != RECORD_TYPE) {
        return false;
    }
    if (version != RECORD_VERSION) {
        logger::warn("Ignored unsupported Borrowed Time record version {}.", version);
        return true;
    }
    std::int32_t loaded = 0;
    if (serialization->ReadRecordData(loaded) != sizeof(loaded)) {
        logger::warn("Ignored truncated Borrowed Time record.");
        return true;
    }
    consecutiveSuccesses.store(std::max<std::int32_t>(0, loaded));
    logger::info("Loaded Borrowed Time streak={} from co-save.", consecutiveSuccesses.load());
    return true;
}

void BorrowedTimeManager::Revert() {
    consecutiveSuccesses.store(0);
}

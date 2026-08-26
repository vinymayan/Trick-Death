#include "SerializationManager.h"

#include "CheckpointManager.h"
#include "RespawnPolicyManager.h"
#include "TextManager.h"

void SerializationManager::Save(SKSE::SerializationInterface* serialization) {
    CheckpointManager::Save(serialization);
    RespawnPolicyManager::Save(serialization);
    TextManager::Save(serialization);
}

void SerializationManager::Load(SKSE::SerializationInterface* serialization) {
    Revert(serialization);
    if (!serialization) {
        return;
    }

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    while (serialization->GetNextRecordInfo(type, version, length)) {
        const bool handled =
            CheckpointManager::LoadRecord(serialization, type, version, length) ||
            RespawnPolicyManager::LoadRecord(serialization, type, version, length) ||
            TextManager::LoadRecord(serialization, type, version, length);
        if (!handled) {
            logger::warn(
                "Ignoring unknown Trick Death co-save record type={:08X}, version={}, length={}.",
                type,
                version,
                length);
        }
    }
}

void SerializationManager::Revert(SKSE::SerializationInterface*) {
    CheckpointManager::Revert();
    RespawnPolicyManager::Revert();
    TextManager::Revert();
}

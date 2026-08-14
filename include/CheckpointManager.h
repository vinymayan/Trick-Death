#pragma once

namespace CheckpointManager {
    void RegisterEvents();
    void CaptureAfterSleep();
    bool HasCheckpoint();
    bool MovePlayerToCheckpoint();
    void Save(SKSE::SerializationInterface* serialization);
    void Load(SKSE::SerializationInterface* serialization);
    void Revert(SKSE::SerializationInterface* serialization);
}

#pragma once

namespace SerializationManager {
    void Save(SKSE::SerializationInterface* serialization);
    void Load(SKSE::SerializationInterface* serialization);
    void Revert(SKSE::SerializationInterface* serialization);
}

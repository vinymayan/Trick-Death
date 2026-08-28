#include "IntegrationEvents.h"
#include "TrickDeathAPI.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace {
    static_assert(sizeof(IntegrationEvents::EventData) == sizeof(TRICK_DEATH_API::EventData));
    static_assert(
        offsetof(IntegrationEvents::EventData, name) ==
        offsetof(TRICK_DEATH_API::EventData, name));
    std::mutex eventLock;
    std::string lastEventDescription{ "No integration event emitted yet" };

    const char* GetPapyrusEventName(IntegrationEvents::Type type) {
        switch (type) {
        case IntegrationEvents::Type::LastSleepChanged:
            return "TrickDeath_LastSleepChanged";
        case IntegrationEvents::Type::CheckpointChanged:
            return "TrickDeath_CheckpointChanged";
        case IntegrationEvents::Type::RespawnSelected:
            return "TrickDeath_RespawnSelected";
        case IntegrationEvents::Type::RespawnCompleted:
            return "TrickDeath_RespawnCompleted";
        }
        return "TrickDeath_Unknown";
    }

    void Send(IntegrationEvents::EventData& data) {
        const auto eventName = GetPapyrusEventName(data.type);
        const auto text = data.name.data();

        if (auto messaging = SKSE::GetMessagingInterface()) {
            messaging->Dispatch(
                static_cast<std::uint32_t>(data.type),
                std::addressof(data),
                static_cast<std::uint32_t>(sizeof(data)),
                nullptr);
        }
        if (auto source = SKSE::GetModCallbackEventSource()) {
            SKSE::ModCallbackEvent event{
                RE::BSFixedString(eventName),
                RE::BSFixedString(text),
                static_cast<float>(Respawn::ToMask(data.option)),
                nullptr
            };
            source->SendEvent(std::addressof(event));
        }

        std::scoped_lock lock(eventLock);
        lastEventDescription = fmt::format(
            "{} option={} marker={:08X} owner={:08X} name='{}'",
            eventName,
            Respawn::ToString(data.option),
            data.markerFormID,
            data.ownerFormID,
            text);
    }

    IntegrationEvents::EventData MakeEvent(
        IntegrationEvents::Type type,
        Respawn::Option option,
        RE::FormID markerFormID,
        RE::FormID ownerFormID,
        std::string_view name)
    {
        IntegrationEvents::EventData data;
        data.type = type;
        data.option = option;
        data.markerFormID = markerFormID;
        data.ownerFormID = ownerFormID;
        const auto count = std::min(name.size(), data.name.size() - 1);
        std::memcpy(data.name.data(), name.data(), count);
        data.name[count] = '\0';
        return data;
    }
}

void IntegrationEvents::SendLastSleepChanged(RE::FormID markerFormID, std::string_view name) {
    auto data = MakeEvent(Type::LastSleepChanged, Respawn::Option::LastSleep, markerFormID, 0, name);
    Send(data);
}

void IntegrationEvents::SendCheckpointChanged(
    RE::FormID markerFormID,
    RE::FormID ownerFormID,
    std::string_view name)
{
    auto data = MakeEvent(Type::CheckpointChanged, Respawn::Option::LastCheckpoint, markerFormID, ownerFormID, name);
    Send(data);
}

void IntegrationEvents::SendRespawnSelected(Respawn::Option option) {
    auto data = MakeEvent(Type::RespawnSelected, option, 0, 0, Respawn::ToString(option));
    Send(data);
}

void IntegrationEvents::SendRespawnCompleted(Respawn::Option option) {
    auto data = MakeEvent(Type::RespawnCompleted, option, 0, 0, Respawn::ToString(option));
    Send(data);
}

std::string IntegrationEvents::GetLastEventDescription() {
    std::scoped_lock lock(eventLock);
    return lastEventDescription;
}

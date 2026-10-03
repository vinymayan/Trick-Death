#pragma once

#include <atomic>

class PlayerAnimationSink final :
    public RE::BSTEventSink<RE::BSAnimationGraphEvent>,
    public RE::BSTEventSink<RE::TESHitEvent>,
    public RE::BSTEventSink<RE::TESDeathEvent>,
    public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
    public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent> {
public:
    static PlayerAnimationSink* GetSingleton();

    void Install();
    void Reconnect();

    RE::BSEventNotifyControl ProcessEvent(
        const RE::BSAnimationGraphEvent* event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESHitEvent* event,
        RE::BSTEventSource<RE::TESHitEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESDeathEvent* event,
        RE::BSTEventSource<RE::TESDeathEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESObjectLoadedEvent* event,
        RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESSwitchRaceCompleteEvent* event,
        RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override;

private:
    void ScheduleRegistration(std::uint32_t attempt);

    bool installed_ = false;
    std::atomic_bool registrationScheduled_{ false };
};

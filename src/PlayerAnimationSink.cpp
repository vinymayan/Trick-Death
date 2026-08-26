#include "PlayerAnimationSink.h"

#include "DeathManager.h"
#include "DelayedDispatcher.h"

#include <chrono>

PlayerAnimationSink* PlayerAnimationSink::GetSingleton() {
    static PlayerAnimationSink singleton;
    return &singleton;
}

void PlayerAnimationSink::Install() {
    if (installed_) {
        Reconnect();
        return;
    }

    if (auto source = RE::ScriptEventSourceHolder::GetSingleton()) {
        source->AddEventSink<RE::TESObjectLoadedEvent>(this);
        source->AddEventSink<RE::TESHitEvent>(this);
        installed_ = true;
        logger::info("Player animation, hit and load listeners installed.");
    } else {
        logger::error("Could not install the player 3D load listener.");
    }
    Reconnect();
}

void PlayerAnimationSink::Reconnect() {
    ScheduleRegistration(0);
}

void PlayerAnimationSink::ScheduleRegistration(std::uint32_t attempt) {
    constexpr std::uint32_t maxAttempts = 20;
    if (attempt > maxAttempts) {
        logger::error("Could not attach the player animation sink after {} attempts.", maxAttempts);
        return;
    }

    bool expected = false;
    if (!registrationScheduled_.compare_exchange_strong(expected, true)) {
        return;
    }

    Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(100), [attempt] {
        SKSE::GetTaskInterface()->AddTask([attempt] {
            auto sink = PlayerAnimationSink::GetSingleton();
            sink->registrationScheduled_.store(false);

            auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                sink->ScheduleRegistration(attempt + 1);
                return;
            }

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            player->GetAnimationGraphManager(graphManager);
            if (!graphManager) {
                sink->ScheduleRegistration(attempt + 1);
                return;
            }

            std::size_t attachedGraphs = 0;
            for (auto& graph : graphManager->graphs) {
                if (!graph) {
                    continue;
                }

                auto* eventSource =
                    static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph.get());
                eventSource->RemoveEventSink(sink);
                eventSource->AddEventSink(sink);
                logger::info(
                    "Player animation sink attached graphIndex={} graphSource=0x{:X}.",
                    attachedGraphs,
                    reinterpret_cast<std::uintptr_t>(eventSource));
                ++attachedGraphs;
            }

            if (attachedGraphs > 0) {
                logger::info("Player animation sink attached to {} graph(s).", attachedGraphs);
            } else {
                logger::warn("Player animation sink registration was rejected; retrying.");
                sink->ScheduleRegistration(attempt + 1);
            }
        });
    });
}

RE::BSEventNotifyControl PlayerAnimationSink::ProcessEvent(
    const RE::BSAnimationGraphEvent* event,
    RE::BSTEventSource<RE::BSAnimationGraphEvent>* eventSource) {
    if (!event || !event->holder) {
        return RE::BSEventNotifyControl::kContinue;
    }
    const auto actor = event->holder->As<RE::Actor>();
    if (!actor || !actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    DeathManager::HandlePlayerAnimationEvent(
        std::string_view(event->tag),
        std::string_view(event->payload),
        reinterpret_cast<std::uintptr_t>(eventSource));
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl PlayerAnimationSink::ProcessEvent(
    const RE::TESHitEvent* event,
    RE::BSTEventSource<RE::TESHitEvent>*) {
    if (event) {
        DeathManager::HandlePlayerHitEvent(*event);
    }
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl PlayerAnimationSink::ProcessEvent(
    const RE::TESObjectLoadedEvent* event,
    RE::BSTEventSource<RE::TESObjectLoadedEvent>*) {
    if (!event || !event->loaded) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const auto player = RE::PlayerCharacter::GetSingleton();
    if (player && event->formID == player->GetFormID()) {
        Reconnect();
    }
    return RE::BSEventNotifyControl::kContinue;
}

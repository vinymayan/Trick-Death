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
        installed_ = true;
        logger::info("Player animation sink load listener installed.");
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

    Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(100), [attempt] {
        SKSE::GetTaskInterface()->AddTask([attempt] {
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                PlayerAnimationSink::GetSingleton()->ScheduleRegistration(attempt + 1);
                return;
            }

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            player->GetAnimationGraphManager(graphManager);
            if (!graphManager) {
                PlayerAnimationSink::GetSingleton()->ScheduleRegistration(attempt + 1);
                return;
            }

            auto sink = PlayerAnimationSink::GetSingleton();
            player->RemoveAnimationGraphEventSink(sink);
            if (player->AddAnimationGraphEventSink(sink)) {
                logger::info("Player animation sink attached.");
            } else {
                logger::warn("Player animation sink registration was rejected; retrying.");
                sink->ScheduleRegistration(attempt + 1);
            }
        });
    });
}

RE::BSEventNotifyControl PlayerAnimationSink::ProcessEvent(
    const RE::BSAnimationGraphEvent* event,
    RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
    if (!event || !event->holder) {
        return RE::BSEventNotifyControl::kContinue;
    }
    const auto actor = event->holder->As<RE::Actor>();
    if (!actor || !actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    DeathManager::HandlePlayerAnimationEvent(std::string_view(event->tag));
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

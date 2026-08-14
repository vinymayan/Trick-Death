#include "DeathManager.h"

#include "CheckpointManager.h"
#include "Configuration.h"
#include "DelayedDispatcher.h"
#include "Prisma.h"

#include <atomic>
#include <chrono>

namespace {
    enum class DeathState : std::uint8_t {
        Alive,
        PendingKillMove,
        Defeated,
        Resolving,
        Recovering,
        LoadingSave
    };

    std::atomic state{ DeathState::Alive };
    std::uint32_t savedEnabledControls = 0;
    std::uint32_t savedStoredControls = 0;
    bool controlsCaptured = false;
    bool ghostCaptured = false;
    bool playerWasGhost = false;
    std::atomic_uint64_t defeatGeneration{ 0 };
    RE::ActorHandle killMoveAttacker;

    using SetGhost_t = void (*)(RE::Actor*, bool);
    REL::Relocation<SetGhost_t> setGhost{ RELOCATION_ID(36287, 37276) };

    constexpr auto GAMEPLAY_CONTROLS = static_cast<RE::UserEvents::USER_EVENT_FLAG>(
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kMovement) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kLooking) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kActivate) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kFighting) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kSneaking) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kMainFour) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom) |
        static_cast<std::uint32_t>(RE::UserEvents::USER_EVENT_FLAG::kJumping));

    void SetCurrentActorValue(RE::ActorValueOwner* owner, RE::ActorValue actorValue, float value) {
        if (!owner) {
            return;
        }
        const auto current = owner->GetActorValue(actorValue);
        owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, actorValue, value - current);
    }

    void SetActorValuePercent(RE::ActorValueOwner* owner, RE::ActorValue actorValue, int percent) {
        if (!owner) {
            return;
        }
        const auto maximum = std::max(0.0F, owner->GetPermanentActorValue(actorValue));
        SetCurrentActorValue(owner, actorValue, maximum * (static_cast<float>(percent) / 100.0F));
    }

    void CaptureAndDisableControls() {
        auto controlMap = RE::ControlMap::GetSingleton();
        if (!controlMap) {
            return;
        }
        if (!controlsCaptured) {
            controlMap->GetControlsState(savedEnabledControls, savedStoredControls);
            controlsCaptured = true;
        }
        controlMap->ToggleControls(GAMEPLAY_CONTROLS, false, false);
    }

    void RestoreControls() {
        if (!controlsCaptured) {
            return;
        }
        if (auto controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetControlsState(savedEnabledControls, savedStoredControls);
        }
        controlsCaptured = false;
    }

    void ApplyTemporaryGhost(RE::PlayerCharacter* player) {
        if (!player) {
            return;
        }
        if (!ghostCaptured) {
            playerWasGhost = player->IsGhost();
            ghostCaptured = true;
        }
        if (!player->IsGhost()) {
            setGhost(player, true);
        }
    }

    void RestoreTemporaryGhost(RE::PlayerCharacter* player) {
        if (!ghostCaptured) {
            return;
        }
        if (player && player->IsGhost() != playerWasGhost) {
            setGhost(player, playerWasGhost);
        }
        ghostCaptured = false;
        playerWasGhost = false;
    }

    bool IsPlayerKillMoveActive(RE::PlayerCharacter* player) {
        if (player && player->IsInKillMove()) {
            return true;
        }
        const auto vats = RE::VATS::GetSingleton();
        return vats && vats->mode == RE::VATS::VATS_MODE::kKillCam;
    }

    void ProtectPlayerForDecision(RE::PlayerCharacter* player, bool applyGhost) {
        auto owner = player->AsActorValueOwner();
        if (owner) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        player->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
        if (applyGhost) {
            ApplyTemporaryGhost(player);
        }
        CaptureAndDisableControls();
    }

    void ApplyBleedoutAndShowMenu(RE::PlayerCharacter* player) {
        if (!player || state.load() != DeathState::Defeated) {
            return;
        }
        player->SetLifeState(RE::ACTOR_LIFE_STATE::kBleedout);
        player->NotifyAnimationGraph("BleedoutStart");
        if (auto processLists = RE::ProcessLists::GetSingleton()) {
            processLists->StopCombatAndAlarmOnActor(player, true);
            logger::info("Existing combat and alarms against the player were cleared once.");
        } else {
            logger::warn("Could not clear combat against the player: ProcessLists is unavailable.");
        }
        Prisma::ShowDeathMenu(CheckpointManager::HasCheckpoint());
        logger::info("Player bleedout and decision UI applied.");
    }

    void FinalizePendingKillMove(std::uint64_t generation, bool forced) {
        if (generation != defeatGeneration.load()) {
            return;
        }

        auto expected = DeathState::PendingKillMove;
        if (!state.compare_exchange_strong(expected, DeathState::Defeated)) {
            return;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            state.store(DeathState::Alive);
            return;
        }

        if (auto vats = RE::VATS::GetSingleton(); vats && vats->mode == RE::VATS::VATS_MODE::kKillCam) {
            vats->SetMode(RE::VATS::VATS_MODE::kNone);
        }

        if (forced) {
            logger::warn("KillMoveEnd was not received; releasing the paired animation through the fallback.");
            player->NotifyAnimationGraph("pairedStop");
            player->NotifyAnimationGraph("IdleForceDefaultState");
            if (const auto attacker = killMoveAttacker.get()) {
                attacker->NotifyAnimationGraph("pairedStop");
            }
        }

        killMoveAttacker.reset();
        ApplyTemporaryGhost(player);
        ApplyBleedoutAndShowMenu(player);
        logger::info("Pending killmove defeat finalized{}.", forced ? " by fallback" : " from animation event");
    }

    void ScheduleKillMoveFallback(std::uint64_t generation) {
        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::seconds(10), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                FinalizePendingKillMove(generation, true);
            });
        });
    }

    void RestorePlayer(RE::PlayerCharacter* player) {
        if (!player) {
            return;
        }
        player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
        RestoreTemporaryGhost(player);
        player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
        player->NotifyAnimationGraph("BleedoutStop");
        auto owner = player->AsActorValueOwner();
        SetActorValuePercent(owner, RE::ActorValue::kHealth, Settings::Gameplay.healthPercent);
        SetActorValuePercent(owner, RE::ActorValue::kMagicka, Settings::Gameplay.magickaPercent);
        SetActorValuePercent(owner, RE::ActorValue::kStamina, Settings::Gameplay.staminaPercent);
        RestoreControls();
        player->NotifyAnimationGraph("TrickDeathRevive");
        logger::info("Player respawn completed; animation event 'TrickDeathRevive' sent.");
    }

    void FinishRecoveryLater() {
        const auto seconds = Settings::Gameplay.invulnerabilitySeconds;
        if (seconds <= 0) {
            state.store(DeathState::Alive);
            return;
        }

        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::seconds(seconds), [] {
            SKSE::GetTaskInterface()->AddTask([] {
                auto expected = DeathState::Recovering;
                state.compare_exchange_strong(expected, DeathState::Alive);
            });
        });
    }

    void Respawn(bool atCheckpoint) {
        auto expected = DeathState::Defeated;
        if (!state.compare_exchange_strong(expected, DeathState::Resolving)) {
            return;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            state.store(DeathState::Defeated);
            Prisma::ShowError("Player is not available.");
            return;
        }
        if (atCheckpoint && !CheckpointManager::MovePlayerToCheckpoint()) {
            state.store(DeathState::Defeated);
            Prisma::ShowError("No valid sleep checkpoint is available.");
            return;
        }

        Prisma::Hide();
        RestorePlayer(player);
        state.store(DeathState::Recovering);
        FinishRecoveryLater();
    }

    void LoadLastSave() {
        auto expected = DeathState::Defeated;
        if (!state.compare_exchange_strong(expected, DeathState::LoadingSave)) {
            return;
        }

        Prisma::Hide();
        RestoreTemporaryGhost(RE::PlayerCharacter::GetSingleton());
        RestoreControls();
        auto manager = RE::BGSSaveLoadManager::GetSingleton();
        if (!manager || !manager->LoadMostRecentSaveGame()) {
            state.store(DeathState::Defeated);
            ApplyTemporaryGhost(RE::PlayerCharacter::GetSingleton());
            CaptureAndDisableControls();
            Prisma::ShowDeathMenu(CheckpointManager::HasCheckpoint());
            Prisma::ShowError("Could not load the most recent save.");
        }
    }
}

bool DeathManager::TryInterceptDeath(RE::PlayerCharacter* player, RE::Actor* attacker) {
    if (!player || !Settings::Gameplay.enabled) {
        return false;
    }

    const auto current = state.load();
    if (current != DeathState::Alive) {
        if (auto owner = player->AsActorValueOwner()) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        return true;
    }

    if (!Prisma::CanShow()) {
        logger::error("Death was not intercepted because PrismaUI is unavailable or not ready.");
        return false;
    }

    const bool inKillMove = IsPlayerKillMoveActive(player);
    auto expected = DeathState::Alive;
    const auto nextState = inKillMove ? DeathState::PendingKillMove : DeathState::Defeated;
    if (!state.compare_exchange_strong(expected, nextState)) {
        return true;
    }

    const auto generation = defeatGeneration.fetch_add(1) + 1;
    if (inKillMove) {
        ProtectPlayerForDecision(player, false);
        killMoveAttacker = attacker ? attacker->GetHandle() : RE::ActorHandle{};
        logger::info("Player defeat intercepted during killcam; waiting for the player animation event.");
        ScheduleKillMoveFallback(generation);
    } else {
        ProtectPlayerForDecision(player, true);
        ApplyBleedoutAndShowMenu(player);
        logger::info("Player defeat intercepted outside a killcam.");
    }
    return true;
}

void DeathManager::HandlePlayerAnimationEvent(std::string_view eventName) {
    if (state.load() != DeathState::PendingKillMove) {
        return;
    }

    const bool completesKillMove =
        eventName == "KillMoveEnd" ||
        eventName == "NPCKillMoveEnd" ||
        eventName == "2_KillMoveEnd" ||
        eventName == "pairedStop" ||
        eventName == "NPCpairedStop" ||
        eventName == "2_pairedStop";
    if (!completesKillMove) {
        return;
    }

    const auto generation = defeatGeneration.load();
    const std::string eventCopy(eventName);
    SKSE::GetTaskInterface()->AddTask([generation, eventCopy] {
        logger::info("Player animation event '{}' completed the pending killmove.", eventCopy);
        FinalizePendingKillMove(generation, false);
    });
}

bool DeathManager::IsDamageBlocked() {
    return state.load() != DeathState::Alive;
}

bool DeathManager::IsMenuOpen() {
    return state.load() == DeathState::Defeated;
}

void DeathManager::HandleUIAction(std::string_view action) {
    const std::string actionCopy(action);
    SKSE::GetTaskInterface()->AddTask([actionCopy] {
        if (actionCopy == "respawn_checkpoint") {
            Respawn(true);
        } else if (actionCopy == "respawn_here") {
            Respawn(false);
        } else if (actionCopy == "load_last_save") {
            LoadLastSave();
        } else {
            logger::warn("Rejected unknown death menu action: {}", actionCopy);
        }
    });
}

void DeathManager::Reset() {
    defeatGeneration.fetch_add(1);
    killMoveAttacker.reset();
    Prisma::Hide();
    const auto previousState = state.exchange(DeathState::Alive);
    auto player = RE::PlayerCharacter::GetSingleton();
    RestoreTemporaryGhost(player);
    if (player && previousState != DeathState::Alive) {
        player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
    }
    RestoreControls();
}

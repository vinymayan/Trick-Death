#include "DeathManager.h"

#include "CheckpointManager.h"
#include "CurrentSaveManager.h"
#include "Configuration.h"
#include "DelayedDispatcher.h"
#include "DeathTrackerManager.h"
#include "IntegrationEvents.h"
#include "MoreRagdollClient.h"
#include "Prisma.h"
#include "RespawnPolicyManager.h"
#include "RespawnCostManager.h"
#include "RespawnTypes.h"
#include "TextManager.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace {
    enum class DeathState : std::uint8_t {
        Alive,
        PendingKillMove,
        Defeated,
        Resolving,
        Recovering,
        LoadingSave
    };

    enum class DefeatCause : std::uint8_t {
        None,
        Standard,
        Projectile,
        KillMove,
        LethalFall
    };

    enum class DeathTextCause : std::uint8_t {
        Generic,
        Fall,
        Sword,
        Magic
    };

    enum class DefeatRecoveryMode : std::uint8_t {
        None,
        Bleedout,
        Ragdoll
    };

    enum class AppliedDamageOrigin : std::uint8_t {
        None,
        StandardHit,
        ProjectileImpact,
        FallPhysics
    };

    struct PlayerHitContext {
        bool valid{ false };
        std::uint64_t serial{ 0 };
        RE::FormID attacker{ 0 };
        RE::FormID source{ 0 };
        RE::FormType sourceType{ RE::FormType::None };
        bool projectileImpact{ false };
    };

    struct FallDamageContext {
        bool valid{ false };
        std::uint64_t serial{ 0 };
    };

    struct LethalDamageContext {
        bool valid{ false };
        AppliedDamageOrigin origin{ AppliedDamageOrigin::None };
        RE::FormID attacker{ 0 };
        RE::FormID source{ 0 };
        RE::FormType sourceType{ RE::FormType::None };
    };

    std::atomic state{ DeathState::Alive };
    std::atomic_bool damageProtectionActive{ false };
    std::atomic<float> protectedHealth{ 1.0F };
    std::atomic_int64_t damageProtectionDeadlineMilliseconds{ 0 };
    std::uint32_t savedEnabledControls = 0;
    std::uint32_t savedStoredControls = 0;
    bool controlsCaptured = false;
    bool ghostCaptured = false;
    bool playerWasGhost = false;
    std::atomic activeDefeatCause{ DefeatCause::None };
    std::atomic activeRecoveryMode{ DefeatRecoveryMode::None };
    std::atomic_uint64_t defeatGeneration{ 0 };
    std::atomic_bool awaitingRagdollRecovery{ false };
    std::atomic_bool recoveryGetUpStarted{ false };
    std::atomic_bool recoveryControllerAdded{ false };
    std::atomic_bool recoveryGetUpFinished{ false };
    std::atomic_bool recoveryCompletionScheduled{ false };
    std::atomic_bool adoptPendingNativeRagdoll{ false };
    std::atomic_uint32_t activeRespawnMask{ 0 };
    std::atomic<Respawn::Option> activeRespawnOption{ Respawn::Option::None };
    RE::ActorHandle killMoveAttacker;

    std::atomic activeDeathTextCause{ DeathTextCause::Generic };
    std::mutex presentationLock;
    std::string activeBackgroundTemplate;
    std::string activeDeathSourceName;

    std::mutex damageContextLock;
    std::uint64_t nextDamageContextSerial{ 0 };
    PlayerHitContext pendingPlayerHit;
    FallDamageContext pendingFallDamage;
    LethalDamageContext pendingLethalDamage;

    void FinishRecoveryLater();

    std::int64_t GetSteadyMilliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    const char* ToString(DeathState value) {
        switch (value) {
        case DeathState::Alive:
            return "Alive";
        case DeathState::PendingKillMove:
            return "PendingKillMove";
        case DeathState::Defeated:
            return "Defeated";
        case DeathState::Resolving:
            return "Resolving";
        case DeathState::Recovering:
            return "Recovering";
        case DeathState::LoadingSave:
            return "LoadingSave";
        }
        return "Unknown";
    }

    const char* ToString(DefeatCause value) {
        switch (value) {
        case DefeatCause::None:
            return "None";
        case DefeatCause::Standard:
            return "Standard";
        case DefeatCause::Projectile:
            return "Projectile";
        case DefeatCause::KillMove:
            return "KillMove";
        case DefeatCause::LethalFall:
            return "LethalFall";
        }
        return "Unknown";
    }

    const char* ToString(DeathTextCause value) {
        switch (value) {
        case DeathTextCause::Generic:
            return "generic";
        case DeathTextCause::Fall:
            return "fall";
        case DeathTextCause::Sword:
            return "sword";
        case DeathTextCause::Magic:
            return "magic";
        }
        return "generic";
    }

    bool IsProjectileFormType(RE::FormType formType) {
        switch (formType) {
        case RE::FormType::MagicEffect:
        case RE::FormType::Spell:
        case RE::FormType::Scroll:
        case RE::FormType::Ammo:
        case RE::FormType::Projectile:
        case RE::FormType::ProjectileMissile:
        case RE::FormType::ProjectileArrow:
        case RE::FormType::ProjectileGrenade:
        case RE::FormType::ProjectileBeam:
        case RE::FormType::ProjectileFlame:
        case RE::FormType::ProjectileCone:
        case RE::FormType::ProjectileBarrier:
            return true;
        default:
            return false;
        }
    }

    void ClearAppliedDamageContexts() {
        std::scoped_lock lock(damageContextLock);
        pendingPlayerHit = {};
        pendingFallDamage = {};
        pendingLethalDamage = {};
    }

    LethalDamageContext ConsumeLethalDamageContext(RE::Actor* attacker) {
        std::scoped_lock lock(damageContextLock);
        auto context = pendingLethalDamage;

        // KillImpl normally follows the post-applied health hook. Some external
        // damage implementations skip that hook, so consume the newest concrete
        // engine event as a causal fallback, never by elapsed wall-clock time.
        if (!context.valid) {
            const bool hitIsNewest =
                pendingPlayerHit.valid &&
                (!pendingFallDamage.valid || pendingPlayerHit.serial >= pendingFallDamage.serial);
            if (hitIsNewest) {
                context.valid = true;
                context.origin = pendingPlayerHit.projectileImpact ?
                    AppliedDamageOrigin::ProjectileImpact : AppliedDamageOrigin::StandardHit;
                context.attacker = pendingPlayerHit.attacker;
                context.source = pendingPlayerHit.source;
                context.sourceType = pendingPlayerHit.sourceType;
            } else if (pendingFallDamage.valid && !attacker) {
                context.valid = true;
                context.origin = AppliedDamageOrigin::FallPhysics;
            }
        }

        pendingPlayerHit = {};
        pendingFallDamage = {};
        pendingLethalDamage = {};
        return context;
    }

    bool IsMagicSourceType(RE::FormType type) {
        return type == RE::FormType::MagicEffect ||
            type == RE::FormType::Spell ||
            type == RE::FormType::Scroll ||
            type == RE::FormType::Enchantment;
    }

    struct DeathPresentation {
        DeathTextCause cause{ DeathTextCause::Generic };
        std::string sourceName;
    };

    DeathPresentation ClassifyDeathPresentation(const LethalDamageContext& damage) {
        DeathPresentation result;
        if (damage.origin == AppliedDamageOrigin::FallPhysics) {
            result.cause = DeathTextCause::Fall;
            return result;
        }

        auto* source = damage.source ? RE::TESForm::LookupByID(damage.source) : nullptr;
        if (source) {
            if (const auto name = source->GetName(); name && name[0] != '\0') {
                result.sourceName = name;
            }
            if (auto* weapon = source->As<RE::TESObjectWEAP>()) {
                const auto weaponType = weapon->GetWeaponType();
                if (weaponType == RE::WEAPON_TYPE::kOneHandSword ||
                    weaponType == RE::WEAPON_TYPE::kTwoHandSword) {
                    result.cause = DeathTextCause::Sword;
                    return result;
                }
                if (weaponType == RE::WEAPON_TYPE::kStaff) {
                    result.cause = DeathTextCause::Magic;
                    return result;
                }
            }
        }
        if (IsMagicSourceType(damage.sourceType) ||
            (source && IsMagicSourceType(source->GetFormType()))) {
            result.cause = DeathTextCause::Magic;
        }
        return result;
    }

    void SelectBackgroundTemplate(DeathTextCause cause) {
        auto candidates = ModMenu::GetLocList(fmt::format("death_messages.{}", ToString(cause)));
        if (candidates.empty() && cause != DeathTextCause::Generic) {
            candidates = ModMenu::GetLocList("death_messages.generic");
        }
        std::string selected;
        if (!candidates.empty()) {
            selected = candidates[defeatGeneration.load() % candidates.size()];
        }
        std::scoped_lock lock(presentationLock);
        activeBackgroundTemplate = std::move(selected);
    }

    void ApplyDeathPresentationContext(const DeathPresentation& presentation) {
        activeDeathTextCause.store(presentation.cause);
        {
            std::scoped_lock lock(presentationLock);
            activeDeathSourceName = presentation.sourceName;
        }
        TextManager::SetRuntimeVariable("death.cause", ToString(presentation.cause));
        TextManager::SetRuntimeVariable(
            "death.weapon",
            presentation.cause == DeathTextCause::Sword ? presentation.sourceName : "");
        TextManager::SetRuntimeVariable(
            "death.magic",
            presentation.cause == DeathTextCause::Magic ? presentation.sourceName : "");
        TextManager::SetRuntimeVariable("death.count", std::to_string(DeathTrackerManager::GetCount()));
        SelectBackgroundTemplate(presentation.cause);
    }

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

    void BeginDamageProtection(RE::PlayerCharacter* player, bool applyGhost) {
        damageProtectionActive.store(true);
        damageProtectionDeadlineMilliseconds.store(0);
        if (player) {
            if (auto* owner = player->AsActorValueOwner()) {
                protectedHealth.store(std::max(1.0F, owner->GetActorValue(RE::ActorValue::kHealth)));
            }
            if (applyGhost) {
                ApplyTemporaryGhost(player);
            }
        }
    }

    void EndDamageProtection(RE::PlayerCharacter* player, std::string_view) {
        RestoreTemporaryGhost(player);
        damageProtectionDeadlineMilliseconds.store(0);
        damageProtectionActive.store(false);
        protectedHealth.store(1.0F);
    }

    bool IsPlayerKillMoveActive(RE::PlayerCharacter* player) {
        if (player && player->IsInKillMove()) {
            return true;
        }
        const auto vats = RE::VATS::GetSingleton();
        return vats && vats->mode == RE::VATS::VATS_MODE::kKillCam;
    }

    bool IsAlreadyRagdolled(RE::Actor* actor) {
        if (!actor) {
            return false;
        }
        if (actor->IsInRagdollState()) {
            return true;
        }

        const auto actorState = actor->AsActorState();
        if (!actorState) {
            return false;
        }
        switch (actorState->GetKnockState()) {
        case RE::KNOCK_STATE_ENUM::kExplode:
        case RE::KNOCK_STATE_ENUM::kExplodeLeadIn:
        case RE::KNOCK_STATE_ENUM::kOut:
        case RE::KNOCK_STATE_ENUM::kOutLeadIn:
        case RE::KNOCK_STATE_ENUM::kQueued:
        case RE::KNOCK_STATE_ENUM::kDown:
        case RE::KNOCK_STATE_ENUM::kWaitForTaskQueue:
            return true;
        default:
            return false;
        }
    }

    void ProtectPlayerForDecision(RE::PlayerCharacter* player, bool applyGhost) {
        auto owner = player->AsActorValueOwner();
        if (owner) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        player->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
        BeginDamageProtection(player, applyGhost);
        CaptureAndDisableControls();
    }

    void UpdateTextContext(
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        const DeathPresentation& presentation)
    {
        TextManager::ClearRuntimeVariables();
        TextManager::SetRuntimeVariable(
            "killer.name",
            attacker && attacker->GetName() ? attacker->GetName() : "");
        std::string locationName;
        if (auto cell = player ? player->GetParentCell() : nullptr) {
            if (auto location = cell->GetLocation()) {
                if (const auto name = location->GetFullName(); name && name[0] != '\0') {
                    locationName = name;
                }
            }
            if (locationName.empty()) {
                if (const auto name = cell->GetFullName(); name && name[0] != '\0') {
                    locationName = name;
                }
            }
        }
        TextManager::SetRuntimeVariable("death.location", std::move(locationName));
        ApplyDeathPresentationContext(presentation);
    }

    void ShowDecisionUI(RE::PlayerCharacter* player) {
        if (!player || state.load() != DeathState::Defeated) {
            return;
        }
        if (auto processLists = RE::ProcessLists::GetSingleton()) {
            processLists->StopCombatAndAlarmOnActor(player, true);
        } else {
            logger::warn("Could not clear combat against the player: ProcessLists is unavailable.");
        }
        Prisma::ShowDeathMenu(activeRespawnMask.load());
    }

    void ApplyDefeatedPoseAndShowMenu(RE::PlayerCharacter* player) {
        if (!player || state.load() != DeathState::Defeated) {
            return;
        }

        const auto selectedPose = static_cast<Settings::DefeatPose>(Settings::Gameplay.defeatPose);
        const bool nativeRagdollPending = adoptPendingNativeRagdoll.exchange(false);
        if (selectedPose == Settings::DefeatPose::kRagdoll) {
            const bool alreadyRagdolled = IsAlreadyRagdolled(player);
            const bool adoptOnly = nativeRagdollPending || alreadyRagdolled;
            player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
            MoreRagdollClient::Enable(player, adoptOnly);
            activeRecoveryMode.store(DefeatRecoveryMode::Ragdoll);
        } else {
            player->SetLifeState(RE::ACTOR_LIFE_STATE::kBleedout);
            player->NotifyAnimationGraph("BleedoutStart");
            activeRecoveryMode.store(DefeatRecoveryMode::Bleedout);
        }

        ShowDecisionUI(player);
    }

    void ScheduleImpactDefeatPose(
        std::uint64_t generation,
        RE::ActorHandle playerHandle,
        DefeatCause cause) {
        // Run after the current engine damage/impact stack unwinds. This is a
        // task-queue boundary, not an elapsed-time classification window.
        SKSE::GetTaskInterface()->AddTask([generation, playerHandle, cause] {
            if (generation != defeatGeneration.load() || state.load() != DeathState::Defeated) {
                return;
            }

            const auto actor = playerHandle.get();
            auto* player = actor && actor->IsPlayerRef() ?
                static_cast<RE::PlayerCharacter*>(actor.get()) : nullptr;
            if (!player) {
                logger::error(
                    "Could not finalize {} defeat after the engine damage stack unwound: "
                    "the player handle expired.",
                    ToString(cause));
                state.store(DeathState::Alive);
                EndDamageProtection(nullptr, "impact pose failed because player expired");
                RestoreControls();
                return;
            }

            const bool nativeRagdollAfterImpact = IsAlreadyRagdolled(player);
            adoptPendingNativeRagdoll.store(nativeRagdollAfterImpact);
            ApplyDefeatedPoseAndShowMenu(player);
        });
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
            EndDamageProtection(nullptr, "killmove finalization lost player");
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
        ApplyDefeatedPoseAndShowMenu(player);
    }

    void ScheduleKillMoveFallback(std::uint64_t generation) {
        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::seconds(10), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                FinalizePendingKillMove(generation, true);
            });
        });
    }

    bool HasCharacterControllerInWorld(RE::PlayerCharacter* player) {
        const auto controller = player ? player->GetCharController() : nullptr;
        return controller && controller->GetHavokWorld();
    }

    bool RefreshControllerConfirmation(RE::PlayerCharacter* player) {
        const bool inWorld = HasCharacterControllerInWorld(player);
        if (inWorld) {
            recoveryControllerAdded.store(true);
        }
        return inWorld;
    }

    void AttemptNativeRagdollRepair(RE::PlayerCharacter* player, std::string_view context) {
        if (!player) {
            return;
        }
        const bool controllerBefore = HasCharacterControllerInWorld(player);
        player->PotentiallyFixRagdollState();
        const bool controllerAfter = RefreshControllerConfirmation(player);
        logger::warn(
            "Requested native ragdoll-state repair at '{}': controllerBefore={}, controllerAfter={}",
            context,
            controllerBefore,
            controllerAfter);
    }

    bool IsDestinationRespawn(Respawn::Option option) {
        return option == Respawn::Option::LastSleep ||
            option == Respawn::Option::LastCheckpoint;
    }

    bool HasRespawnDestination(Respawn::Option option) {
        if (option == Respawn::Option::LastSleep) {
            return CheckpointManager::HasLastSleep();
        }
        if (option == Respawn::Option::LastCheckpoint) {
            return CheckpointManager::HasCheckpoint();
        }
        return true;
    }

    bool MoveToRespawnDestination(Respawn::Option option) {
        if (option == Respawn::Option::LastSleep) {
            return CheckpointManager::MovePlayerToLastSleep();
        }
        if (option == Respawn::Option::LastCheckpoint) {
            return CheckpointManager::MovePlayerToCheckpoint();
        }
        return true;
    }

    void CompleteRespawn(std::uint64_t generation, std::string reason) {
        if (generation != defeatGeneration.load() || state.load() != DeathState::Resolving) {
            return;
        }

        awaitingRagdollRecovery.store(false);
        recoveryCompletionScheduled.store(false);
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            logger::error("Could not complete respawn after '{}': player is unavailable.", reason);
            state.store(DeathState::Alive);
            EndDamageProtection(nullptr, "respawn completion lost player");
            RestoreControls();
            return;
        }

        const auto completedOption = activeRespawnOption.load();
        if (IsDestinationRespawn(completedOption)) {
            const bool moved = MoveToRespawnDestination(completedOption);
            if (!moved) {
                logger::error(
                    "The '{}' destination was valid when selected but could not be resolved "
                    "after recovery; completing at the recovered current position.",
                    Respawn::ToString(completedOption));
            }
        }
        RestoreControls();
        player->NotifyAnimationGraph("TrickDeathRespawn");
        state.store(DeathState::Recovering);
        IntegrationEvents::SendRespawnCompleted(completedOption);
        FinishRecoveryLater();
    }

    void ScheduleRagdollRecoveryCompletion(
        std::uint64_t generation,
        std::chrono::milliseconds delay,
        std::string reason,
        bool requireController) {
        bool expected = false;
        if (!recoveryCompletionScheduled.compare_exchange_strong(expected, true)) {
            return;
        }

        Utils::DelayedDispatcher::Get().PostDelayed(
            delay,
            [generation, reason = std::move(reason), requireController] {
                SKSE::GetTaskInterface()->AddTask(
                    [generation, reason, requireController] {
                        recoveryCompletionScheduled.store(false);
                        if (generation != defeatGeneration.load() ||
                            state.load() != DeathState::Resolving ||
                            !awaitingRagdollRecovery.load()) {
                            return;
                        }
                        auto player = RE::PlayerCharacter::GetSingleton();
                        if (requireController && !RefreshControllerConfirmation(player)) {
                            logger::warn(
                                "Deferred ragdoll recovery completion after '{}': "
                                "the character controller is not in a Havok world.",
                                reason);
                            return;
                        }
                        CompleteRespawn(generation, reason);
                    });
            });
    }

    void ScheduleRagdollRecoveryFallback(std::uint64_t generation) {
        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(750), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                if (generation != defeatGeneration.load() ||
                    state.load() != DeathState::Resolving ||
                    !awaitingRagdollRecovery.load()) {
                    return;
                }

                auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                if (RefreshControllerConfirmation(player)) {
                    logger::warn(
                        "GetUpEnd/GetUpExit was not observed, but the character controller returned; "
                        "completing recovery after the get-up grace period.");
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(50),
                        "character controller grace fallback",
                        true);
                    return;
                }

                AttemptNativeRagdollRepair(player, "first fallback");

                if (!recoveryGetUpStarted.load()) {
                    const bool resent = player->NotifyAnimationGraph("GetUpBegin");
                    logger::warn(
                        "The ragdoll get-up sequence did not emit GetUpStart; resent GetUpBegin once "
                        "(accepted={}).",
                        resent);
                } else {
                    logger::warn(
                        "GetUpStart was observed, but AddCharacterControllerToWorld is still pending; "
                        "waiting for the vanilla sequence before using the direct fallback.");
                }
            });
        });

        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(1500), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                if (generation != defeatGeneration.load() ||
                    state.load() != DeathState::Resolving ||
                    !awaitingRagdollRecovery.load()) {
                    return;
                }

                auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                if (RefreshControllerConfirmation(player)) {
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(50),
                        "late character controller confirmation",
                        true);
                    return;
                }

                AttemptNativeRagdollRepair(player, "direct controller fallback");
                if (RefreshControllerConfirmation(player)) {
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(50),
                        "native ragdoll-state repair",
                        true);
                    return;
                }

                const bool controllerEventAccepted =
                    player->NotifyAnimationGraph("AddCharacterControllerToWorld");
                logger::warn(
                    "The ragdoll get-up sequence did not restore the character controller; sent one "
                    "direct AddCharacterControllerToWorld fallback (accepted={}).",
                    controllerEventAccepted);
            });
        });

        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(2250), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                if (generation != defeatGeneration.load() ||
                    state.load() != DeathState::Resolving ||
                    !awaitingRagdollRecovery.load()) {
                    return;
                }

                auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                if (RefreshControllerConfirmation(player)) {
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(50),
                        "final character controller confirmation",
                        true);
                    return;
                }

                AttemptNativeRagdollRepair(player, "final animation-state fallback");
                const bool normalized = player->NotifyAnimationGraph("IdleForceDefaultState");
                logger::warn(
                    "Ragdoll recovery still lacked a controller after the direct repair; "
                    "sent IdleForceDefaultState (accepted={}).",
                    normalized);
                ScheduleRagdollRecoveryCompletion(
                    generation,
                    std::chrono::milliseconds(250),
                    "final animation-state fallback",
                    true);
            });
        });
    }

    bool RestorePlayer(RE::PlayerCharacter* player) {
        if (!player) {
            return false;
        }
        player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
        player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
        const auto previousRecoveryMode = activeRecoveryMode.exchange(DefeatRecoveryMode::None);
        const bool reviveFromRagdoll = previousRecoveryMode == DefeatRecoveryMode::Ragdoll;
        if (previousRecoveryMode == DefeatRecoveryMode::Bleedout) {
            player->NotifyAnimationGraph("BleedoutStop");
        }
        auto owner = player->AsActorValueOwner();
        const auto healthPercent = Settings::ResolveNumericValue(
            Settings::Gameplay.healthPercent, player, 1, 100);
        const auto magickaPercent = Settings::ResolveNumericValue(
            Settings::Gameplay.magickaPercent, player, 0, 100);
        const auto staminaPercent = Settings::ResolveNumericValue(
            Settings::Gameplay.staminaPercent, player, 0, 100);
        SetActorValuePercent(owner, RE::ActorValue::kHealth, healthPercent);
        SetActorValuePercent(owner, RE::ActorValue::kMagicka, magickaPercent);
        SetActorValuePercent(owner, RE::ActorValue::kStamina, staminaPercent);
        if (owner) {
            protectedHealth.store(std::max(1.0F, owner->GetActorValue(RE::ActorValue::kHealth)));
        }
        if (reviveFromRagdoll) {
            awaitingRagdollRecovery.store(true);
            recoveryGetUpStarted.store(false);
            recoveryControllerAdded.store(false);
            recoveryGetUpFinished.store(false);
            recoveryCompletionScheduled.store(false);
            const auto generation = defeatGeneration.load();
            MoreRagdollClient::Disable(player);
            ScheduleRagdollRecoveryFallback(generation);
            return true;
        }

        RestoreControls();
        player->NotifyAnimationGraph("TrickDeathRespawn");
        return false;
    }

    void FinishRecoveryLater() {
        const auto seconds = Settings::ResolveNumericValue(
            Settings::Gameplay.invulnerabilitySeconds,
            RE::PlayerCharacter::GetSingleton(),
            0,
            30);
        const auto generation = defeatGeneration.load();
        if (seconds <= 0) {
            auto expected = DeathState::Recovering;
            if (state.compare_exchange_strong(expected, DeathState::Alive)) {
                EndDamageProtection(RE::PlayerCharacter::GetSingleton(), "recovery completed with zero grace time");
                activeDefeatCause.store(DefeatCause::None);
            }
            return;
        }

        damageProtectionDeadlineMilliseconds.store(
            GetSteadyMilliseconds() + static_cast<std::int64_t>(seconds) * 1000);
        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::seconds(seconds), [generation] {
            SKSE::GetTaskInterface()->AddTask([generation] {
                if (generation != defeatGeneration.load()) {
                    return;
                }
                auto expected = DeathState::Recovering;
                if (state.compare_exchange_strong(expected, DeathState::Alive)) {
                    EndDamageProtection(RE::PlayerCharacter::GetSingleton(), "post-recovery invulnerability elapsed");
                    activeDefeatCause.store(DefeatCause::None);
                }
            });
        });
    }

    void RespawnPlayer(Respawn::Option option) {
        if (!Respawn::Contains(activeRespawnMask.load(), option)) {
            logger::warn("Rejected unavailable respawn option '{}'.", Respawn::ToString(option));
            Prisma::ShowError("That respawn option is currently unavailable.");
            return;
        }
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
        if (IsDestinationRespawn(option) && !HasRespawnDestination(option)) {
            state.store(DeathState::Defeated);
            Prisma::ShowError(
                option == Respawn::Option::LastSleep ?
                    "No valid sleep checkpoint is available." :
                    "No valid external checkpoint is available.");
            return;
        }
        const auto costStatus = RespawnCostManager::GetStatus(option, player);
        if (costStatus.configured && (!costStatus.resourceValid || !costStatus.affordable)) {
            state.store(DeathState::Defeated);
            Prisma::ApplyUISettings();
            Prisma::ShowError(
                costStatus.resourceValid ?
                    "You do not have enough of the required resource." :
                    "The configured respawn resource could not be resolved.");
            return;
        }

        const auto recoveryMode = activeRecoveryMode.load();
        const bool deferDestinationUntilRagdollRecovery =
            IsDestinationRespawn(option) &&
            recoveryMode == DefeatRecoveryMode::Ragdoll;
        if (IsDestinationRespawn(option) && !deferDestinationUntilRagdollRecovery &&
            !MoveToRespawnDestination(option)) {
            state.store(DeathState::Defeated);
            Prisma::ShowError(
                option == Respawn::Option::LastSleep ?
                    "No valid sleep checkpoint is available." :
                    "No valid external checkpoint is available.");
            return;
        }
        if (!RespawnCostManager::Apply(option, player)) {
            state.store(DeathState::Defeated);
            Prisma::ApplyUISettings();
            Prisma::ShowError("The configured respawn resource could not be used or spent.");
            return;
        }
        activeRespawnOption.store(option);
        IntegrationEvents::SendRespawnSelected(option);
        Prisma::Hide();
        const bool waitingForRagdoll = RestorePlayer(player);
        if (!waitingForRagdoll) {
            state.store(DeathState::Recovering);
            IntegrationEvents::SendRespawnCompleted(option);
            FinishRecoveryLater();
        }
    }

    void ReloadCurrentSave() {
        if (!Respawn::Contains(activeRespawnMask.load(), Respawn::Option::ReloadSave)) {
            logger::warn("Rejected unavailable reload-save option.");
            Prisma::ShowError("Reloading the current save is currently unavailable.");
            return;
        }
        const auto saveName = CurrentSaveManager::GetCurrentSaveName();
        auto manager = RE::BGSSaveLoadManager::GetSingleton();
        if (saveName.empty() || !manager) {
            logger::warn(
                "Could not reload current save: name='{}', managerAvailable={}.",
                saveName,
                manager != nullptr);
            Prisma::ShowError("No current save is available to reload.");
            return;
        }
        auto expected = DeathState::Defeated;
        if (!state.compare_exchange_strong(expected, DeathState::LoadingSave)) {
            return;
        }

        activeRespawnOption.store(Respawn::Option::ReloadSave);
        IntegrationEvents::SendRespawnSelected(Respawn::Option::ReloadSave);
        Prisma::Hide();
        RestoreTemporaryGhost(RE::PlayerCharacter::GetSingleton());
        RestoreControls();
        manager->Load(saveName.c_str());
    }
}

void DeathManager::CaptureAppliedPlayerDamage(
    RE::PlayerCharacter* player,
    RE::Actor* attacker) {
    const auto owner = player ? player->AsActorValueOwner() : nullptr;
    const float health = owner ? owner->GetActorValue(RE::ActorValue::kHealth) : 1.0F;
    const bool lethal = std::isfinite(health) && health <= 0.0F;

    LethalDamageContext captured;
    {
        std::scoped_lock lock(damageContextLock);
        if (!lethal) {
            pendingPlayerHit = {};
            pendingFallDamage = {};
            pendingLethalDamage = {};
            return;
        }

        captured.valid = true;
        captured.attacker = attacker ? attacker->GetFormID() : 0;

        const bool hitMatchesAttacker =
            pendingPlayerHit.valid &&
            (!attacker || pendingPlayerHit.attacker == 0 ||
             pendingPlayerHit.attacker == attacker->GetFormID());
        const bool hitIsNewest =
            hitMatchesAttacker &&
            (!pendingFallDamage.valid || pendingPlayerHit.serial >= pendingFallDamage.serial);

        if (hitIsNewest) {
            captured.origin = pendingPlayerHit.projectileImpact ?
                AppliedDamageOrigin::ProjectileImpact : AppliedDamageOrigin::StandardHit;
            captured.attacker = pendingPlayerHit.attacker != 0 ?
                pendingPlayerHit.attacker : captured.attacker;
            captured.source = pendingPlayerHit.source;
            captured.sourceType = pendingPlayerHit.sourceType;
        } else if (pendingFallDamage.valid && !attacker) {
            captured.origin = AppliedDamageOrigin::FallPhysics;
        } else {
            captured.origin = AppliedDamageOrigin::StandardHit;
        }

        pendingLethalDamage = captured;
        pendingPlayerHit = {};
        pendingFallDamage = {};
    }
}

void DeathManager::MarkPlayerFallDamage() {
    FallDamageContext context;
    {
        std::scoped_lock lock(damageContextLock);
        context.valid = true;
        context.serial = ++nextDamageContextSerial;
        pendingFallDamage = context;
    }
}

void DeathManager::HandlePlayerHitEvent(const RE::TESHitEvent& event) {
    const auto targetReference = event.target.get();
    auto* target = targetReference ? targetReference->As<RE::Actor>() : nullptr;
    if (!target || !target->IsPlayerRef()) {
        return;
    }

    auto* player = static_cast<RE::PlayerCharacter*>(target);
    if (IsDamageBlocked()) {
        RepairBlockedPlayerHealth(player);
        return;
    }
    const auto sourceForm = event.source ? RE::TESForm::LookupByID(event.source) : nullptr;
    const auto sourceType = sourceForm ? sourceForm->GetFormType() : RE::FormType::None;
    const auto sourceWeapon = sourceForm ? sourceForm->As<RE::TESObjectWEAP>() : nullptr;
    const bool rangedWeaponProjectile =
        sourceWeapon &&
        !event.flags.any(RE::TESHitEvent::Flag::kBashAttack) &&
        (sourceWeapon->GetWeaponType() == RE::WEAPON_TYPE::kBow ||
         sourceWeapon->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow);
    const auto causeReference = event.cause.get();
    auto* attacker = causeReference ? causeReference->As<RE::Actor>() : nullptr;
    const bool projectileHit =
        event.projectile != 0 || rangedWeaponProjectile || IsProjectileFormType(sourceType);
    PlayerHitContext hitContext;
    {
        std::scoped_lock lock(damageContextLock);
        hitContext.valid = true;
        hitContext.serial = ++nextDamageContextSerial;
        hitContext.attacker = attacker ? attacker->GetFormID() : 0;
        hitContext.source = event.source;
        hitContext.sourceType = sourceType;
        hitContext.projectileImpact = projectileHit;
        pendingPlayerHit = hitContext;
    }
}

bool DeathManager::TryInterceptDeath(
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    bool) {
    if (!player || !Settings::Gameplay.enabled) {
        return false;
    }

    if (player->IsGodMode()) {
        if (auto owner = player->AsActorValueOwner(); owner &&
            owner->GetActorValue(RE::ActorValue::kHealth) < 1.0F) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        return true;
    }

    const auto current = state.load();
    if (current != DeathState::Alive) {
        RepairBlockedPlayerHealth(player);
        return true;
    }

    const auto respawnEvaluation = RespawnPolicyManager::Evaluate();
    if (respawnEvaluation.trickDeathDisabled || respawnEvaluation.availableMask == 0) {
        return false;
    }

    if (!Prisma::CanShow()) {
        logger::error("Death was not intercepted because PrismaUI is unavailable or not ready.");
        return false;
    }

    const bool inKillMove = IsPlayerKillMoveActive(player);
    const bool alreadyRagdolled = IsAlreadyRagdolled(player);
    const auto lethalDamage = ConsumeLethalDamageContext(attacker);
    const bool projectileImpact =
        !inKillMove && lethalDamage.origin == AppliedDamageOrigin::ProjectileImpact;
    const bool lethalFall =
        !inKillMove && lethalDamage.origin == AppliedDamageOrigin::FallPhysics;
    const auto deathPresentation = ClassifyDeathPresentation(lethalDamage);

    // Only adopt physical state that actually exists. A fall/projectile context
    // is a damage cause, not proof that the engine created a ragdoll.
    adoptPendingNativeRagdoll.store(alreadyRagdolled || inKillMove);
    auto expected = DeathState::Alive;
    const auto nextState = inKillMove ? DeathState::PendingKillMove : DeathState::Defeated;
    if (!state.compare_exchange_strong(expected, nextState)) {
        return true;
    }

    activeRespawnMask.store(respawnEvaluation.availableMask);
    activeRespawnOption.store(Respawn::Option::None);

    activeDefeatCause.store(
        inKillMove ? DefeatCause::KillMove :
        projectileImpact ? DefeatCause::Projectile :
        lethalFall ? DefeatCause::LethalFall : DefeatCause::Standard);
    activeRecoveryMode.store(DefeatRecoveryMode::None);

    const auto generation = defeatGeneration.fetch_add(1) + 1;
    DeathTrackerManager::RecordDeath(player);
    UpdateTextContext(player, attacker, deathPresentation);
    ProtectPlayerForDecision(player, !inKillMove);
    player->NotifyAnimationGraph("TrickDeathStarted");
    if (inKillMove) {
        killMoveAttacker = attacker ? attacker->GetHandle() : RE::ActorHandle{};
        ScheduleKillMoveFallback(generation);
    } else if (projectileImpact || lethalFall) {
        const auto impactCause = projectileImpact ? DefeatCause::Projectile : DefeatCause::LethalFall;
        ScheduleImpactDefeatPose(generation, player->GetHandle(), impactCause);
    } else {
        ApplyDefeatedPoseAndShowMenu(player);
    }
    return true;
}

void DeathManager::HandlePlayerAnimationEvent(
    std::string_view eventName,
    std::string_view,
    std::uintptr_t) {
    if (IsDamageBlocked()) {
        RepairBlockedPlayerHealth(RE::PlayerCharacter::GetSingleton());
    }
    const auto currentState = state.load();

    if (currentState == DeathState::Resolving && awaitingRagdollRecovery.load()) {
        const bool getUpStarted = eventName == "GetUpStart";
        const bool controllerAdded = eventName == "AddCharacterControllerToWorld";
        const bool getUpFinished = eventName == "GetUpEnd" || eventName == "GetUpExit";
        if (getUpStarted || controllerAdded || getUpFinished) {
            const auto generation = defeatGeneration.load();
            SKSE::GetTaskInterface()->AddTask(
                [generation, getUpStarted, controllerAdded, getUpFinished] {
                if (generation != defeatGeneration.load() ||
                    state.load() != DeathState::Resolving ||
                    !awaitingRagdollRecovery.load()) {
                    return;
                }

                if (getUpStarted) {
                    recoveryGetUpStarted.store(true);
                }
                if (controllerAdded) {
                    RefreshControllerConfirmation(RE::PlayerCharacter::GetSingleton());
                }
                if (getUpFinished) {
                    recoveryGetUpFinished.store(true);
                }

                if (recoveryControllerAdded.load() && recoveryGetUpFinished.load()) {
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(50),
                        "AddCharacterControllerToWorld plus get-up completion",
                        true);
                } else if (controllerAdded) {
                    ScheduleRagdollRecoveryCompletion(
                        generation,
                        std::chrono::milliseconds(600),
                        "AddCharacterControllerToWorld grace period",
                        true);
                }
            });
        }
    }

    if (currentState != DeathState::PendingKillMove) {
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
    SKSE::GetTaskInterface()->AddTask([generation] {
        FinalizePendingKillMove(generation, false);
    });
}

bool DeathManager::IsDamageBlocked() {
    return damageProtectionActive.load() || state.load() != DeathState::Alive;
}

void DeathManager::RepairBlockedPlayerHealth(RE::PlayerCharacter* player) {
    if (!player || !IsDamageBlocked()) {
        return;
    }
    auto owner = player->AsActorValueOwner();
    if (!owner) {
        return;
    }

    const float before = owner->GetActorValue(RE::ActorValue::kHealth);
    const float minimum = std::max(1.0F, protectedHealth.load());
    if (std::isfinite(before) && before >= minimum) {
        return;
    }
    SetCurrentActorValue(owner, RE::ActorValue::kHealth, minimum);
    logger::warn(
        "Repaired player health after blocked post-applied damage: before={}, after={}; "
        "state={}, cause={}, protectedHealth={}.",
        before,
        owner->GetActorValue(RE::ActorValue::kHealth),
        ToString(state.load()),
        ToString(activeDefeatCause.load()),
        minimum);
}

bool DeathManager::IsMenuOpen() {
    return state.load() == DeathState::Defeated;
}

std::string DeathManager::GetBackgroundText() {
    std::string fallback;
    {
        std::scoped_lock lock(presentationLock);
        fallback = activeBackgroundTemplate;
    }
    return TextManager::ResolveSlot("background_text", fallback);
}

DeathManager::DebugInfo DeathManager::GetDebugInfo() {
    DebugInfo info;
    info.damageProtectionActive = damageProtectionActive.load();
    info.ghostCaptured = ghostCaptured;
    info.playerWasGhost = playerWasGhost;
    info.protectedHealth = protectedHealth.load();
    const auto deadline = damageProtectionDeadlineMilliseconds.load();
    info.protectionRemainingMilliseconds = !info.damageProtectionActive ? 0 :
        deadline <= 0 ? -1 : std::max<std::int64_t>(0, deadline - GetSteadyMilliseconds());
    info.state = ToString(state.load());
    info.physicalCause = ToString(activeDefeatCause.load());
    info.presentationCause = ToString(activeDeathTextCause.load());
    {
        std::scoped_lock lock(presentationLock);
        info.backgroundTemplate = activeBackgroundTemplate;
    }
    return info;
}

bool DeathManager::DebugSelectDeathTextCause(std::string_view cause) {
    DeathTextCause selected;
    if (cause == "generic") {
        selected = DeathTextCause::Generic;
    } else if (cause == "fall") {
        selected = DeathTextCause::Fall;
    } else if (cause == "sword") {
        selected = DeathTextCause::Sword;
    } else if (cause == "magic") {
        selected = DeathTextCause::Magic;
    } else {
        return false;
    }
    activeDeathTextCause.store(selected);
    TextManager::SetRuntimeVariable("death.cause", ToString(selected));
    TextManager::SetRuntimeVariable("death.count", std::to_string(DeathTrackerManager::GetCount()));
    SelectBackgroundTemplate(selected);
    Prisma::ApplyUISettings();
    return true;
}

void DeathManager::HandleUIAction(std::string_view action) {
    const std::string actionCopy(action);
    SKSE::GetTaskInterface()->AddTask([actionCopy] {
        if (actionCopy == "respawn_checkpoint") {
            RespawnPlayer(Respawn::Option::LastCheckpoint);
        } else if (actionCopy == "respawn_last_sleep") {
            RespawnPlayer(Respawn::Option::LastSleep);
        } else if (actionCopy == "respawn_here") {
            RespawnPlayer(Respawn::Option::Here);
        } else if (actionCopy == "reload_save" || actionCopy == "load_last_save") {
            ReloadCurrentSave();
        } else {
            logger::warn("Rejected unknown death menu action: {}", actionCopy);
        }
    });
}

void DeathManager::OnGameLoadFinished(bool success) {
    if (success) {
        Reset();
        return;
    }
    auto expected = DeathState::LoadingSave;
    if (!state.compare_exchange_strong(expected, DeathState::Defeated)) {
        return;
    }
    logger::warn("Reload Save failed; restoring the defeated menu for another selection.");
    ApplyTemporaryGhost(RE::PlayerCharacter::GetSingleton());
    CaptureAndDisableControls();
    Prisma::ShowDeathMenu(activeRespawnMask.load());
    Prisma::ShowError("Could not reload the current save.");
}

void DeathManager::Reset() {
    defeatGeneration.fetch_add(1);
    awaitingRagdollRecovery.store(false);
    recoveryGetUpStarted.store(false);
    recoveryControllerAdded.store(false);
    recoveryGetUpFinished.store(false);
    recoveryCompletionScheduled.store(false);
    adoptPendingNativeRagdoll.store(false);
    activeRespawnMask.store(0);
    activeRespawnOption.store(Respawn::Option::None);
    activeDeathTextCause.store(DeathTextCause::Generic);
    {
        std::scoped_lock lock(presentationLock);
        activeBackgroundTemplate.clear();
        activeDeathSourceName.clear();
    }
    TextManager::ClearRuntimeVariables();
    ClearAppliedDamageContexts();
    killMoveAttacker.reset();
    activeDefeatCause.store(DefeatCause::None);
    const auto previousRecoveryMode = activeRecoveryMode.exchange(DefeatRecoveryMode::None);
    Prisma::Hide();
    const auto previousState = state.exchange(DeathState::Alive);
    auto player = RE::PlayerCharacter::GetSingleton();
    EndDamageProtection(player, "DeathManager reset");
    if (player && previousRecoveryMode == DefeatRecoveryMode::Ragdoll) {
        MoreRagdollClient::Disable(player);
    }
    if (player && previousState != DeathState::Alive) {
        player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
    }
    RestoreControls();
}

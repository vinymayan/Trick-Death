#include "DeathManager.h"

#include "CheckpointManager.h"
#include "Configuration.h"
#include "DelayedDispatcher.h"
#include "IntegrationEvents.h"
#include "MoreRagdollClient.h"
#include "Prisma.h"
#include "RespawnPolicyManager.h"
#include "RespawnTypes.h"
#include "TextManager.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
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

    enum class DefeatRecoveryMode : std::uint8_t {
        None,
        Bleedout,
        VanillaRagdoll,
        MoreRagdoll
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
        RE::FormID projectile{ 0 };
        RE::FormType sourceType{ RE::FormType::None };
        std::uint8_t flags{ 0 };
        bool projectileImpact{ false };
    };

    struct FallDamageContext {
        bool valid{ false };
        std::uint64_t serial{ 0 };
        float fallDistance{ 0.0F };
        float calculatedDamage{ 0.0F };
        bool moveFinishSource{ false };
    };

    struct LethalDamageContext {
        bool valid{ false };
        std::uint64_t damageSequence{ 0 };
        AppliedDamageOrigin origin{ AppliedDamageOrigin::None };
        RE::FormID attacker{ 0 };
        RE::FormID source{ 0 };
        RE::FormID projectile{ 0 };
        RE::FormType sourceType{ RE::FormType::None };
        std::uint8_t hitFlags{ 0 };
    };

    std::atomic state{ DeathState::Alive };
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
    std::atomic_bool fallSequenceActive{ false };
    std::atomic_int64_t lastFallEventMilliseconds{ 0 };
    std::atomic_int64_t lastLandingEventMilliseconds{ 0 };
    std::atomic_uint64_t fallGeneration{ 0 };
    std::atomic_uint64_t diagnosticEventSequence{ 0 };
    std::atomic_int64_t lastDiagnosticAnimationMilliseconds{ 0 };
    std::atomic_uint64_t lethalHitTraceCounter{ 0 };
    std::atomic_uint64_t activeLethalHitTrace{ 0 };
    std::atomic_uint64_t lethalHitTraceLogSequence{ 0 };
    std::atomic_int64_t lethalHitTraceStartMilliseconds{ 0 };
    std::atomic_int64_t lethalHitTraceDeadlineMilliseconds{ 0 };
    std::atomic_int64_t lastLethalHitTraceAnimationMilliseconds{ 0 };
    RE::ActorHandle killMoveAttacker;

    std::mutex damageContextLock;
    std::uint64_t nextDamageContextSerial{ 0 };
    PlayerHitContext pendingPlayerHit;
    FallDamageContext pendingFallDamage;
    LethalDamageContext pendingLethalDamage;

    struct BufferedAnimationEvent {
        std::int64_t milliseconds;
        std::uintptr_t graphSource;
        std::string eventName;
        std::string payload;
    };

    std::mutex animationPreRollLock;
    std::deque<BufferedAnimationEvent> animationPreRoll;

    void FinishRecoveryLater();

    constexpr std::int64_t LETHAL_HIT_TRACE_WINDOW_MILLISECONDS = 8000;
    constexpr std::int64_t ANIMATION_PRE_ROLL_MILLISECONDS = 2000;
    constexpr std::size_t MAX_ANIMATION_PRE_ROLL_EVENTS = 256;

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

    const char* ToString(DefeatRecoveryMode value) {
        switch (value) {
        case DefeatRecoveryMode::None:
            return "None";
        case DefeatRecoveryMode::Bleedout:
            return "Bleedout";
        case DefeatRecoveryMode::VanillaRagdoll:
            return "VanillaRagdoll";
        case DefeatRecoveryMode::MoreRagdoll:
            return "MoreRagdoll";
        }
        return "Unknown";
    }

    const char* ToString(AppliedDamageOrigin value) {
        switch (value) {
        case AppliedDamageOrigin::None:
            return "None";
        case AppliedDamageOrigin::StandardHit:
            return "StandardHit";
        case AppliedDamageOrigin::ProjectileImpact:
            return "ProjectileImpact";
        case AppliedDamageOrigin::FallPhysics:
            return "FallPhysics";
        }
        return "Unknown";
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
                context.projectile = pendingPlayerHit.projectile;
                context.sourceType = pendingPlayerHit.sourceType;
                context.hitFlags = pendingPlayerHit.flags;
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

    std::string NormalizeAnimationEvent(std::string_view eventName) {
        std::string normalized(eventName);
        std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return normalized;
    }

    bool IsDiagnosticAnimationEvent(std::string_view normalizedEvent) {
        constexpr std::array tokens{
            "jump",
            "fall",
            "land",
            "ragdoll",
            "getup",
            "charactercontroller",
            "killmove",
            "paired",
            "bleedout"
        };
        return std::ranges::any_of(tokens, [normalizedEvent](std::string_view token) {
            return normalizedEvent.contains(token);
        });
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

    bool IsLethalHitTraceActive(std::int64_t now = GetSteadyMilliseconds()) {
        return activeLethalHitTrace.load() != 0 &&
               now <= lethalHitTraceDeadlineMilliseconds.load();
    }

    std::int64_t GetLethalHitTraceElapsed(std::int64_t now = GetSteadyMilliseconds()) {
        const auto started = lethalHitTraceStartMilliseconds.load();
        return started > 0 ? now - started : -1;
    }

    void BufferAnimationEvent(
        std::int64_t now,
        std::uintptr_t graphSource,
        std::string_view eventName,
        std::string_view payload) {
        std::scoped_lock lock(animationPreRollLock);
        animationPreRoll.push_back(BufferedAnimationEvent{
            now,
            graphSource,
            std::string(eventName),
            std::string(payload)
        });
        while (!animationPreRoll.empty() &&
               (now - animationPreRoll.front().milliseconds > ANIMATION_PRE_ROLL_MILLISECONDS ||
                animationPreRoll.size() > MAX_ANIMATION_PRE_ROLL_EVENTS)) {
            animationPreRoll.pop_front();
        }
    }

    void DumpAnimationPreRoll(std::uint64_t traceId, std::int64_t traceStarted) {
        std::deque<BufferedAnimationEvent> copy;
        {
            std::scoped_lock lock(animationPreRollLock);
            copy = animationPreRoll;
        }
        logger::info(
            "[LethalTrace][PreRoll] traceId={} bufferedEvents={} windowMs={}.",
            traceId,
            copy.size(),
            ANIMATION_PRE_ROLL_MILLISECONDS);
        for (const auto& event : copy) {
            logger::info(
                "[LethalTrace][PreRoll] traceId={} t={}ms graphSource=0x{:X} "
                "event='{}' payload='{}'.",
                traceId,
                event.milliseconds - traceStarted,
                event.graphSource,
                event.eventName,
                event.payload);
        }
    }

    std::uint64_t BeginOrExtendLethalHitTrace(
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        std::string_view trigger) {
        const auto now = GetSteadyMilliseconds();
        std::uint64_t traceId = activeLethalHitTrace.load();
        if (!IsLethalHitTraceActive(now)) {
            traceId = lethalHitTraceCounter.fetch_add(1) + 1;
            lethalHitTraceStartMilliseconds.store(now);
            lethalHitTraceLogSequence.store(0);
            lastLethalHitTraceAnimationMilliseconds.store(0);
            activeLethalHitTrace.store(traceId);
            logger::info(
                "[LethalTrace][BEGIN] traceId={} trigger={} player={} attacker={} "
                "diagnosticCaptureDurationMs={}; every player animation event will be recorded.",
                traceId,
                trigger,
                player ? fmt::format("{:08X}", player->GetFormID()) : std::string("none"),
                attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
                LETHAL_HIT_TRACE_WINDOW_MILLISECONDS);
            DumpAnimationPreRoll(traceId, now);
        }
        lethalHitTraceDeadlineMilliseconds.store(now + LETHAL_HIT_TRACE_WINDOW_MILLISECONDS);
        return traceId;
    }

    int ReadGraphBool(RE::Actor* actor, const char* name) {
        bool value = false;
        return actor && actor->GetGraphVariableBool(name, value) ? (value ? 1 : 0) : -1;
    }

    int ReadGraphInt(RE::Actor* actor, const char* name) {
        int value = 0;
        return actor && actor->GetGraphVariableInt(name, value) ? value : -1;
    }

    float ReadGraphFloat(RE::Actor* actor, const char* name) {
        float value = 0.0F;
        return actor && actor->GetGraphVariableFloat(name, value) ? value : -99999.0F;
    }

    void LogLethalHitTraceState(
        std::string_view phase,
        RE::PlayerCharacter* player,
        RE::Actor* attacker,
        float damage) {
        const auto now = GetSteadyMilliseconds();
        if (!IsLethalHitTraceActive(now)) {
            return;
        }

        const auto traceId = activeLethalHitTrace.load();
        const auto traceSequence = lethalHitTraceLogSequence.fetch_add(1) + 1;
        const auto owner = player ? player->AsActorValueOwner() : nullptr;
        const auto actorState = player ? player->AsActorState() : nullptr;
        const auto controller = player ? player->GetCharController() : nullptr;
        const auto world = controller ? controller->GetHavokWorld() : nullptr;
        RE::hkVector4 velocity{};
        if (controller) {
            controller->GetLinearVelocityImpl(velocity);
        }

        std::size_t graphCount = 0;
        if (player) {
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            player->GetAnimationGraphManager(graphManager);
            if (graphManager) {
                graphCount = graphManager->graphs.size();
            }
        }

        const auto vats = RE::VATS::GetSingleton();
        logger::info(
            "[LethalTrace][State] traceId={} traceSeq={} t={}ms phase='{}' "
            "modState={} cause={} recovery={} attacker={} damage={} health={} maxHealth={} "
            "lifeState={} dead={} killMove={} vatsMode={} knockState={} nativeRagdoll={} "
            "midair={} loaded3D={} position=({:.3f},{:.3f},{:.3f}) angleZ={:.3f} "
            "controller=0x{:X} world=0x{:X} controllerInWorld={} flags=0x{:08X} "
            "controllerState={} wantedState={} supportCount={} fallTime={:.3f} gravity={:.3f} "
            "velocity=({:.3f},{:.3f},{:.3f}) outVelocity=({:.3f},{:.3f},{:.3f}) "
            "velocityMod=({:.3f},{:.3f},{:.3f}) graphs={} "
            "graphVars=[IsBleedingOut:{},IsBleedingOutTransition:{},bAnimationDriven:{},"
            "bIsSynced:{},bInJumpState:{},iGetUpType:{},Speed:{:.3f},SpeedDamped:{:.3f},"
            "VelocityZ:{:.3f}].",
            traceId,
            traceSequence,
            GetLethalHitTraceElapsed(now),
            phase,
            ToString(state.load()),
            ToString(activeDefeatCause.load()),
            ToString(activeRecoveryMode.load()),
            attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
            damage,
            owner ? owner->GetActorValue(RE::ActorValue::kHealth) : -1.0F,
            owner ? owner->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0F,
            player ? static_cast<int>(player->GetLifeState()) : -1,
            player && player->IsDead(),
            player && player->IsInKillMove(),
            vats ? static_cast<int>(vats->mode) : -1,
            actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
            IsAlreadyRagdolled(player),
            player && player->IsInMidair(),
            player && player->Is3DLoaded(),
            player ? player->GetPositionX() : 0.0F,
            player ? player->GetPositionY() : 0.0F,
            player ? player->GetPositionZ() : 0.0F,
            player ? player->GetAngleZ() : 0.0F,
            reinterpret_cast<std::uintptr_t>(controller),
            reinterpret_cast<std::uintptr_t>(world),
            world != nullptr,
            controller ? controller->flags.underlying() : 0U,
            controller ? static_cast<int>(controller->context.currentState) : -1,
            controller ? static_cast<int>(controller->wantState) : -1,
            controller ? controller->supportCount : -1,
            controller ? controller->fallTime : -1.0F,
            controller ? controller->gravity : -1.0F,
            controller ? velocity.quad.m128_f32[0] : 0.0F,
            controller ? velocity.quad.m128_f32[1] : 0.0F,
            controller ? velocity.quad.m128_f32[2] : 0.0F,
            controller ? controller->outVelocity.quad.m128_f32[0] : 0.0F,
            controller ? controller->outVelocity.quad.m128_f32[1] : 0.0F,
            controller ? controller->outVelocity.quad.m128_f32[2] : 0.0F,
            controller ? controller->velocityMod.quad.m128_f32[0] : 0.0F,
            controller ? controller->velocityMod.quad.m128_f32[1] : 0.0F,
            controller ? controller->velocityMod.quad.m128_f32[2] : 0.0F,
            graphCount,
            ReadGraphBool(player, "IsBleedingOut"),
            ReadGraphBool(player, "IsBleedingOutTransition"),
            ReadGraphBool(player, "bAnimationDriven"),
            ReadGraphBool(player, "bIsSynced"),
            ReadGraphBool(player, "bInJumpState"),
            ReadGraphInt(player, "iGetUpType"),
            ReadGraphFloat(player, "Speed"),
            ReadGraphFloat(player, "SpeedDamped"),
            ReadGraphFloat(player, "VelocityZ"));
    }

    void ScheduleLethalHitTraceSnapshots(
        std::uint64_t traceId,
        RE::ActorHandle playerHandle,
        RE::ActorHandle attackerHandle,
        std::string anchor) {
        constexpr std::array delaysMs{ 1, 5, 10, 16, 33, 50, 100, 150, 250, 500, 750, 1000, 1500, 2500, 4000 };
        for (const int delayMs : delaysMs) {
            Utils::DelayedDispatcher::Get().PostDelayed(
                std::chrono::milliseconds(delayMs),
                [traceId, playerHandle, attackerHandle, anchor, delayMs] {
                    SKSE::GetTaskInterface()->AddTask(
                        [traceId, playerHandle, attackerHandle, anchor, delayMs] {
                            if (activeLethalHitTrace.load() != traceId) {
                                return;
                            }
                            const auto playerActor = playerHandle.get();
                            auto* player = playerActor && playerActor->IsPlayerRef() ?
                                static_cast<RE::PlayerCharacter*>(playerActor.get()) : nullptr;
                            const auto attackerActor = attackerHandle.get();
                            LogLethalHitTraceState(
                                fmt::format("{}+{}ms", anchor, delayMs),
                                player,
                                attackerActor.get(),
                                0.0F);
                        });
                });
        }
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

    void UpdateTextContext(RE::PlayerCharacter* player, RE::Actor* attacker) {
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
    }

    void ShowDecisionUI(RE::PlayerCharacter* player) {
        if (!player || state.load() != DeathState::Defeated) {
            return;
        }
        if (auto processLists = RE::ProcessLists::GetSingleton()) {
            processLists->StopCombatAndAlarmOnActor(player, true);
            logger::info("Existing combat and alarms against the player were cleared once.");
        } else {
            logger::warn("Could not clear combat against the player: ProcessLists is unavailable.");
        }
        Prisma::ShowDeathMenu(activeRespawnMask.load());
        logger::info("Player decision UI applied.");
    }

    void ApplyDefeatedPoseAndShowMenu(RE::PlayerCharacter* player) {
        if (!player || state.load() != DeathState::Defeated) {
            return;
        }

        const auto selectedPose = static_cast<Settings::DefeatPose>(Settings::Gameplay.defeatPose);
        const bool nativeRagdollPending = adoptPendingNativeRagdoll.exchange(false);
        if (selectedPose == Settings::DefeatPose::kRagdoll ||
            selectedPose == Settings::DefeatPose::kPersistentRagdoll) {
            const bool alreadyRagdolled = IsAlreadyRagdolled(player);
            const bool adoptOnly = nativeRagdollPending || alreadyRagdolled;
            player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
            if (selectedPose == Settings::DefeatPose::kPersistentRagdoll) {
                const auto request = MoreRagdollClient::Enable(player, adoptOnly);
                activeRecoveryMode.store(DefeatRecoveryMode::MoreRagdoll);
                logger::info(
                    "Requested More Ragdoll persistent hold through {} "
                    "(accepted={}, existingRagdoll={}, adoptOnly={}).",
                    MoreRagdollClient::ToString(request.route),
                    request.accepted,
                    alreadyRagdolled,
                    adoptOnly);
            } else if (!adoptOnly) {
                const bool accepted = player->NotifyAnimationGraph("RagdollInstant");
                activeRecoveryMode.store(DefeatRecoveryMode::VanillaRagdoll);
                logger::info("Requested vanilla RagdollInstant (accepted={}).", accepted);
            } else {
                activeRecoveryMode.store(DefeatRecoveryMode::VanillaRagdoll);
                logger::info(
                    "Adopted the existing or pending native player ragdoll; "
                    "RagdollInstant was not sent again.");
            }
            logger::info(
                "Player defeat pose applied as {} ragdoll without unconscious state.",
                selectedPose == Settings::DefeatPose::kPersistentRagdoll ? "persistent" : "vanilla");
        } else {
            player->SetLifeState(RE::ACTOR_LIFE_STATE::kBleedout);
            player->NotifyAnimationGraph("BleedoutStart");
            activeRecoveryMode.store(DefeatRecoveryMode::Bleedout);
            logger::info("Player defeat pose applied as bleedout.");
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
                RestoreControls();
                return;
            }

            const bool nativeRagdollAfterImpact = IsAlreadyRagdolled(player);
            adoptPendingNativeRagdoll.store(nativeRagdollAfterImpact);
            logger::info(
                "Finalizing {} defeat on the next engine task: nativeRagdollAfterImpact={}, "
                "adoptPendingNativeRagdoll={}. A missing native ragdoll will be created by "
                "More Ragdoll through the Numpad 7 knock-explosion route.",
                ToString(cause),
                nativeRagdollAfterImpact,
                adoptPendingNativeRagdoll.load());
            LogLethalHitTraceState(
                cause == DefeatCause::Projectile ?
                    "ProjectileDefeat:after-impact-task" : "LethalFallDefeat:after-impact-task",
                player,
                nullptr,
                0.0F);
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
        logger::info("Pending killmove defeat finalized{}.", forced ? " by fallback" : " from animation event");
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

    void LogRecoveryState(RE::PlayerCharacter* player, std::string_view context) {
        const auto actorState = player ? player->AsActorState() : nullptr;
        const auto controller = player ? player->GetCharController() : nullptr;
        const bool controllerInWorld = controller && controller->GetHavokWorld();
        logger::info(
            "Ragdoll recovery state at '{}': lifeState={}, knockState={}, getUpStarted={}, "
            "controllerEvent={}, controllerPresent={}, controllerInWorld={}, nativeRagdoll={}, "
            "midair={}, controllerFlags=0x{:08X}, controllerState={}, wantedState={}, supportCount={}, "
            "getUpFinished={}.",
            context,
            player ? static_cast<int>(player->GetLifeState()) : -1,
            actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
            recoveryGetUpStarted.load(),
            recoveryControllerAdded.load(),
            controller != nullptr,
            controllerInWorld,
            player && player->IsInRagdollState(),
            player && player->IsInMidair(),
            controller ? controller->flags.underlying() : 0U,
            controller ? static_cast<int>(controller->context.currentState) : -1,
            controller ? static_cast<int>(controller->wantState) : -1,
            controller ? controller->supportCount : -1,
            recoveryGetUpFinished.load());
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
            RestoreControls();
            return;
        }

        LogRecoveryState(player, reason);
        RestoreControls();
        player->NotifyAnimationGraph("TrickDeathRevive");
        state.store(DeathState::Recovering);
        logger::info(
            "Player respawn completed after '{}'; controls restored and TrickDeathRevive sent.",
            reason);
        IntegrationEvents::SendRespawnCompleted(activeRespawnOption.load());
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
                            LogRecoveryState(player, "deferred completion rejected");
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
                LogRecoveryState(player, "first fallback");

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
                        "The vanilla get-up sequence did not emit GetUpStart; resent GetUpBegin once "
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
                LogRecoveryState(player, "direct GetUpStart fallback");
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
                    "The vanilla get-up sequence did not restore the character controller; sent one "
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
                LogRecoveryState(player, "final animation-state fallback");
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
        RestoreTemporaryGhost(player);
        player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
        fallSequenceActive.store(false);
        lastFallEventMilliseconds.store(0);
        lastLandingEventMilliseconds.store(0);
        const auto previousRecoveryMode = activeRecoveryMode.exchange(DefeatRecoveryMode::None);
        const bool reviveFromRagdoll =
            previousRecoveryMode == DefeatRecoveryMode::VanillaRagdoll ||
            previousRecoveryMode == DefeatRecoveryMode::MoreRagdoll;
        if (previousRecoveryMode == DefeatRecoveryMode::Bleedout) {
            player->NotifyAnimationGraph("BleedoutStop");
        }
        auto owner = player->AsActorValueOwner();
        SetActorValuePercent(owner, RE::ActorValue::kHealth, Settings::Gameplay.healthPercent);
        SetActorValuePercent(owner, RE::ActorValue::kMagicka, Settings::Gameplay.magickaPercent);
        SetActorValuePercent(owner, RE::ActorValue::kStamina, Settings::Gameplay.staminaPercent);
        logger::info(
            "Restoring player resources: recoveryMode={}, healthAfter={}",
            static_cast<int>(previousRecoveryMode),
            owner ? owner->GetActorValue(RE::ActorValue::kHealth) : -1.0F);
        if (reviveFromRagdoll) {
            awaitingRagdollRecovery.store(true);
            recoveryGetUpStarted.store(false);
            recoveryControllerAdded.store(false);
            recoveryGetUpFinished.store(false);
            recoveryCompletionScheduled.store(false);
            const auto generation = defeatGeneration.load();
            bool getUpAccepted = false;
            std::string releaseRoute;
            if (previousRecoveryMode == DefeatRecoveryMode::MoreRagdoll) {
                const auto request = MoreRagdollClient::Disable(player);
                getUpAccepted = request.accepted;
                releaseRoute = fmt::format(
                    "MoreRagdollDisable through {}",
                    MoreRagdollClient::ToString(request.route));
            } else {
                getUpAccepted = player->NotifyAnimationGraph("GetUpBegin");
                releaseRoute = "GetUpBegin animation event";
            }
            ScheduleRagdollRecoveryFallback(generation);
            LogRecoveryState(player, releaseRoute);
            logger::info(
                "Player ragdoll release requested through {} (accepted={}); "
                "GetUpStart is now left to the vanilla sequence and controls remain disabled until "
                "recovery confirmation",
                releaseRoute,
                getUpAccepted);
            return true;
        }

        RestoreControls();
        player->NotifyAnimationGraph("TrickDeathRevive");
        logger::info(
            "Player non-ragdoll respawn completed from recovery mode {}; "
            "animation event 'TrickDeathRevive' sent.",
            static_cast<int>(previousRecoveryMode));
        return false;
    }

    void FinishRecoveryLater() {
        const auto seconds = Settings::Gameplay.invulnerabilitySeconds;
        if (seconds <= 0) {
            state.store(DeathState::Alive);
            activeDefeatCause.store(DefeatCause::None);
            return;
        }

        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::seconds(seconds), [] {
            SKSE::GetTaskInterface()->AddTask([] {
                auto expected = DeathState::Recovering;
                if (state.compare_exchange_strong(expected, DeathState::Alive)) {
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
        if (option == Respawn::Option::LastSleep && !CheckpointManager::MovePlayerToLastSleep()) {
            state.store(DeathState::Defeated);
            Prisma::ShowError("No valid sleep checkpoint is available.");
            return;
        }
        if (option == Respawn::Option::LastCheckpoint && !CheckpointManager::MovePlayerToCheckpoint()) {
            state.store(DeathState::Defeated);
            Prisma::ShowError("No valid external checkpoint is available.");
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

    void LoadLastSave() {
        if (!Respawn::Contains(activeRespawnMask.load(), Respawn::Option::LoadLastSave)) {
            logger::warn("Rejected unavailable load-last-save option.");
            Prisma::ShowError("Loading the last save is currently unavailable.");
            return;
        }
        auto expected = DeathState::Defeated;
        if (!state.compare_exchange_strong(expected, DeathState::LoadingSave)) {
            return;
        }

        activeRespawnOption.store(Respawn::Option::LoadLastSave);
        IntegrationEvents::SendRespawnSelected(Respawn::Option::LoadLastSave);
        Prisma::Hide();
        RestoreTemporaryGhost(RE::PlayerCharacter::GetSingleton());
        RestoreControls();
        auto manager = RE::BGSSaveLoadManager::GetSingleton();
        if (!manager || !manager->LoadMostRecentSaveGame()) {
            state.store(DeathState::Defeated);
            ApplyTemporaryGhost(RE::PlayerCharacter::GetSingleton());
            CaptureAndDisableControls();
            Prisma::ShowDeathMenu(activeRespawnMask.load());
            Prisma::ShowError("Could not load the most recent save.");
        }
    }
}

void DeathManager::LogHealthDamageHookSnapshot(
    std::string_view phase,
    std::uint64_t damageSequence,
    std::uintptr_t callerOffset,
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    float rawDamage,
    float forwardedDamage) {
    if (phase == "entry" && player && attacker) {
        const auto traceId = BeginOrExtendLethalHitTrace(player, attacker, "HealthDamageHook");
        ScheduleLethalHitTraceSnapshots(
            traceId,
            player->GetHandle(),
            attacker->GetHandle(),
            fmt::format("damageSeq={}", damageSequence));
    }
    LogLethalHitTraceState(
        fmt::format("DamageHook:{}:damageSeq={}", phase, damageSequence),
        player,
        attacker,
        rawDamage);

    const auto owner = player ? player->AsActorValueOwner() : nullptr;
    const float healthNow = owner ? owner->GetActorValue(RE::ActorValue::kHealth) : -1.0F;
    const float estimatedBeforeSignedDamage = healthNow - rawDamage;
    const float estimatedBeforeMagnitude = healthNow + std::abs(rawDamage);
    const auto actorState = player ? player->AsActorState() : nullptr;
    const auto controller = player ? player->GetCharController() : nullptr;
    const auto now = GetSteadyMilliseconds();
    const auto lastFall = lastFallEventMilliseconds.load();
    const auto lastLanding = lastLandingEventMilliseconds.load();
    logger::info(
        "[DamageHook] phase={} damageSeq={} caller=SkyrimSE+0x{:X} state={} cause={} "
        "recovery={} attacker={} rawDamage={} forwardedDamage={} healthNow={} "
        "healthMinusRaw={} healthPlusMagnitude={} fallActive={} diagnosticMsSinceJumpFall={} "
        "diagnosticMsSinceLanding={} lifeState={} knockState={} nativeRagdoll={} midair={} "
        "controllerPresent={} controllerInWorld={} controllerFlags=0x{:08X} "
        "controllerState={} wantedState={} supportCount={}.",
        phase,
        damageSequence,
        callerOffset,
        ToString(state.load()),
        ToString(activeDefeatCause.load()),
        ToString(activeRecoveryMode.load()),
        attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
        rawDamage,
        forwardedDamage,
        healthNow,
        estimatedBeforeSignedDamage,
        estimatedBeforeMagnitude,
        fallSequenceActive.load(),
        lastFall > 0 ? now - lastFall : -1,
        lastLanding > 0 ? now - lastLanding : -1,
        player ? static_cast<int>(player->GetLifeState()) : -1,
        actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
        IsAlreadyRagdolled(player),
        player && player->IsInMidair(),
        controller != nullptr,
        controller && controller->GetHavokWorld(),
        controller ? controller->flags.underlying() : 0U,
        controller ? static_cast<int>(controller->context.currentState) : -1,
        controller ? static_cast<int>(controller->wantState) : -1,
        controller ? controller->supportCount : -1);
}

void DeathManager::LogKillHookSnapshot(
    std::uintptr_t callerOffset,
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    float damage,
    bool sendEvent,
    bool ragdollInstantRequested) {
    if (player) {
        const auto traceId = BeginOrExtendLethalHitTrace(player, attacker, "KillHook");
        ScheduleLethalHitTraceSnapshots(
            traceId,
            player->GetHandle(),
            attacker ? attacker->GetHandle() : RE::ActorHandle{},
            "KillHook");
        LogLethalHitTraceState("KillHook:entry", player, attacker, damage);
    }

    const auto owner = player ? player->AsActorValueOwner() : nullptr;
    const auto actorState = player ? player->AsActorState() : nullptr;
    const auto controller = player ? player->GetCharController() : nullptr;
    const auto sequence = diagnosticEventSequence.fetch_add(1) + 1;
    logger::info(
        "[KillHook] diagnosticSeq={} caller=SkyrimSE+0x{:X} state={} cause={} recovery={} "
        "attacker={} damage={} sendEvent={} ragdollInstantRequested={} health={} lifeState={} "
        "knockState={} nativeRagdoll={} midair={} controllerPresent={} controllerInWorld={} "
        "controllerFlags=0x{:08X} controllerState={} wantedState={} supportCount={}.",
        sequence,
        callerOffset,
        ToString(state.load()),
        ToString(activeDefeatCause.load()),
        ToString(activeRecoveryMode.load()),
        attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
        damage,
        sendEvent,
        ragdollInstantRequested,
        owner ? owner->GetActorValue(RE::ActorValue::kHealth) : -1.0F,
        player ? static_cast<int>(player->GetLifeState()) : -1,
        actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
        IsAlreadyRagdolled(player),
        player && player->IsInMidair(),
        controller != nullptr,
        controller && controller->GetHavokWorld(),
        controller ? controller->flags.underlying() : 0U,
        controller ? static_cast<int>(controller->context.currentState) : -1,
        controller ? static_cast<int>(controller->wantState) : -1,
        controller ? controller->supportCount : -1);
}

void DeathManager::LogLethalHitTraceSnapshot(
    std::string_view phase,
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    float damage) {
    LogLethalHitTraceState(phase, player, attacker, damage);
}

void DeathManager::CaptureAppliedPlayerDamage(
    std::uint64_t damageSequence,
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    float rawDamage) {
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
        captured.damageSequence = damageSequence;
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
            captured.projectile = pendingPlayerHit.projectile;
            captured.sourceType = pendingPlayerHit.sourceType;
            captured.hitFlags = pendingPlayerHit.flags;
        } else if (pendingFallDamage.valid && !attacker) {
            captured.origin = AppliedDamageOrigin::FallPhysics;
        } else {
            captured.origin = AppliedDamageOrigin::StandardHit;
        }

        pendingLethalDamage = captured;
        pendingPlayerHit = {};
        pendingFallDamage = {};
    }

    logger::info(
        "Captured lethal damage context without a time window: damageSeq={} origin={} "
        "attacker={:08X} source={:08X} sourceType={} projectile={:08X} "
        "hitFlags=0x{:02X} rawDamage={} health={}.",
        captured.damageSequence,
        ToString(captured.origin),
        captured.attacker,
        captured.source,
        captured.sourceType,
        captured.projectile,
        captured.hitFlags,
        rawDamage,
        health);
}

void DeathManager::MarkPlayerFallDamage(
    float fallDistance,
    float calculatedDamage,
    bool moveFinishSource) {
    FallDamageContext context;
    {
        std::scoped_lock lock(damageContextLock);
        context.valid = true;
        context.serial = ++nextDamageContextSerial;
        context.fallDistance = fallDistance;
        context.calculatedDamage = calculatedDamage;
        context.moveFinishSource = moveFinishSource;
        pendingFallDamage = context;
    }
    logger::info(
        "Marked causal fall-damage context: serial={} source={} fallDistance={} "
        "calculatedDamage={}.",
        context.serial,
        context.moveFinishSource ? "move-finish" : "fall-physics",
        context.fallDistance,
        context.calculatedDamage);
}

void DeathManager::HandlePlayerHitEvent(const RE::TESHitEvent& event) {
    const auto targetReference = event.target.get();
    auto* target = targetReference ? targetReference->As<RE::Actor>() : nullptr;
    if (!target || !target->IsPlayerRef()) {
        return;
    }

    auto* player = static_cast<RE::PlayerCharacter*>(target);
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
        hitContext.projectile = event.projectile;
        hitContext.sourceType = sourceType;
        hitContext.flags = event.flags.underlying();
        hitContext.projectileImpact = projectileHit;
        pendingPlayerHit = hitContext;
    }
    const auto traceId = BeginOrExtendLethalHitTrace(player, attacker, "TESHitEvent");
    const auto traceSequence = lethalHitTraceLogSequence.fetch_add(1) + 1;
    logger::info(
        "[LethalTrace][HitEvent] traceId={} traceSeq={} t={}ms target={:08X} "
        "attacker={} source={:08X} sourceType={} projectile={:08X} flags=0x{:02X} "
        "power={} sneak={} bash={} blocked={} contextSerial={} projectileContext={}.",
        traceId,
        traceSequence,
        GetLethalHitTraceElapsed(),
        player->GetFormID(),
        attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
        event.source,
        sourceType,
        event.projectile,
        event.flags.underlying(),
        event.flags.any(RE::TESHitEvent::Flag::kPowerAttack),
        event.flags.any(RE::TESHitEvent::Flag::kSneakAttack),
        event.flags.any(RE::TESHitEvent::Flag::kBashAttack),
        event.flags.any(RE::TESHitEvent::Flag::kHitBlocked),
        hitContext.serial,
        projectileHit);
    LogLethalHitTraceState("TESHitEvent", player, attacker, 0.0F);
}

bool DeathManager::TryInterceptDeath(
    RE::PlayerCharacter* player,
    RE::Actor* attacker,
    bool ragdollInstantRequested) {
    LogLethalHitTraceState(
        "TryInterceptDeath:entry",
        player,
        attacker,
        0.0F);
    if (!player || !Settings::Gameplay.enabled) {
        LogLethalHitTraceState(
            "TryInterceptDeath:not-intercepted-disabled-or-no-player",
            player,
            attacker,
            0.0F);
        return false;
    }

    if (player->IsGodMode()) {
        if (auto owner = player->AsActorValueOwner(); owner &&
            owner->GetActorValue(RE::ActorValue::kHealth) < 1.0F) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        logger::info("Ignored player KillImpl while God Mode is active.");
        LogLethalHitTraceState("TryInterceptDeath:god-mode", player, attacker, 0.0F);
        return true;
    }

    const auto current = state.load();
    if (current != DeathState::Alive) {
        if (auto owner = player->AsActorValueOwner()) {
            SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
        }
        LogLethalHitTraceState(
            "TryInterceptDeath:already-defeated",
            player,
            attacker,
            0.0F);
        return true;
    }

    const auto respawnEvaluation = RespawnPolicyManager::Evaluate();
    if (respawnEvaluation.trickDeathDisabled || respawnEvaluation.availableMask == 0) {
        logger::info(
            "Death was not intercepted by respawn policy: disabled={}, available=0x{:X}, blocked=0x{:X}.",
            respawnEvaluation.trickDeathDisabled,
            respawnEvaluation.availableMask,
            respawnEvaluation.blockedMask);
        LogLethalHitTraceState(
            "TryInterceptDeath:not-intercepted-respawn-policy",
            player,
            attacker,
            0.0F);
        return false;
    }

    if (!Prisma::CanShow()) {
        logger::error("Death was not intercepted because PrismaUI is unavailable or not ready.");
        LogLethalHitTraceState(
            "TryInterceptDeath:not-intercepted-prisma-unavailable",
            player,
            attacker,
            0.0F);
        return false;
    }

    const bool inKillMove = IsPlayerKillMoveActive(player);
    const bool alreadyRagdolled = IsAlreadyRagdolled(player);
    const bool inMidair = player->IsInMidair();
    const auto lethalDamage = ConsumeLethalDamageContext(attacker);
    const bool projectileImpact =
        !inKillMove && lethalDamage.origin == AppliedDamageOrigin::ProjectileImpact;
    const bool lethalFall =
        !inKillMove && lethalDamage.origin == AppliedDamageOrigin::FallPhysics;

    // Only adopt physical state that actually exists. A fall/projectile context
    // is a damage cause, not proof that the engine created a ragdoll.
    adoptPendingNativeRagdoll.store(alreadyRagdolled || inKillMove);
    const auto actorState = player->AsActorState();
    logger::info(
        "Intercepting player death: attacker={}, ragdollInstantRequested={}, inKillMove={}, "
        "nativeRagdoll={}, midair={}, knockState={}, fallAnimationActive={}, "
        "causalDamageSeq={}, causalOrigin={}, projectile={:08X}, hitSource={:08X}, "
        "hitSourceType={}, hitFlags=0x{:02X}, classifiedProjectile={}, "
        "classifiedLethalFall={}, adoptPendingNativeRagdoll={}.",
        attacker ? fmt::format("{:08X}", attacker->GetFormID()) : std::string("none"),
        ragdollInstantRequested,
        inKillMove,
        alreadyRagdolled,
        inMidair,
        actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
        fallSequenceActive.load(),
        lethalDamage.damageSequence,
        ToString(lethalDamage.origin),
        lethalDamage.projectile,
        lethalDamage.source,
        lethalDamage.sourceType,
        lethalDamage.hitFlags,
        projectileImpact,
        lethalFall,
        adoptPendingNativeRagdoll.load());
    auto expected = DeathState::Alive;
    const auto nextState = inKillMove ? DeathState::PendingKillMove : DeathState::Defeated;
    if (!state.compare_exchange_strong(expected, nextState)) {
        LogLethalHitTraceState(
            "TryInterceptDeath:state-race-consumed",
            player,
            attacker,
            0.0F);
        return true;
    }

    activeRespawnMask.store(respawnEvaluation.availableMask);
    activeRespawnOption.store(Respawn::Option::None);
    UpdateTextContext(player, attacker);

    activeDefeatCause.store(
        inKillMove ? DefeatCause::KillMove :
        projectileImpact ? DefeatCause::Projectile :
        lethalFall ? DefeatCause::LethalFall : DefeatCause::Standard);
    activeRecoveryMode.store(DefeatRecoveryMode::None);

    const auto generation = defeatGeneration.fetch_add(1) + 1;
    if (inKillMove) {
        ProtectPlayerForDecision(player, false);
        killMoveAttacker = attacker ? attacker->GetHandle() : RE::ActorHandle{};
        logger::info("Player defeat intercepted during killcam; waiting for the player animation event.");
        ScheduleKillMoveFallback(generation);
    } else if (projectileImpact || lethalFall) {
        ProtectPlayerForDecision(player, true);
        const auto impactCause = projectileImpact ? DefeatCause::Projectile : DefeatCause::LethalFall;
        ScheduleImpactDefeatPose(generation, player->GetHandle(), impactCause);
        if (lethalFall) {
            fallSequenceActive.store(false);
        }
        logger::info(
            "Player {} defeat intercepted from causal damage context; pose application "
            "was queued after the current engine damage stack.",
            ToString(impactCause));
    } else {
        ProtectPlayerForDecision(player, true);
        ApplyDefeatedPoseAndShowMenu(player);
        logger::info("Player defeat intercepted outside a killcam.");
    }
    LogLethalHitTraceState(
        inKillMove ? "TryInterceptDeath:intercepted-killmove" :
        projectileImpact ? "TryInterceptDeath:intercepted-projectile-causal" :
        lethalFall ? "TryInterceptDeath:intercepted-lethal-fall-causal" :
                     "TryInterceptDeath:intercepted-standard-after-pose",
        player,
        attacker,
        0.0F);
    return true;
}

void DeathManager::HandlePlayerAnimationEvent(
    std::string_view eventName,
    std::string_view payload,
    std::uintptr_t graphSource) {
    const auto now = GetSteadyMilliseconds();
    const auto normalizedEvent = NormalizeAnimationEvent(eventName);
    BufferAnimationEvent(now, graphSource, eventName, payload);

    if (IsLethalHitTraceActive(now)) {
        const auto traceId = activeLethalHitTrace.load();
        const auto traceSequence = lethalHitTraceLogSequence.fetch_add(1) + 1;
        const auto previousAnimationEvent = lastLethalHitTraceAnimationMilliseconds.exchange(now);
        logger::info(
            "[LethalTrace][AnimEvent] traceId={} traceSeq={} t={}ms graphSource=0x{:X} "
            "event='{}' payload='{}' deltaFromPreviousAnimationMs={}.",
            traceId,
            traceSequence,
            GetLethalHitTraceElapsed(now),
            graphSource,
            eventName,
            payload,
            previousAnimationEvent > 0 ? now - previousAnimationEvent : -1);
        LogLethalHitTraceState(
            fmt::format("AnimEvent:{}", eventName),
            RE::PlayerCharacter::GetSingleton(),
            nullptr,
            0.0F);
    } else if (IsDiagnosticAnimationEvent(normalizedEvent)) {
        const auto sequence = diagnosticEventSequence.fetch_add(1) + 1;
        const auto previousAnimationEvent = lastDiagnosticAnimationMilliseconds.exchange(now);
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto owner = player ? player->AsActorValueOwner() : nullptr;
        const auto actorState = player ? player->AsActorState() : nullptr;
        const auto controller = player ? player->GetCharController() : nullptr;
        logger::info(
            "[AnimEvent] diagnosticSeq={} graphSource=0x{:X} event='{}' payload='{}' "
            "deltaFromPreviousDiagnosticMs={} state={} cause={} recovery={} health={} "
            "lifeState={} knockState={} nativeRagdoll={} midair={} controllerPresent={} "
            "controllerInWorld={} controllerFlags=0x{:08X} controllerState={} wantedState={} "
            "supportCount={}.",
            sequence,
            graphSource,
            eventName,
            payload,
            previousAnimationEvent > 0 ? now - previousAnimationEvent : -1,
            ToString(state.load()),
            ToString(activeDefeatCause.load()),
            ToString(activeRecoveryMode.load()),
            owner ? owner->GetActorValue(RE::ActorValue::kHealth) : -1.0F,
            player ? static_cast<int>(player->GetLifeState()) : -1,
            actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
            IsAlreadyRagdolled(player),
            player && player->IsInMidair(),
            controller != nullptr,
            controller && controller->GetHavokWorld(),
            controller ? controller->flags.underlying() : 0U,
            controller ? static_cast<int>(controller->context.currentState) : -1,
            controller ? static_cast<int>(controller->wantState) : -1,
            controller ? controller->supportCount : -1);
    }

    if (normalizedEvent.starts_with("jumpfall")) {
        lastFallEventMilliseconds.store(now);
        lastLandingEventMilliseconds.store(0);
        if (!fallSequenceActive.exchange(true)) {
            const auto generation = fallGeneration.fetch_add(1) + 1;
            logger::debug(
                "Player fall sequence {} started from {}.",
                generation,
                eventName);
        }
    } else if (normalizedEvent.starts_with("jumpland") || normalizedEvent == "jumpdown") {
        const bool wasFalling = fallSequenceActive.exchange(false);
        lastLandingEventMilliseconds.store(now);
        if (wasFalling) {
            logger::debug(
                "Player fall sequence {} landed through {}.",
                fallGeneration.load(),
                eventName);
        }
    }

    const auto currentState = state.load();

    if (currentState == DeathState::Resolving && awaitingRagdollRecovery.load()) {
        const bool getUpStarted = eventName == "GetUpStart";
        const bool controllerAdded = eventName == "AddCharacterControllerToWorld";
        const bool getUpFinished = eventName == "GetUpEnd" || eventName == "GetUpExit";
        if (getUpStarted || controllerAdded || getUpFinished) {
            const auto generation = defeatGeneration.load();
            const std::string eventCopy(eventName);
            SKSE::GetTaskInterface()->AddTask(
                [generation, eventCopy, getUpStarted, controllerAdded, getUpFinished] {
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
                logger::info("Observed player ragdoll recovery event '{}'.", eventCopy);

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
    const std::string eventCopy(eventName);
    SKSE::GetTaskInterface()->AddTask([generation, eventCopy] {
        logger::info("Player animation event '{}' completed the pending killmove.", eventCopy);
        FinalizePendingKillMove(generation, false);
    });
}

bool DeathManager::IsDamageBlocked() {
    return state.load() != DeathState::Alive;
}

void DeathManager::RepairBlockedPlayerHealth(RE::PlayerCharacter* player) {
    if (!player || state.load() == DeathState::Alive) {
        return;
    }
    auto owner = player->AsActorValueOwner();
    if (!owner) {
        return;
    }

    const float before = owner->GetActorValue(RE::ActorValue::kHealth);
    if (std::isfinite(before) && before >= 1.0F) {
        return;
    }
    SetCurrentActorValue(owner, RE::ActorValue::kHealth, 1.0F);
    logger::warn(
        "Repaired player health after blocked post-applied damage: before={}, after={}; "
        "state={}, cause={}.",
        before,
        owner->GetActorValue(RE::ActorValue::kHealth),
        ToString(state.load()),
        ToString(activeDefeatCause.load()));
}

bool DeathManager::IsMenuOpen() {
    return state.load() == DeathState::Defeated;
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
        } else if (actionCopy == "load_last_save") {
            LoadLastSave();
        } else {
            logger::warn("Rejected unknown death menu action: {}", actionCopy);
        }
    });
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
    TextManager::ClearRuntimeVariables();
    fallSequenceActive.store(false);
    lastFallEventMilliseconds.store(0);
    lastLandingEventMilliseconds.store(0);
    ClearAppliedDamageContexts();
    lastDiagnosticAnimationMilliseconds.store(0);
    activeLethalHitTrace.store(0);
    lethalHitTraceStartMilliseconds.store(0);
    lethalHitTraceDeadlineMilliseconds.store(0);
    lethalHitTraceLogSequence.store(0);
    lastLethalHitTraceAnimationMilliseconds.store(0);
    {
        std::scoped_lock lock(animationPreRollLock);
        animationPreRoll.clear();
    }
    killMoveAttacker.reset();
    activeDefeatCause.store(DefeatCause::None);
    const auto previousRecoveryMode = activeRecoveryMode.exchange(DefeatRecoveryMode::None);
    Prisma::Hide();
    const auto previousState = state.exchange(DeathState::Alive);
    auto player = RE::PlayerCharacter::GetSingleton();
    RestoreTemporaryGhost(player);
    if (player && previousRecoveryMode == DefeatRecoveryMode::MoreRagdoll) {
        MoreRagdollClient::Disable(player);
    }
    if (player && previousState != DeathState::Alive) {
        player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
    }
    RestoreControls();
}

#include "Hooks.h"
#include "DelayedDispatcher.h"
#include "DeathManager.h"
#include "InputEventHandler.h"
#include "Prisma.h"
#include "RespawnTypes.h"

#include "RE/A/AIProcess.h"

#include <MinHook.h>
#include <intrin.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {
    std::uintptr_t ToModuleOffset(void* address) {
        const auto absolute = reinterpret_cast<std::uintptr_t>(address);
        const auto base = REL::Module::get().base();
        return absolute >= base ? absolute - base : absolute;
    }

    struct PlayerKillHook {
        static void Thunk(RE::PlayerCharacter* player, RE::Actor* attacker, float damage, bool sendEvent, bool ragdollInstant) {
            const auto callerOffset = ToModuleOffset(_ReturnAddress());
            DeathManager::LogKillHookSnapshot(
                callerOffset,
                player,
                attacker,
                damage,
                sendEvent,
                ragdollInstant);
            if (DeathManager::TryInterceptDeath(player, attacker, ragdollInstant)) {
                DeathManager::LogLethalHitTraceSnapshot(
                    "KillHook:intercepted-before-original",
                    player,
                    attacker,
                    damage);
                return;
            }
            DeathManager::LogLethalHitTraceSnapshot(
                "KillHook:forwarding-to-original",
                player,
                attacker,
                damage);
            Function(player, attacker, damage, sendEvent, ragdollInstant);
            DeathManager::LogLethalHitTraceSnapshot(
                "KillHook:after-original",
                player,
                attacker,
                damage);
        }

        static inline REL::Relocation<decltype(Thunk)> Function;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtable{ RE::PlayerCharacter::VTABLE[0] };
            Function = vtable.write_vfunc(REL::Relocate(0x10E, 0x10E, 0x110), Thunk);
        }
    };

    struct PlayerHealthDamageHook {
        static void Thunk(RE::PlayerCharacter* player, RE::Actor* attacker, float damage) {
            const auto sequence = damageSequence.fetch_add(1) + 1;
            const auto callerOffset = ToModuleOffset(_ReturnAddress());
            if (DeathManager::IsDamageBlocked()) {
                DeathManager::RepairBlockedPlayerHealth(player);
                DeathManager::LogHealthDamageHookSnapshot(
                    "blocked-by-death-state",
                    sequence,
                    callerOffset,
                    player,
                    attacker,
                    damage,
                    0.0F);
                return;
            }

            DeathManager::CaptureAppliedPlayerDamage(
                sequence,
                player,
                attacker,
                damage);

            if (handlingDamage) {
                DeathManager::LogHealthDamageHookSnapshot(
                    "reentrant-forward",
                    sequence,
                    callerOffset,
                    player,
                    attacker,
                    damage,
                    damage);
                Function(player, attacker, damage);
                return;
            }

            handlingDamage = true;
            float forwardedDamage = damage;
            DeathManager::LogHealthDamageHookSnapshot(
                "entry",
                sequence,
                callerOffset,
                player,
                attacker,
                damage,
                forwardedDamage);
            Function(player, attacker, forwardedDamage);
            DeathManager::LogHealthDamageHookSnapshot(
                "after-original-pass-through",
                sequence,
                callerOffset,
                player,
                attacker,
                damage,
                forwardedDamage);
            handlingDamage = false;
        }

        static inline REL::Relocation<decltype(Thunk)> Function;
        static inline thread_local bool handlingDamage = false;
        static inline std::atomic_uint64_t damageSequence{ 0 };

        static void Install() {
            REL::Relocation<std::uintptr_t> vtable{ RE::PlayerCharacter::VTABLE[0] };
            Function = vtable.write_vfunc(REL::Relocate(0x104, 0x104, 0x106), Thunk);
        }
    };

    struct FallPhysicsDamageHook {
        using Function_t = float (*)(RE::Actor*, float, float);

        template <bool MoveFinish>
        static float Thunk(RE::Actor* actor, float fallDistance, float defaultMultiplier) {
            auto& original = MoveFinish ? MoveFinishFunction : FallFunction;
            const float calculatedDamage = original(actor, fallDistance, defaultMultiplier);
            if (!actor || !actor->IsPlayerRef() || calculatedDamage <= 0.0F) {
                return calculatedDamage;
            }

            auto* player = static_cast<RE::PlayerCharacter*>(actor);
            if (DeathManager::IsDamageBlocked()) {
                DeathManager::RepairBlockedPlayerHealth(player);
                logger::info(
                    "[FallDamageHook] source={} suppressed while defeat is active: "
                    "fallDistance={}, defaultMultiplier={}, calculatedDamage={}.",
                    MoveFinish ? "move-finish" : "fall-physics",
                    fallDistance,
                    defaultMultiplier,
                    calculatedDamage);
                return 0.0F;
            }
            DeathManager::MarkPlayerFallDamage(
                fallDistance,
                calculatedDamage,
                MoveFinish);
            logger::info(
                "[FallDamageHook] source={} fallDistance={}, defaultMultiplier={}, "
                "calculatedDamage={}, forwardedDamage={}; diagnostic-only pass-through so "
                "the native lethal pipeline can reach KillImpl.",
                MoveFinish ? "move-finish" : "fall-physics",
                fallDistance,
                defaultMultiplier,
                calculatedDamage,
                calculatedDamage);
            return calculatedDamage;
        }

        static void Install() {
            auto& trampoline = SKSE::GetTrampoline();

            REL::Relocation<std::uintptr_t> fallPhysicsDamage{
                RELOCATION_ID(36346, 37336)
            };
            FallFunction = trampoline.write_call<5>(
                fallPhysicsDamage.address() + 0x35,
                Thunk<false>);

            REL::Relocation<std::uintptr_t> moveFinish{
                RELOCATION_ID(36973, 37998)
            };
            MoveFinishFunction = trampoline.write_call<5>(
                moveFinish.address() + REL::Relocate(0xAE, 0xAB),
                Thunk<true>);

            logger::info("Dedicated fall/physics damage hooks installed.");
        }

        static inline REL::Relocation<Function_t> FallFunction;
        static inline REL::Relocation<Function_t> MoveFinishFunction;
    };

    struct StartCombatHook {
        using Function_t = bool (*)(RE::Actor*, RE::Actor*, RE::CombatGroup*);

        static bool Thunk(RE::Actor* actor, RE::Actor* target, RE::CombatGroup* combatGroup) {
            if (target && target->IsPlayerRef() && DeathManager::IsMenuOpen()) {
                logger::debug("Blocked a new combat acquisition against the defeated player.");
                return false;
            }
            return Function(actor, target, combatGroup);
        }

        static inline Function_t Function = nullptr;

        static bool Install() {
            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error("Could not initialize the combat-target hook: MinHook status {}.", static_cast<int>(initStatus));
                return false;
            }

            REL::Relocation<std::uintptr_t> startCombat{ RELOCATION_ID(37608, 38561) };
            const auto createStatus = MH_CreateHook(
                reinterpret_cast<void*>(startCombat.address()),
                reinterpret_cast<void*>(&Thunk),
                reinterpret_cast<void**>(&Function));
            if (createStatus != MH_OK) {
                logger::error("Could not create the combat-target hook: MinHook status {}.", static_cast<int>(createStatus));
                return false;
            }

            const auto enableStatus = MH_EnableHook(reinterpret_cast<void*>(startCombat.address()));
            if (enableStatus != MH_OK) {
                logger::error("Could not enable the combat-target hook: MinHook status {}.", static_cast<int>(enableStatus));
                return false;
            }

            logger::info("Combat acquisition hook installed for the defeated player test.");
            return true;
        }
    };
}

struct ProcessInputQueueHook {
    static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_event) {
        a_event = InputEventHandler::Process(const_cast<RE::InputEvent**>(a_event));
        originalFunction(a_dispatcher, a_event);
    }
    static inline REL::Relocation<decltype(thunk)> originalFunction;
    static void install() {
        auto& trampoline = SKSE::GetTrampoline();
        originalFunction = trampoline.write_call<5>(REL::RelocationID(67315, 68617, 67315).address() + REL::Relocate(0x7B, 0x7B, 0x81), thunk);
    }
};

namespace {
    enum class ManualRagdollTest {
        AddRagdollToWorld,
        RagdollInstant,
        NativeKnockExplosion,
        ReconcileState
    };

    const char* ToString(ManualRagdollTest test) {
        switch (test) {
        case ManualRagdollTest::AddRagdollToWorld:
            return "AddRagdollToWorld";
        case ManualRagdollTest::RagdollInstant:
            return "RagdollInstant";
        case ManualRagdollTest::NativeKnockExplosion:
            return "AIProcessKnockExplosion";
        case ManualRagdollTest::ReconcileState:
            return "PotentiallyFixRagdollState";
        }
        return "unknown";
    }

    void LogManualRagdollAudit(std::string_view test, std::string_view phase, RE::Actor* actor) {
        if (!actor) {
            logger::error("[ManualRagdollTest] test={} phase={}: player handle expired.", test, phase);
            return;
        }

        const auto actorState = actor->AsActorState();
        const auto controller = actor->GetCharController();
        RE::hkVector4 velocity{};
        if (controller) {
            controller->GetLinearVelocityImpl(velocity);
        }
        logger::info(
            "[ManualRagdollTest] test={} phase={} position=({:.3f},{:.3f},{:.3f}) "
            "velocity=({:.3f},{:.3f},{:.3f}) lifeState={} knockState={} nativeRagdoll={} "
            "controllerPresent={} controllerInWorld={}.",
            test,
            phase,
            actor->GetPositionX(),
            actor->GetPositionY(),
            actor->GetPositionZ(),
            velocity.quad.m128_f32[0],
            velocity.quad.m128_f32[1],
            velocity.quad.m128_f32[2],
            static_cast<int>(actor->GetLifeState()),
            actorState ? static_cast<int>(actorState->GetKnockState()) : -1,
            actor->IsInRagdollState(),
            controller != nullptr,
            controller && controller->GetHavokWorld());
    }

    bool AddPlayerRagdollsToWorld(RE::PlayerCharacter* player, std::string_view test) {
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!player->GetAnimationGraphManager(manager) || !manager) {
            logger::error("[ManualRagdollTest] test={}: no animation graph manager.", test);
            return false;
        }

        bool calledAnyGraph = false;
        for (std::uint32_t index = 0; index < manager->graphs.size(); ++index) {
            const auto& graph = manager->graphs[index];
            if (!graph) {
                logger::info("[ManualRagdollTest] test={} graph={} is unavailable.", test, index);
                continue;
            }

            const bool hadRagdoll = graph->HasRagdoll();
            const bool added = graph->AddRagdollToWorld();
            calledAnyGraph = true;
            logger::info(
                "[ManualRagdollTest] test={} graph={} HasRagdollBefore={} AddRagdollToWorldReturned={}.",
                test,
                index,
                hadRagdoll,
                added);
        }
        return calledAnyGraph;
    }

    void ScheduleManualRagdollAudits(RE::ActorHandle handle, std::string test) {
        for (const auto [delay, phase] : {
                 std::pair{ std::chrono::milliseconds(16), std::string("16ms") },
                 std::pair{ std::chrono::milliseconds(50), std::string("50ms") },
                 std::pair{ std::chrono::milliseconds(100), std::string("100ms") },
                 std::pair{ std::chrono::milliseconds(250), std::string("250ms") },
                 std::pair{ std::chrono::milliseconds(500), std::string("500ms") },
                 std::pair{ std::chrono::milliseconds(1000), std::string("1000ms") } }) {
            Utils::DelayedDispatcher::Get().PostDelayed(delay, [handle, test, phase] {
                SKSE::GetTaskInterface()->AddTask([handle, test, phase] {
                    LogManualRagdollAudit(test, phase, handle.get().get());
                    });
                });
        }
    }

    void RunManualRagdollTest(ManualRagdollTest test) {
        const std::string testName(ToString(test));
        SKSE::GetTaskInterface()->AddTask([test, testName] {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) {
                logger::error("[ManualRagdollTest] test={}: player or 3D is unavailable.", testName);
                return;
            }
            if (DeathManager::IsMenuOpen()) {
                logger::warn("[ManualRagdollTest] test={} rejected while the defeat UI is open.", testName);
                return;
            }
            if (player->IsInRagdollState()) {
                logger::warn("[ManualRagdollTest] test={} rejected: player is already in ragdoll.", testName);
                return;
            }

            const auto controller = player->GetCharController();
            if (!controller || !controller->GetHavokWorld()) {
                logger::warn(
                    "[ManualRagdollTest] test={} rejected: character controller is not in the Havok world.",
                    testName);
                return;
            }

            LogManualRagdollAudit(testName, "before", player);
            switch (test) {
            case ManualRagdollTest::AddRagdollToWorld:
                // This is the native graph operation ultimately requested by Papyrus'
                // ForceAddRagdollToWorld. It can only insert a ragdoll that already exists.
                AddPlayerRagdollsToWorld(player, testName);
                break;
            case ManualRagdollTest::RagdollInstant:
                logger::info(
                    "[ManualRagdollTest] test={}: NotifyAnimationGraph(RagdollInstant) returned {}.",
                    testName,
                    player->NotifyAnimationGraph("RagdollInstant"));
                break;
            case ManualRagdollTest::NativeKnockExplosion: {
                auto* process = player->GetActorRuntimeData().currentProcess;
                if (!process) {
                    logger::error(
                        "[ManualRagdollTest] test={}: currentProcess is unavailable.",
                        testName);
                    break;
                }
                if (!process->InHighProcess()) {
                    logger::warn(
                        "[ManualRagdollTest] test={}: currentProcess is not high process.",
                        testName);
                    break;
                }

                const auto location = player->GetPosition();
                constexpr float magnitude = std::numeric_limits<float>::min();
                logger::info(
                    "[ManualRagdollTest] test={}: calling AIProcess::KnockExplosion "
                    "at ({:.3f},{:.3f},{:.3f}) magnitude={:.9e}.",
                    testName,
                    location.x,
                    location.y,
                    location.z,
                    magnitude);
                process->KnockExplosion(player, location, magnitude);
                break;
            }
            case ManualRagdollTest::ReconcileState:
                player->PotentiallyFixRagdollState();
                logger::info("[ManualRagdollTest] test={}: called PotentiallyFixRagdollState.", testName);
                break;
            }

            LogManualRagdollAudit(testName, "immediate", player);
            ScheduleManualRagdollAudits(player->GetHandle(), testName);
            });
    }

    std::optional<ManualRagdollTest> GetManualRagdollTest(std::uint32_t keyCode) {
        using Keys = RE::BSWin32KeyboardDevice::Keys;
        if (keyCode == Keys::kNum5 || keyCode == Keys::kKP_5) return ManualRagdollTest::AddRagdollToWorld;
        if (keyCode == Keys::kNum6 || keyCode == Keys::kKP_6) return ManualRagdollTest::RagdollInstant;
        if (keyCode == Keys::kNum7 || keyCode == Keys::kKP_7) return ManualRagdollTest::NativeKnockExplosion;
        if (keyCode == Keys::kNum8 || keyCode == Keys::kKP_8) return ManualRagdollTest::ReconcileState;
        return std::nullopt;
    }
}

bool OnInput(RE::InputEvent* event) {
    if (!event) return false;
    if (event->device != RE::INPUT_DEVICE::kKeyboard) return false;
    auto button = event->AsButtonEvent();
    if (!button) return false;
    if (!button->IsDown()) return false;
    const auto keyCode = button->GetIDCode();
    if (const auto test = GetManualRagdollTest(keyCode)) {
        logger::info("[ManualRagdollTest] key={} requested test={}", keyCode, ToString(*test));
        RunManualRagdollTest(*test);
        return true;
    }
    if (keyCode == RE::BSWin32KeyboardDevice::Keys::kF2) {
#ifdef DEV_SERVER
        if (Prisma::IsHidden()) {
            Prisma::ShowDeathMenu(Respawn::ACTION_MASK);
        }
        else {
            Prisma::Hide();
        }
        return true;
#endif
    }
    return false;
}



void Hooks::Install() {
    SKSE::AllocTrampoline(64);
    PlayerKillHook::Install();
    PlayerHealthDamageHook::Install();
    FallPhysicsDamageHook::Install();
    StartCombatHook::Install();
    ProcessInputQueueHook::install();
    InputEventHandler::Register(OnInput);
    logger::info("Death and input hooks installed. Combat groups are not accessed or modified.");
}

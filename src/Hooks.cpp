#include "Hooks.h"
#include "DeathManager.h"
#include "InputEventHandler.h"
#include "Prisma.h"
#include "RespawnTypes.h"

#include "RE/A/AIProcess.h"

#include <MinHook.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace {
    struct PlayerKillHook {
        static void Thunk(RE::PlayerCharacter* player, RE::Actor* attacker, float damage, bool sendEvent, bool ragdollInstant) {
            if (DeathManager::TryInterceptDeath(player, attacker, ragdollInstant)) {
                return;
            }
            Function(player, attacker, damage, sendEvent, ragdollInstant);
        }

        static inline REL::Relocation<decltype(Thunk)> Function;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtable{ RE::PlayerCharacter::VTABLE[0] };
            Function = vtable.write_vfunc(REL::Relocate(0x10E, 0x10E, 0x110), Thunk);
        }
    };

    struct PlayerHealthDamageHook {
        static void Thunk(RE::PlayerCharacter* player, RE::Actor* attacker, float damage) {
            if (DeathManager::IsDamageBlocked()) {
                DeathManager::RepairBlockedPlayerHealth(player);
                return;
            }

            DeathManager::CaptureAppliedPlayerDamage(
                player,
                attacker);

            if (handlingDamage) {
                Function(player, attacker, damage);
                return;
            }

            handlingDamage = true;
            Function(player, attacker, damage);
            handlingDamage = false;
        }

        static inline REL::Relocation<decltype(Thunk)> Function;
        static inline thread_local bool handlingDamage = false;

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
                return 0.0F;
            }
            DeathManager::MarkPlayerFallDamage();
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

            logger::info("Combat acquisition hook installed for the defeated player.");
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
        NativeKnockExplosion,
        ReconcileState
    };

    const char* ToString(ManualRagdollTest test) {
        switch (test) {
        case ManualRagdollTest::AddRagdollToWorld:
            return "AddRagdollToWorld";
        case ManualRagdollTest::NativeKnockExplosion:
            return "AIProcessKnockExplosion";
        case ManualRagdollTest::ReconcileState:
            return "PotentiallyFixRagdollState";
        }
        return "unknown";
    }

    void AddPlayerRagdollsToWorld(RE::PlayerCharacter* player, std::string_view test) {
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!player->GetAnimationGraphManager(manager) || !manager) {
            logger::error("[ManualRagdollTest] test={}: no animation graph manager.", test);
            return;
        }

        for (std::uint32_t index = 0; index < manager->graphs.size(); ++index) {
            const auto& graph = manager->graphs[index];
            if (!graph) {
                continue;
            }
            graph->AddRagdollToWorld();
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

            switch (test) {
            case ManualRagdollTest::AddRagdollToWorld:
                // This is the native graph operation ultimately requested by Papyrus'
                // ForceAddRagdollToWorld. It can only insert a ragdoll that already exists.
                AddPlayerRagdollsToWorld(player, testName);
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
                process->KnockExplosion(player, location, magnitude);
                break;
            }
            case ManualRagdollTest::ReconcileState:
                player->PotentiallyFixRagdollState();
                break;
            }
            });
    }

    void RunManualTeleportBehindTargetTest() {
        SKSE::GetTaskInterface()->AddTask([] {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) {
                logger::error("[ManualTeleportTest] player or 3D is unavailable.");
                return;
            }
            if (DeathManager::IsMenuOpen()) {
                logger::warn("[ManualTeleportTest] rejected while the defeat UI is open.");
                return;
            }
            if (player->IsInRagdollState()) {
                logger::warn("[ManualTeleportTest] rejected while the player is in ragdoll.");
                return;
            }

            const auto controller = player->GetCharController();
            if (!controller || !controller->GetHavokWorld()) {
                logger::warn(
                    "[ManualTeleportTest] rejected: character controller is not in the Havok world.");
                return;
            }

            const auto* crosshair = RE::CrosshairPickData::GetSingleton();
            const auto targetReference = crosshair ? crosshair->GetActiveTarget().get() : nullptr;
            auto* target = targetReference ? targetReference->As<RE::Actor>() : nullptr;
            if (!target || target == player) {
                logger::warn("[ManualTeleportTest] rejected: crosshair is not pointing at another actor.");
                return;
            }
            if (!target->Is3DLoaded() || target->IsDead()) {
                logger::warn(
                    "[ManualTeleportTest] rejected: target {:08X} is dead or its 3D is unavailable.",
                    target->GetFormID());
                return;
            }
            if (target->GetParentCell() != player->GetParentCell()) {
                logger::warn(
                    "[ManualTeleportTest] rejected: target {:08X} is not in the player's current cell.",
                    target->GetFormID());
                return;
            }

            const bool targetHostileToPlayer = target->IsHostileToActor(player);
            const bool playerHostileToTarget = player->IsHostileToActor(target);
            if (!targetHostileToPlayer && !playerHostileToTarget) {
                logger::warn(
                    "[ManualTeleportTest] rejected: aimed actor {:08X} ({}) is not hostile.",
                    target->GetFormID(),
                    target->GetName());
                return;
            }

            constexpr float kDistanceBehind = 80.0F;
            const auto targetPosition = target->GetPosition();
            const float targetHeading = target->GetAngleZ();
            const RE::NiPoint3 targetForward{
                std::sin(targetHeading),
                std::cos(targetHeading),
                0.0F
            };
            RE::NiPoint3 destination = targetPosition - targetForward * kDistanceBehind;
            destination.z = targetPosition.z;

            player->SetPosition(destination, true);
            player->SetHeading(targetHeading);
            });
    }

    std::optional<ManualRagdollTest> GetManualRagdollTest(std::uint32_t keyCode) {
        using Keys = RE::BSWin32KeyboardDevice::Keys;
        if (keyCode == Keys::kNum5 || keyCode == Keys::kKP_5) return ManualRagdollTest::AddRagdollToWorld;
        if (keyCode == Keys::kNum7 || keyCode == Keys::kKP_7) return ManualRagdollTest::NativeKnockExplosion;
        if (keyCode == Keys::kNum9 || keyCode == Keys::kKP_9) return ManualRagdollTest::ReconcileState;
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
    using Keys = RE::BSWin32KeyboardDevice::Keys;
    if (keyCode == Keys::kNum8 || keyCode == Keys::kKP_8) {
        RunManualTeleportBehindTargetTest();
        return true;
    }
    if (const auto test = GetManualRagdollTest(keyCode)) {
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
    //ProcessInputQueueHook::install();
    //InputEventHandler::Register(OnInput);
    logger::info("Death and input hooks installed");
}

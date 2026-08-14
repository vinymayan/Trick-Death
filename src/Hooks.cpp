#include "Hooks.h"
#include "DeathManager.h"
#include "InputEventHandler.h"
#include "Prisma.h"

#include <MinHook.h>

namespace {
    struct PlayerKillHook {
        static void Thunk(RE::PlayerCharacter* player, RE::Actor* attacker, float damage, bool sendEvent, bool ragdollInstant) {
            if (DeathManager::TryInterceptDeath(player, attacker)) {
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
                return;
            }
            Function(player, attacker, damage);
        }

        static inline REL::Relocation<decltype(Thunk)> Function;

        static void Install() {
            REL::Relocation<std::uintptr_t> vtable{ RE::PlayerCharacter::VTABLE[0] };
            Function = vtable.write_vfunc(REL::Relocate(0x104, 0x104, 0x106), Thunk);
        }
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
        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();
        originalFunction = trampoline.write_call<5>(REL::RelocationID(67315, 68617, 67315).address() + REL::Relocate(0x7B, 0x7B, 0x81), thunk);
    }
};

bool OnInput(RE::InputEvent* event) { 
    if (!event) return false;
    if (event->device != RE::INPUT_DEVICE::kKeyboard) return false;
    auto button = event->AsButtonEvent();
    if (!button) return false;
    if (!button->IsDown()) return false;
    if (button->GetIDCode() == RE::BSWin32KeyboardDevice::Keys::kF2) {
#ifdef DEV_SERVER
        if (Prisma::IsHidden()) {
            Prisma::ShowDeathMenu(true);
        } else {
            Prisma::Hide();
        }
        return true;
#endif
    }
    return false;
}



void Hooks::Install() {
    PlayerKillHook::Install();
    PlayerHealthDamageHook::Install();
    StartCombatHook::Install();
    ProcessInputQueueHook::install();
    InputEventHandler::Register(OnInput);
    logger::info("Death and input hooks installed. Combat groups are not accessed or modified.");
}

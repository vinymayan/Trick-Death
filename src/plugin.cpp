#include "Plugin.h"
#include "CheckpointManager.h"
#include "Configuration.h"
#include "DeathManager.h"
#include "Hooks.h"
#include "MoreRagdollClient.h"
#include "PapyrusAPI.h"
#include "PlayerAnimationSink.h"
#include "Prisma.h"
#include "SerializationManager.h"

namespace {
    constexpr std::uint32_t SERIALIZATION_ID = 0x44544844;  // DTHD
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        MoreRagdollClient::Install();
        Prisma::Install();
        ModMenu::Register();
    } else if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        CheckpointManager::RegisterEvents();
        Prisma::Preload();
        PlayerAnimationSink::GetSingleton()->Install();
    } else if (message->type == SKSE::MessagingInterface::kNewGame ||
               message->type == SKSE::MessagingInterface::kPostLoadGame) {
        DeathManager::Reset();
        PlayerAnimationSink::GetSingleton()->Reconnect();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    if (auto serialization = SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(SERIALIZATION_ID);
        serialization->SetSaveCallback(SerializationManager::Save);
        serialization->SetLoadCallback(SerializationManager::Load);
        serialization->SetRevertCallback(SerializationManager::Revert);
    }
    if (auto papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(PapyrusAPI::Register);
    }
    SetupLog();
    logger::info("Plugin loaded");
    Hooks::Install();
    return true;
}

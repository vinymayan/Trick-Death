#include "Plugin.h"
#include "CheckpointManager.h"
#include "Configuration.h"
#include "CurrentSaveManager.h"
#include "DeathManager.h"
#include "DeathTrackerManager.h"
#include "Hooks.h"
#include "MoreRagdollClient.h"
#include "PapyrusAPI.h"
#include "PlayerAnimationSink.h"
#include "Prisma.h"
#include "SerializationManager.h"

namespace {
    constexpr std::uint32_t SERIALIZATION_ID = 0x44544844;  // DTHD
    bool hasDynamicFormsGenerator{ false };
    bool gameplaySettingsLoaded{ false };

    void LoadTypedGameplaySettings(std::string_view reason) {
        ModMenu::RefreshGlobalList();
        ModMenu::LoadSettings();
        gameplaySettingsLoaded = true;
        logger::info("Gameplay settings loaded after {}.", reason);
    }

    class DynamicFormsGeneratorListener final :
        public RE::BSTEventSink<SKSE::ModCallbackEvent> {
    public:
        static DynamicFormsGeneratorListener* GetSingleton() {
            static DynamicFormsGeneratorListener singleton;
            return std::addressof(singleton);
        }

        void Register() {
            if (const auto source = SKSE::GetModCallbackEventSource()) {
                source->AddEventSink(this);
            }
        }

        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const std::string_view eventName = event->eventName.c_str();
            if (eventName == "DynamicFormsGeneratorLoaded") {
                LoadTypedGameplaySettings("DynamicFormsGeneratorLoaded");
            } else if (eventName == "DynamicFormsGeneratorUpdated") {
                LoadTypedGameplaySettings("DynamicFormsGeneratorUpdated");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    std::string CopyMessageString(const SKSE::MessagingInterface::Message* message) {
        if (!message || !message->data) {
            return {};
        }
        const auto* value = static_cast<const char*>(message->data);
        if (message->dataLen == 0) {
            return value;
        }
        std::size_t length = 0;
        while (length < message->dataLen && value[length] != '\0') {
            ++length;
        }
        return std::string(value, length);
    }
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        hasDynamicFormsGenerator = GetModuleHandleA("DynamicFormsGenerator.dll") != nullptr;
        logger::info(
            "Dynamic Forms Generator detection: installed={}.",
            hasDynamicFormsGenerator);
        MoreRagdollClient::Install();
        Prisma::Install();
    } else if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        if (hasDynamicFormsGenerator) {
            ModMenu::Register(false);
            logger::info(
                "Gameplay settings load is waiting for DynamicFormsGeneratorLoaded so dynamic "
                "Global forms can be resolved.");
        } else {
            ModMenu::Register(true);
            gameplaySettingsLoaded = true;
        }
        CheckpointManager::RegisterEvents();
        Prisma::Preload();
        PlayerAnimationSink::GetSingleton()->Install();
    } else if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
        CurrentSaveManager::BeginLoad(CopyMessageString(message));
    } else if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
        // SKSE transports this bool as the pointer value itself: nullptr=false, 1=true.
        // It is not a pointer to readable bool storage.
        const bool success = message->data != nullptr;
        CurrentSaveManager::FinishLoad(success);
        DeathManager::OnGameLoadFinished(success);
        if (success) {
            if (hasDynamicFormsGenerator && !gameplaySettingsLoaded) {
                LoadTypedGameplaySettings("PostLoadGame fallback");
            }
            PlayerAnimationSink::GetSingleton()->Reconnect();
            DeathTrackerManager::ScheduleGraphSync();
        }
    } else if (message->type == SKSE::MessagingInterface::kNewGame) {
        if (hasDynamicFormsGenerator && !gameplaySettingsLoaded) {
            LoadTypedGameplaySettings("NewGame fallback");
        }
        CurrentSaveManager::ClearForNewGame();
        DeathManager::Reset();
        PlayerAnimationSink::GetSingleton()->Reconnect();
        DeathTrackerManager::ScheduleGraphSync();
    } else if (message->type == SKSE::MessagingInterface::kSaveGame) {
        CurrentSaveManager::RecordSave(CopyMessageString(message));
    } else if (message->type == SKSE::MessagingInterface::kDeleteGame) {
        CurrentSaveManager::RecordDelete(CopyMessageString(message));
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    DynamicFormsGeneratorListener::GetSingleton()->Register();
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

#include "Prisma.h"

#include "Configuration.h"
#include "DeathManager.h"
#include "PrismaUI_API.h"
#include "TextManager.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
    PRISMA_UI_API::IVPrismaUI1* prismaUI = nullptr;
    PrismaView view = 0;
    bool domReady = false;
    bool focused = false;
    bool menuVisible = false;
    bool pendingShow = false;
    std::uint32_t pendingAvailableRespawns = 0;

    std::string BuildSettingsPayload() {
        const auto title = TextManager::ResolveSlot("title", ModMenu::GetLoc("ui.title", "DEFEATED"));
        const auto backgroundText = DeathManager::GetBackgroundText();
        const auto lastSleep = TextManager::ResolveSlot(
            "respawn_last_sleep",
            ModMenu::GetLoc("ui.last_sleep", "Respawn at last place slept"));
        const auto checkpoint = TextManager::ResolveSlot(
            "respawn_checkpoint",
            ModMenu::GetLoc("ui.checkpoint", "Respawn at last checkpoint"));
        const auto respawnHere = TextManager::ResolveSlot(
            "respawn_here",
            ModMenu::GetLoc("ui.respawn", "Respawn here"));
        const auto reload = TextManager::ResolveSlot(
            "reload_save",
            ModMenu::GetLoc("ui.reload", "Reload save"));
        const auto unavailableHere = TextManager::ResolveSlot(
            "unavailable_here",
            ModMenu::GetLoc("ui.unavailable_here", "Respawn here is blocked"));
        const auto unavailableLastSleep = TextManager::ResolveSlot(
            "unavailable_last_sleep",
            ModMenu::GetLoc("ui.unavailable_last_sleep", "No available last-sleep destination"));
        const auto unavailableCheckpoint = TextManager::ResolveSlot(
            "unavailable_checkpoint",
            ModMenu::GetLoc("ui.unavailable_checkpoint", "No available external checkpoint"));
        const auto unavailableReload = TextManager::ResolveSlot(
            "unavailable_reload",
            ModMenu::GetLoc("ui.unavailable_reload", "Reloading the current save is blocked"));
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("backgroundOpacityPercent");
        writer.Int(Settings::UI.backgroundOpacityPercent);
        writer.Key("backgroundBlurPixels");
        writer.Int(Settings::UI.backgroundBlurPixels);
        writer.Key("scalePercent");
        writer.Int(Settings::UI.scalePercent);
        writer.Key("titleTextSizePercent");
        writer.Int(Settings::UI.titleTextSizePercent);
        writer.Key("backgroundTextSizePercent");
        writer.Int(Settings::UI.backgroundTextSizePercent);
        writer.Key("actionStyles");
        writer.StartObject();
        const auto writeActionStyle = [&](const char* key, const Settings::ActionStyle& style) {
            writer.Key(key);
            writer.StartObject();
            writer.Key("textSizePercent");
            writer.Int(style.textSizePercent);
            writer.Key("buttonScalePercent");
            writer.Int(style.buttonScalePercent);
            writer.EndObject();
        };
        writeActionStyle("respawn_here", Settings::UI.respawnHere);
        writeActionStyle("respawn_last_sleep", Settings::UI.lastSleep);
        writeActionStyle("respawn_checkpoint", Settings::UI.lastCheckpoint);
        writeActionStyle("reload_save", Settings::UI.reloadSave);
        writer.EndObject();
        writer.Key("labels");
        writer.StartObject();
        writer.Key("title");
        writer.String(title.c_str());
        writer.Key("backgroundText");
        writer.String(backgroundText.c_str());
        writer.Key("lastSleep");
        writer.String(lastSleep.c_str());
        writer.Key("checkpoint");
        writer.String(checkpoint.c_str());
        writer.Key("respawn");
        writer.String(respawnHere.c_str());
        writer.Key("reload");
        writer.String(reload.c_str());
        writer.Key("unavailableHere");
        writer.String(unavailableHere.c_str());
        writer.Key("unavailableLastSleep");
        writer.String(unavailableLastSleep.c_str());
        writer.Key("unavailableCheckpoint");
        writer.String(unavailableCheckpoint.c_str());
        writer.Key("unavailableReload");
        writer.String(unavailableReload.c_str());
        writer.EndObject();
        writer.EndObject();
        return buffer.GetString();
    }

    void SendSettings() {
        if (!prismaUI || !view || !domReady || !prismaUI->IsValid(view)) {
            return;
        }
        const auto payload = BuildSettingsPayload();
        prismaUI->InteropCall(view, "applyDeathMenuSettings", payload.c_str());
    }

    void SendShow(std::uint32_t availableRespawns) {
        if (!prismaUI || !view || !domReady || !prismaUI->IsValid(view)) {
            pendingShow = true;
            pendingAvailableRespawns = availableRespawns;
            return;
        }

        SendSettings();
        prismaUI->Show(view);
        const auto payload = std::to_string(availableRespawns);
        prismaUI->InteropCall(view, "showDeathMenu", payload.c_str());
        if (focused) {
            prismaUI->Unfocus(view);
            focused = false;
        }
        focused = prismaUI->Focus(view, Settings::Gameplay.pauseGameWhileMenuOpen);
        menuVisible = true;
        pendingShow = false;
    }

    bool CreateView() {
        if (!prismaUI) {
            return false;
        }
        if (view && prismaUI->IsValid(view)) {
            return true;
        }

#ifdef DEV_SERVER
        constexpr const char* path = "http://localhost:5173";
#else
        constexpr const char* path = PRODUCT_NAME "/index.html";
#endif

        domReady = false;
        view = prismaUI->CreateView(path, [](PrismaView readyView) {
            if (!prismaUI || !readyView) {
                return;
            }
            view = readyView;
            domReady = true;
            SendSettings();
            if (pendingShow) {
                SendShow(pendingAvailableRespawns);
            } else {
                prismaUI->Hide(view);
            }
        });

        if (!view) {
            logger::error("PrismaUI failed to create the Trick Death view.");
            return false;
        }

        prismaUI->RegisterJSListener(view, "deathMenuAction", [](const char* data) {
            if (data) {
                DeathManager::HandleUIAction(data);
            }
        });
        prismaUI->SetOrder(view, 100);
        return true;
    }
}

void Prisma::Install() {
    prismaUI = reinterpret_cast<PRISMA_UI_API::IVPrismaUI1*>(PRISMA_UI_API::RequestPluginAPI());
    if (!prismaUI) {
        logger::error("Could not obtain the PrismaUI API.");
        return;
    }
    logger::info("PrismaUI API linked.");
}

void Prisma::Preload() {
    CreateView();
}

void Prisma::Hide() {
    pendingShow = false;
    menuVisible = false;
    if (!prismaUI || !view) {
        return;
    }
    if (focused) {
        prismaUI->Unfocus(view);
        focused = false;
    }
    if (prismaUI->IsValid(view)) {
        prismaUI->InteropCall(view, "hideDeathMenu", "");
        prismaUI->Hide(view);
    }
}

bool Prisma::IsHidden() {
    return !prismaUI || !view || !prismaUI->IsValid(view) || prismaUI->IsHidden(view);
}

bool Prisma::IsReady() {
    return prismaUI && view && domReady && prismaUI->IsValid(view);
}

bool Prisma::CanShow() {
    return prismaUI && CreateView();
}

void Prisma::ShowDeathMenu(std::uint32_t availableRespawns) {
    pendingShow = true;
    pendingAvailableRespawns = availableRespawns;
    if (CreateView()) {
        SendShow(availableRespawns);
    }
}

void Prisma::ShowError(const char* message) {
    if (IsReady() && message) {
        prismaUI->InteropCall(view, "showDeathMenuError", message);
    }
}

void Prisma::ApplyUISettings() {
    SendSettings();
    if (menuVisible && IsReady()) {
        if (focused) {
            prismaUI->Unfocus(view);
            focused = false;
        }
        focused = prismaUI->Focus(view, Settings::Gameplay.pauseGameWhileMenuOpen);
    }
}

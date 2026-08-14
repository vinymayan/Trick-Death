#include "Prisma.h"

#include "Configuration.h"
#include "DeathManager.h"
#include "PrismaUI_API.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
    PRISMA_UI_API::IVPrismaUI1* prismaUI = nullptr;
    PrismaView view = 0;
    bool domReady = false;
    bool focused = false;
    bool menuVisible = false;
    bool pendingShow = false;
    bool pendingCheckpointAvailable = false;

    std::string BuildSettingsPayload() {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("backgroundOpacityPercent");
        writer.Int(Settings::UI.backgroundOpacityPercent);
        writer.Key("backgroundBlurPixels");
        writer.Int(Settings::UI.backgroundBlurPixels);
        writer.Key("scalePercent");
        writer.Int(Settings::UI.scalePercent);
        writer.Key("labels");
        writer.StartObject();
        writer.Key("title");
        writer.String(ModMenu::GetLoc("ui.title", "DEFEATED"));
        writer.Key("checkpoint");
        writer.String(ModMenu::GetLoc("ui.checkpoint", "Respawn at last sleep checkpoint"));
        writer.Key("respawn");
        writer.String(ModMenu::GetLoc("ui.respawn", "Respawn here"));
        writer.Key("load");
        writer.String(ModMenu::GetLoc("ui.load", "Load last save"));
        writer.Key("noCheckpoint");
        writer.String(ModMenu::GetLoc("ui.no_checkpoint", "No sleep checkpoint available"));
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

    void SendShow(bool checkpointAvailable) {
        if (!prismaUI || !view || !domReady || !prismaUI->IsValid(view)) {
            pendingShow = true;
            pendingCheckpointAvailable = checkpointAvailable;
            return;
        }

        SendSettings();
        prismaUI->Show(view);
        prismaUI->InteropCall(view, "showDeathMenu", checkpointAvailable ? "1" : "0");
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
                SendShow(pendingCheckpointAvailable);
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

void Prisma::ShowDeathMenu(bool checkpointAvailable) {
    pendingShow = true;
    pendingCheckpointAvailable = checkpointAvailable;
    if (CreateView()) {
        SendShow(checkpointAvailable);
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

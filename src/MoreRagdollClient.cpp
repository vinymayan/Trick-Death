#include "MoreRagdollClient.h"

#include "MoreRagdollAPI.h"

#include <atomic>

namespace {
    std::atomic<MoreRagdollAPI::Interface*> moreRagdollAPI{ nullptr };
    std::atomic_bool resolutionAttempted{ false };

    MoreRagdollAPI::Interface* GetAPI() {
        if (!resolutionAttempted.load()) {
            MoreRagdollClient::Install();
        }
        return moreRagdollAPI.load();
    }
}

void MoreRagdollClient::Install() {
    if (resolutionAttempted.exchange(true)) {
        return;
    }

    const auto module = GetModuleHandleW(L"MoreRagdoll.dll");
    if (!module) {
        logger::info(
            "More Ragdoll DLL was not found; persistent ragdoll requests will use animation events.");
        return;
    }

    const auto getInterface = reinterpret_cast<MoreRagdollAPI::GetInterface_t>(
        GetProcAddress(module, "GetMoreRagdollAPI"));
    if (!getInterface) {
        logger::warn(
            "More Ragdoll DLL does not export GetMoreRagdollAPI; falling back to animation events.");
        return;
    }

    auto* api = getInterface();
    if (!api) {
        logger::warn("More Ragdoll returned a null API; falling back to animation events.");
        return;
    }

    const auto version = api->GetVersion();
    if (version < MoreRagdollAPI::API_VERSION) {
        logger::warn(
            "More Ragdoll API version {} is older than required version {}; "
            "falling back to animation events.",
            version,
            MoreRagdollAPI::API_VERSION);
        return;
    }

    moreRagdollAPI.store(api);
    logger::info("More Ragdoll API version {} acquired.", version);
}

MoreRagdollClient::RequestResult MoreRagdollClient::Enable(
    RE::Actor* actor,
    bool adoptOnly,
    float safetyTimeoutSeconds) {
    if (!actor) {
        return {};
    }

    if (auto* api = GetAPI()) {
        const bool accepted = adoptOnly ?
            api->Adopt(actor, safetyTimeoutSeconds) :
            api->Enable(actor, safetyTimeoutSeconds);
        if (accepted) {
            return { true, Route::API };
        }
        logger::warn(
            "More Ragdoll API rejected the {} request for {:08X}; trying the animation event fallback.",
            adoptOnly ? "adopt" : "enable",
            actor->GetFormID());
    }

    const auto eventName = adoptOnly ? "MoreRagdollAdopt" : "MoreRagdollEnable";
    return { actor->NotifyAnimationGraph(eventName), Route::AnimationEvent };
}

MoreRagdollClient::RequestResult MoreRagdollClient::Disable(RE::Actor* actor) {
    if (!actor) {
        return {};
    }

    if (auto* api = GetAPI()) {
        if (api->Disable(actor)) {
            return { true, Route::API };
        }
        logger::warn(
            "More Ragdoll API rejected the disable request for {:08X}; "
            "trying the animation event fallback.",
            actor->GetFormID());
    }

    return { actor->NotifyAnimationGraph("MoreRagdollDisable"), Route::AnimationEvent };
}

const char* MoreRagdollClient::ToString(Route route) {
    switch (route) {
    case Route::API:
        return "C++ API";
    case Route::AnimationEvent:
        return "animation event fallback";
    }
    return "unknown";
}

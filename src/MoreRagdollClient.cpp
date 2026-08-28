#include "MoreRagdollClient.h"

#include "MoreRagdollAPI.h"

#include <atomic>

namespace {
    std::atomic<MoreRagdollAPI::Interface*> moreRagdollAPI{ nullptr };
    std::atomic_bool resolutionAttempted{ false };

    MoreRagdollAPI::Interface* GetAPI() {
        if (!moreRagdollAPI.load()) {
            MoreRagdollClient::Install();
        }
        return moreRagdollAPI.load();
    }
}

void MoreRagdollClient::Install() {
    if (moreRagdollAPI.load()) {
        return;
    }

    const bool firstAttempt = !resolutionAttempted.exchange(true);

    const auto module = GetModuleHandleW(L"MoreRagdoll.dll");
    if (!module) {
        if (firstAttempt) {
            logger::info(
                "More Ragdoll DLL was not found yet; ragdoll requests will use "
                "animation events and retry API discovery when needed.");
        }
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
    if (version != MoreRagdollAPI::API_VERSION) {
        logger::warn(
            "More Ragdoll API version {} does not match required version {}; "
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

    const auto actorFormID = actor->GetFormID();
    if (actorFormID == 0) {
        return {};
    }

    if (auto* api = GetAPI()) {
        const bool accepted = adoptOnly ?
            api->Adopt(actorFormID, safetyTimeoutSeconds) :
            api->StartRagdoll(actorFormID, safetyTimeoutSeconds, true);
        if (accepted) {
            return { true, Route::API };
        }
        logger::warn(
            "More Ragdoll API rejected the {} request for {:08X}; trying the animation event fallback.",
            adoptOnly ? "adopt" : "start ragdoll",
            actorFormID);
    }

    // These legacy commands are understood by both the current provider and
    // older More Ragdoll builds when the C++ ABI cannot be used.
    const auto eventName = adoptOnly ? "MoreRagdollAdopt" : "MoreRagdollEnable";
    return { actor->NotifyAnimationGraph(eventName), Route::AnimationEvent };
}

MoreRagdollClient::RequestResult MoreRagdollClient::Disable(RE::Actor* actor) {
    if (!actor) {
        return {};
    }

    const auto actorFormID = actor->GetFormID();
    if (actorFormID == 0) {
        return {};
    }

    if (auto* api = GetAPI()) {
        if (api->Disable(actorFormID)) {
            return { true, Route::API };
        }
        logger::warn(
            "More Ragdoll API rejected the disable request for {:08X}; "
            "trying the animation event fallback.",
            actorFormID);
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

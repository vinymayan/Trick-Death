#pragma once

namespace RE {
    class Actor;
}

namespace MoreRagdollClient {
    enum class Route {
        API,
        AnimationEvent
    };

    struct RequestResult {
        bool accepted{ false };
        Route route{ Route::AnimationEvent };
    };

    void Install();
    RequestResult Enable(RE::Actor* actor, bool adoptOnly, float safetyTimeoutSeconds = 120.0F);
    RequestResult Disable(RE::Actor* actor);
    const char* ToString(Route route);
}

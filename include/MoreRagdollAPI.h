#pragma once

#include <cstdint>

namespace RE {
    class Actor;
}

namespace MoreRagdollAPI {
    inline constexpr std::uint32_t API_VERSION = 2;

    class Interface {
    public:
        virtual ~Interface() = default;
        virtual std::uint32_t GetVersion() const noexcept = 0;

        virtual bool Enable(RE::Actor* actor, float safetyTimeoutSeconds) = 0;
        virtual bool Disable(RE::Actor* actor) = 0;
        virtual bool Hold(RE::Actor* actor, float seconds) = 0;
        virtual bool IsHeld(RE::Actor* actor) const = 0;
        virtual bool Adopt(RE::Actor* actor, float safetyTimeoutSeconds) = 0;
    };

    using GetInterface_t = Interface* (*)();
}

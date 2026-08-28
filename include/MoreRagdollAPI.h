#pragma once

#include <cstdint>

namespace MoreRagdollAPI {
    inline constexpr std::uint32_t API_VERSION = 1;

    class Interface {
    public:
        virtual ~Interface() = default;
        virtual std::uint32_t GetVersion() const noexcept = 0;

        // Acquires control of an existing ragdoll when the actor is already down.
        // Otherwise, enters ragdoll through the engine's native knock-explosion path.
        virtual bool Enable(std::uint32_t actorFormID, float safetyTimeoutSeconds) = 0;
        virtual bool Disable(std::uint32_t actorFormID) = 0;

        // Uses the same acquire-or-enter behavior as Enable, with a timed release.
        virtual bool Hold(std::uint32_t actorFormID, float seconds) = 0;
        virtual bool IsHeld(std::uint32_t actorFormID) const = 0;

        // Arms the hold without requesting a new ragdoll. Use this when the
        // caller knows that another system has already requested the ragdoll.
        virtual bool Adopt(std::uint32_t actorFormID, float safetyTimeoutSeconds) = 0;

        // Starts (or adopts) a ragdoll and holds it for durationSeconds.
        // When forceGetUpOnTimeout is true, the timeout also reconciles the
        // native ragdoll state and sends GetUpBegin. When it is false, the
        // timeout only releases More Ragdoll's hold and lets the engine get
        // the actor up naturally when its normal ground conditions are met.
        virtual bool StartRagdoll(
            std::uint32_t actorFormID,
            float durationSeconds,
            bool forceGetUpOnTimeout) = 0;
    };

    using GetInterface_t = Interface* (*)();
}

extern "C" __declspec(dllexport) MoreRagdollAPI::Interface* GetMoreRagdollAPI();

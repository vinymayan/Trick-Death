#pragma once

#include <cstdint>

namespace Respawn {
    enum class Option : std::uint32_t {
        None = 0,
        Here = 1U << 0,
        LastSleep = 1U << 1,
        LastCheckpoint = 1U << 2,
        ReloadSave = 1U << 3,
        LoadLastSave = ReloadSave,  // compatibility alias for API v1 integrations
        DisableTrickDeath = 1U << 4
    };

    inline constexpr std::uint32_t ACTION_MASK =
        static_cast<std::uint32_t>(Option::Here) |
        static_cast<std::uint32_t>(Option::LastSleep) |
        static_cast<std::uint32_t>(Option::LastCheckpoint) |
        static_cast<std::uint32_t>(Option::ReloadSave);
    inline constexpr std::uint32_t POLICY_MASK =
        ACTION_MASK | static_cast<std::uint32_t>(Option::DisableTrickDeath);

    [[nodiscard]] constexpr std::uint32_t ToMask(Option option) noexcept {
        return static_cast<std::uint32_t>(option);
    }

    [[nodiscard]] constexpr bool Contains(std::uint32_t mask, Option option) noexcept {
        return (mask & ToMask(option)) != 0;
    }

    [[nodiscard]] constexpr const char* ToString(Option option) noexcept {
        switch (option) {
        case Option::Here:
            return "respawn_here";
        case Option::LastSleep:
            return "respawn_last_sleep";
        case Option::LastCheckpoint:
            return "respawn_checkpoint";
        case Option::ReloadSave:
            return "reload_save";
        case Option::DisableTrickDeath:
            return "disable_trick_death";
        case Option::None:
            break;
        }
        return "none";
    }
}

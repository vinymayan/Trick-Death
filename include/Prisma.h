#pragma once

#include <cstdint>

class Prisma {
public:
    static void Install();
    static void Preload();
    static void Hide();
    static bool IsHidden();
    static bool IsReady();
    static bool CanShow();
    static void ShowDeathMenu(std::uint32_t availableRespawns);
    static void ShowBorrowedTime(std::uint32_t durationMilliseconds, std::int32_t streak);
    static void ShowError(const char* message);
    static void ApplyUISettings();
};

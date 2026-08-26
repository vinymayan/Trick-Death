#pragma once

class Prisma {
public:
    static void Install();
    static void Preload();
    static void Hide();
    static bool IsHidden();
    static bool IsReady();
    static bool CanShow();
    static void ShowDeathMenu(std::uint32_t availableRespawns);
    static void ShowError(const char* message);
    static void ApplyUISettings();
};

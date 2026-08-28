#pragma once

#include <string>
#include <string_view>

namespace CurrentSaveManager {
    void BeginLoad(std::string saveName);
    void FinishLoad(bool success);
    void RecordSave(std::string saveName);
    void RecordDelete(std::string_view saveName);
    void ClearForNewGame();

    [[nodiscard]] bool HasCurrentSave();
    [[nodiscard]] std::string GetCurrentSaveName();
}

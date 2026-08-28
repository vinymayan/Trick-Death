#include "CurrentSaveManager.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace {
    std::mutex saveLock;
    std::string currentSaveName;
    std::string pendingLoadSaveName;

    std::string Sanitize(std::string value) {
        if (const auto terminator = value.find('\0'); terminator != std::string::npos) {
            value.resize(terminator);
        }
        return value;
    }

    std::string ComparisonKey(std::string_view value) {
        const auto separator = value.find_last_of("/\\");
        if (separator != std::string_view::npos) {
            value.remove_prefix(separator + 1);
        }
        if (value.ends_with(".ess") || value.ends_with(".ESS")) {
            value.remove_suffix(4);
        }
        std::string result(value);
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }
}

void CurrentSaveManager::BeginLoad(std::string saveName) {
    saveName = Sanitize(std::move(saveName));
    std::scoped_lock lock(saveLock);
    pendingLoadSaveName = std::move(saveName);
}

void CurrentSaveManager::FinishLoad(bool success) {
    std::scoped_lock lock(saveLock);
    if (success && !pendingLoadSaveName.empty()) {
        currentSaveName = pendingLoadSaveName;
    } else if (!success) {
        logger::warn(
            "Save load failed; keeping the previously confirmed current save '{}'.",
            currentSaveName);
    }
    pendingLoadSaveName.clear();
}

void CurrentSaveManager::RecordSave(std::string saveName) {
    saveName = Sanitize(std::move(saveName));
    if (saveName.empty()) {
        return;
    }
    std::scoped_lock lock(saveLock);
    currentSaveName = std::move(saveName);
}

void CurrentSaveManager::RecordDelete(std::string_view saveName) {
    const auto deletedKey = ComparisonKey(saveName);
    if (deletedKey.empty()) {
        return;
    }
    std::scoped_lock lock(saveLock);
    if (ComparisonKey(currentSaveName) == deletedKey) {
        currentSaveName.clear();
    }
}

void CurrentSaveManager::ClearForNewGame() {
    std::scoped_lock lock(saveLock);
    currentSaveName.clear();
    pendingLoadSaveName.clear();
}

bool CurrentSaveManager::HasCurrentSave() {
    std::scoped_lock lock(saveLock);
    return !currentSaveName.empty();
}

std::string CurrentSaveManager::GetCurrentSaveName() {
    std::scoped_lock lock(saveLock);
    return currentSaveName;
}

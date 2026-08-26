#include "TextManager.h"

#include "CheckpointManager.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

namespace {
    constexpr std::uint32_t MAX_ITEMS = 2048;
    constexpr std::uint32_t MAX_STRING = 4096;
    constexpr std::array<std::string_view, 9> VALID_SLOTS{
        "title",
        "respawn_here",
        "respawn_last_sleep",
        "respawn_checkpoint",
        "load_last_save",
        "unavailable_here",
        "unavailable_last_sleep",
        "unavailable_checkpoint",
        "unavailable_load"
    };

    std::mutex textLock;
    std::vector<TextManager::OverrideInfo> overrides;
    std::vector<TextManager::VariableInfo> variables;
    std::unordered_map<std::string, std::string> runtimeVariables;
    std::uint64_t nextSequence = 1;

    std::string NormalizeSlot(std::string_view slot) {
        if (slot == "defeated") return "title";
        if (slot == "respawn" || slot == "here") return "respawn_here";
        if (slot == "last_sleep") return "respawn_last_sleep";
        if (slot == "checkpoint") return "respawn_checkpoint";
        if (slot == "load") return "load_last_save";
        return std::string(slot);
    }

    std::string Trimmed(std::string value, std::size_t maximum = MAX_STRING) {
        if (value.size() > maximum) {
            value.resize(maximum);
        }
        return value;
    }

    std::unordered_map<std::string, std::string> BuildVariableSnapshot() {
        std::unordered_map<std::string, std::string> result;
        {
            std::scoped_lock lock(textLock);
            std::vector<TextManager::VariableInfo> ordered = variables;
            std::ranges::sort(ordered, {}, &TextManager::VariableInfo::sequence);
            for (const auto& variable : ordered) {
                result[variable.key] = variable.value;
            }
            for (const auto& [key, value] : runtimeVariables) {
                result[key] = value;
            }
        }

        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            if (const auto name = player->GetName(); name && name[0] != '\0') {
                result["player.name"] = name;
            }
            if (auto cell = player->GetParentCell()) {
                if (auto location = cell->GetLocation()) {
                    if (const auto name = location->GetFullName(); name && name[0] != '\0') {
                        result["current.location"] = name;
                    }
                }
                if (!result.contains("current.location")) {
                    if (const auto name = cell->GetFullName(); name && name[0] != '\0') {
                        result["current.location"] = name;
                    }
                }
            }
        }
        const auto sleep = CheckpointManager::GetLastSleepInfo();
        const auto checkpoint = CheckpointManager::GetCheckpointInfo();
        result["last_sleep.name"] = sleep.active ? sleep.name : "";
        result["last_sleep.location"] = sleep.active ? sleep.name : "";
        result["checkpoint.name"] = checkpoint.active ? checkpoint.name : "";
        result["checkpoint.location"] = checkpoint.active ? checkpoint.name : "";
        return result;
    }

    std::string ResolveWithVariables(
        std::string_view input,
        const std::unordered_map<std::string, std::string>& snapshot)
    {
        std::string output;
        output.reserve(input.size());
        for (std::size_t index = 0; index < input.size();) {
            const bool doubleBrace = input.substr(index).starts_with("{{$");
            const bool singleBrace = !doubleBrace && input.substr(index).starts_with("{$");
            if (!doubleBrace && !singleBrace) {
                output.push_back(input[index++]);
                continue;
            }
            const auto tokenStart = index;
            const auto keyStart = index + (doubleBrace ? 3 : 2);
            const auto suffix = doubleBrace ? "}}"sv : "}"sv;
            const auto close = input.find(suffix, keyStart);
            if (close == std::string_view::npos) {
                output.append(input.substr(index));
                break;
            }
            const std::string key(input.substr(keyStart, close - keyStart));
            const auto value = snapshot.find(key);
            if (!key.empty() && value != snapshot.end()) {
                output.append(value->second);
            } else {
                const auto tokenEnd = close + suffix.size();
                output.append(input.substr(tokenStart, tokenEnd - tokenStart));
            }
            index = close + suffix.size();
        }
        return output;
    }

    template <class T>
    bool ReadValue(SKSE::SerializationInterface* serialization, T& value) {
        return serialization->ReadRecordData(value) == sizeof(value);
    }

    bool WriteString(SKSE::SerializationInterface* serialization, const std::string& value) {
        const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), MAX_STRING));
        return serialization->WriteRecordData(length) &&
            (length == 0 || serialization->WriteRecordData(value.data(), length));
    }

    bool ReadString(SKSE::SerializationInterface* serialization, std::string& value) {
        std::uint32_t length = 0;
        if (!ReadValue(serialization, length) || length > MAX_STRING) {
            return false;
        }
        value.resize(length);
        return length == 0 || serialization->ReadRecordData(value.data(), length) == length;
    }
}

bool TextManager::IsValidSlot(std::string_view slot) {
    const auto normalized = NormalizeSlot(slot);
    return std::ranges::find(VALID_SLOTS, normalized) != VALID_SLOTS.end();
}

bool TextManager::SetOverride(
    RE::TESForm* owner,
    std::string slot,
    std::string textTemplate,
    std::int32_t priority,
    bool persistent)
{
    slot = NormalizeSlot(slot);
    if (!owner || !IsValidSlot(slot)) {
        logger::warn("Rejected invalid Trick Death text override slot '{}'.", slot);
        return false;
    }
    slot = Trimmed(std::move(slot), 64);
    textTemplate = Trimmed(std::move(textTemplate));
    std::scoped_lock lock(textLock);
    const auto ownerID = owner->GetFormID();
    const auto it = std::ranges::find_if(overrides, [&](const auto& item) {
        return item.ownerFormID == ownerID && item.slot == slot;
    });
    if (textTemplate.empty()) {
        if (it != overrides.end()) {
            overrides.erase(it);
        }
        return true;
    }
    OverrideInfo updated{ ownerID, std::move(slot), std::move(textTemplate), priority, nextSequence++, persistent };
    if (it == overrides.end()) {
        overrides.push_back(std::move(updated));
    } else {
        *it = std::move(updated);
    }
    return true;
}

bool TextManager::SetVariable(
    RE::TESForm* owner,
    std::string key,
    std::string value,
    bool persistent)
{
    if (!owner || key.empty() || key.size() > 128) {
        return false;
    }
    key = Trimmed(std::move(key), 128);
    value = Trimmed(std::move(value));
    std::scoped_lock lock(textLock);
    const auto ownerID = owner->GetFormID();
    const auto it = std::ranges::find_if(variables, [&](const auto& item) {
        return item.ownerFormID == ownerID && item.key == key;
    });
    if (value.empty()) {
        if (it != variables.end()) {
            variables.erase(it);
        }
        return true;
    }
    VariableInfo updated{ ownerID, std::move(key), std::move(value), nextSequence++, persistent };
    if (it == variables.end()) {
        variables.push_back(std::move(updated));
    } else {
        *it = std::move(updated);
    }
    return true;
}

std::size_t TextManager::ClearOwner(RE::TESForm* owner) {
    if (!owner) {
        return 0;
    }
    const auto ownerID = owner->GetFormID();
    std::scoped_lock lock(textLock);
    const auto previous = overrides.size() + variables.size();
    std::erase_if(overrides, [&](const auto& item) { return item.ownerFormID == ownerID; });
    std::erase_if(variables, [&](const auto& item) { return item.ownerFormID == ownerID; });
    return previous - overrides.size() - variables.size();
}

void TextManager::SetRuntimeVariable(std::string key, std::string value) {
    if (key.empty()) {
        return;
    }
    std::scoped_lock lock(textLock);
    runtimeVariables[Trimmed(std::move(key), 128)] = Trimmed(std::move(value));
}

void TextManager::ClearRuntimeVariables() {
    std::scoped_lock lock(textLock);
    runtimeVariables.clear();
}

std::string TextManager::ResolveSlot(std::string_view slot, std::string_view fallback) {
    std::string text(fallback);
    {
        std::scoped_lock lock(textLock);
        const OverrideInfo* selected = nullptr;
        for (const auto& item : overrides) {
            if (item.slot != slot) {
                continue;
            }
            if (!selected || item.priority > selected->priority ||
                (item.priority == selected->priority && item.sequence > selected->sequence)) {
                selected = std::addressof(item);
            }
        }
        if (selected) {
            text = selected->textTemplate;
        }
    }
    return ResolveWithVariables(text, BuildVariableSnapshot());
}

std::string TextManager::ResolveTemplate(std::string_view textTemplate) {
    return ResolveWithVariables(textTemplate, BuildVariableSnapshot());
}

std::vector<TextManager::OverrideInfo> TextManager::GetOverrides() {
    std::scoped_lock lock(textLock);
    return overrides;
}

std::vector<TextManager::VariableInfo> TextManager::GetVariables() {
    std::scoped_lock lock(textLock);
    return variables;
}

void TextManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization || !serialization->OpenRecord(RECORD_TYPE, RECORD_VERSION)) {
        return;
    }
    std::scoped_lock lock(textLock);
    const auto overrideCount = static_cast<std::uint32_t>(std::ranges::count_if(
        overrides,
        [](const auto& item) { return item.persistent; }));
    const auto variableCount = static_cast<std::uint32_t>(std::ranges::count_if(
        variables,
        [](const auto& item) { return item.persistent; }));
    serialization->WriteRecordData(nextSequence);
    serialization->WriteRecordData(overrideCount);
    for (const auto& item : overrides) {
        if (!item.persistent) {
            continue;
        }
        serialization->WriteRecordData(item.ownerFormID);
        WriteString(serialization, item.slot);
        WriteString(serialization, item.textTemplate);
        serialization->WriteRecordData(item.priority);
        serialization->WriteRecordData(item.sequence);
    }
    serialization->WriteRecordData(variableCount);
    for (const auto& item : variables) {
        if (!item.persistent) {
            continue;
        }
        serialization->WriteRecordData(item.ownerFormID);
        WriteString(serialization, item.key);
        WriteString(serialization, item.value);
        serialization->WriteRecordData(item.sequence);
    }
}

bool TextManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || type != RECORD_TYPE || version != RECORD_VERSION) {
        return false;
    }
    std::uint64_t savedNextSequence = 1;
    std::uint32_t overrideCount = 0;
    if (!ReadValue(serialization, savedNextSequence) ||
        !ReadValue(serialization, overrideCount) || overrideCount > MAX_ITEMS) {
        logger::warn("Ignored invalid v1 text record.");
        return true;
    }
    std::vector<OverrideInfo> loadedOverrides;
    for (std::uint32_t index = 0; index < overrideCount; ++index) {
        RE::FormID savedOwner = 0;
        OverrideInfo item;
        if (!ReadValue(serialization, savedOwner) ||
            !ReadString(serialization, item.slot) ||
            !ReadString(serialization, item.textTemplate) ||
            !ReadValue(serialization, item.priority) ||
            !ReadValue(serialization, item.sequence)) {
            return true;
        }
        item.slot = NormalizeSlot(item.slot);
        if (!serialization->ResolveFormID(savedOwner, item.ownerFormID) || !IsValidSlot(item.slot)) {
            continue;
        }
        item.persistent = true;
        loadedOverrides.push_back(std::move(item));
    }
    std::uint32_t variableCount = 0;
    if (!ReadValue(serialization, variableCount) || variableCount > MAX_ITEMS) {
        return true;
    }
    std::vector<VariableInfo> loadedVariables;
    for (std::uint32_t index = 0; index < variableCount; ++index) {
        RE::FormID savedOwner = 0;
        VariableInfo item;
        if (!ReadValue(serialization, savedOwner) ||
            !ReadString(serialization, item.key) ||
            !ReadString(serialization, item.value) ||
            !ReadValue(serialization, item.sequence)) {
            return true;
        }
        if (!serialization->ResolveFormID(savedOwner, item.ownerFormID)) {
            continue;
        }
        item.persistent = true;
        loadedVariables.push_back(std::move(item));
    }
    std::scoped_lock lock(textLock);
    overrides = std::move(loadedOverrides);
    variables = std::move(loadedVariables);
    nextSequence = std::max<std::uint64_t>(savedNextSequence, 1);
    return true;
}

void TextManager::Revert() {
    std::scoped_lock lock(textLock);
    overrides.clear();
    variables.clear();
    runtimeVariables.clear();
    nextSequence = 1;
}

#include "APIDebugMenu.h"

#include "SKSEMCP/SKSEMenuFramework.hpp"

#include "CheckpointManager.h"
#include "CurrentSaveManager.h"
#include "DeathManager.h"
#include "DeathTrackerManager.h"
#include "Prisma.h"
#include "RespawnPolicyManager.h"
#include "TextManager.h"
#include "TrickDeathAPI.h"

#include <array>
#include <string>

namespace {
    struct DebugState {
        char checkpointName[256]{ "Debug checkpoint" };
        std::uint32_t checkpointBlockedMask{ 0 };

        int policyScope{ 0 };
        std::uint32_t policyBlockedMask{ 0 };
        bool policyPersistent{ false };

        int textSlot{ 0 };
        char textTemplate[1024]{ "DEFEATED AT {$death.location}" };
        int textPriority{ 100 };
        bool textPersistent{ false };

        char variableKey[128]{ "debug.text" };
        char variableValue[512]{ "Debug" };
        bool variablePersistent{ false };

        std::string status{ "Ready. Debug calls use the player reference as their owner form." };
    };

    DebugState debug;

    constexpr std::array<const char*, 10> TEXT_SLOTS{
        "title",
        "background_text",
        "respawn_here",
        "respawn_last_sleep",
        "respawn_checkpoint",
        "reload_save",
        "unavailable_here",
        "unavailable_last_sleep",
        "unavailable_checkpoint",
        "unavailable_reload"
    };

    void MaskCheckbox(const char* label, std::uint32_t& mask, std::uint32_t flag) {
        bool enabled = (mask & flag) != 0;
        if (ImGui::Checkbox(label, std::addressof(enabled))) {
            if (enabled) {
                mask |= flag;
            } else {
                mask &= ~flag;
            }
        }
    }

    void RenderActionMask(const char* id, std::uint32_t& mask, bool includeDisable) {
        ImGui::PushID(id);
        MaskCheckbox("Respawn here", mask, TRICK_DEATH_API::kRespawnHere);
        MaskCheckbox("Last place slept", mask, TRICK_DEATH_API::kLastSleep);
        MaskCheckbox("Last external checkpoint", mask, TRICK_DEATH_API::kLastCheckpoint);
        MaskCheckbox("Reload current save", mask, TRICK_DEATH_API::kReloadSave);
        if (includeDisable) {
            MaskCheckbox("Disable Trick Death entirely", mask, TRICK_DEATH_API::kDisableTrickDeath);
        }
        ImGui::PopID();
    }

    RE::TESForm* ResolvePolicyArea(RE::PlayerCharacter* player) {
        if (debug.policyScope == 0) {
            return nullptr;
        }
        auto cell = player ? player->GetParentCell() : nullptr;
        if (debug.policyScope == 1) {
            return cell;
        }
        return cell ? cell->GetLocation() : nullptr;
    }

    bool HasSelectedArea(RE::PlayerCharacter* player) {
        return debug.policyScope == 0 || ResolvePolicyArea(player) != nullptr;
    }

    void RefreshMenuText() {
        Prisma::ApplyUISettings();
    }
}

void APIDebugMenu::Render() {
    auto* api = GetTrickDeathAPI(TRICK_DEATH_API::API_VERSION);
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!api) {
        ImGui::TextUnformatted("GetTrickDeathAPI(v1) returned null.");
        return;
    }
    if (!player) {
        ImGui::TextUnformatted("The player reference is unavailable.");
        return;
    }

    const auto available = api->GetAvailableRespawns();
    ImGui::Text("API version: %u", api->GetVersion());
    ImGui::Text("Debug owner: Player [%08X]", player->GetFormID());
    ImGui::Text(
        "HasLastSleep=%s | HasCheckpoint=%s | AvailableMask=0x%02X",
        api->HasLastSleep() ? "true" : "false",
        api->HasCheckpoint() ? "true" : "false",
        available);
    const auto currentSave = CurrentSaveManager::GetCurrentSaveName();
    ImGui::TextWrapped(
        "Reload Save target: %s",
        currentSave.empty() ? "unavailable (New Game or current save deleted)" : currentSave.c_str());
    ImGui::TextWrapped("Status: %s", debug.status.c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("External checkpoint", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("Checkpoint name", debug.checkpointName, std::size(debug.checkpointName));
        ImGui::TextUnformatted("Checkpoint-provided blocked mask:");
        ImGui::Indent();
        RenderActionMask("checkpoint-blocked-mask", debug.checkpointBlockedMask, false);
        ImGui::Unindent();
        ImGui::Text("Checkpoint draft blocked mask: 0x%02X", debug.checkpointBlockedMask);

        if (ImGui::Button("Set checkpoint at player")) {
            TRICK_DEATH_API::CheckpointRequest request;
            request.anchor = player;
            request.owner = player;
            request.name = debug.checkpointName;
            request.blockedRespawns = debug.checkpointBlockedMask;
            const bool result = api->SetCheckpoint(request);
            debug.status = result ?
                "SetCheckpoint succeeded; the external checkpoint was overwritten." :
                "SetCheckpoint failed. Check the log for details.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear checkpoint")) {
            const bool result = api->ClearCheckpoint(player);
            debug.status = result ?
                "ClearCheckpoint succeeded." :
                "ClearCheckpoint returned false (no checkpoint owned by the debug owner).";
        }
        ImGui::TextUnformatted("Last-sleep can be tested by sleeping normally; it is intentionally separate from this checkpoint.");
        if (ImGui::Button("Capture last-sleep at player (internal test helper)")) {
            CheckpointManager::CaptureAfterSleep();
            debug.status = api->HasLastSleep() ?
                "Last-sleep test destination captured. A normal sleep will overwrite it." :
                "Could not capture the last-sleep test destination.";
        }
    }

    if (ImGui::CollapsingHeader("Respawn policy", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* scopes[]{ "Global", "Current cell", "Current location" };
        ImGui::Combo("Scope", std::addressof(debug.policyScope), scopes, static_cast<int>(std::size(scopes)));
        auto* area = ResolvePolicyArea(player);
        if (debug.policyScope != 0) {
            ImGui::Text(
                "Resolved area: %s",
                area ? fmt::format("{:08X}", area->GetFormID()).c_str() : "unavailable");
        }
        ImGui::TextUnformatted("Blocked mask (deny wins):");
        ImGui::Indent();
        RenderActionMask("policy-blocked-mask", debug.policyBlockedMask, true);
        ImGui::Unindent();
        ImGui::Checkbox("Persist policy in co-save", std::addressof(debug.policyPersistent));

        const auto policySnapshot = RespawnPolicyManager::GetPolicies();
        const auto areaID = area ? area->GetFormID() : 0;
        std::uint32_t appliedPolicyMask = 0;
        bool appliedPolicyPersistent = false;
        bool hasAppliedPolicy = false;
        for (const auto& policy : policySnapshot) {
            if (policy.ownerFormID == player->GetFormID() && policy.areaFormID == areaID) {
                appliedPolicyMask = policy.blockedMask;
                appliedPolicyPersistent = policy.persistent;
                hasAppliedPolicy = true;
                break;
            }
        }
        ImGui::Text(
            "Draft mask: 0x%02X | Applied debug policy: %s0x%02X | persistent=%s",
            debug.policyBlockedMask,
            hasAppliedPolicy ? "" : "none / ",
            appliedPolicyMask,
            appliedPolicyPersistent ? "true" : "false");
        ImGui::TextUnformatted("Checkboxes edit the draft; use Set/replace to call the API.");

        if (ImGui::Button("Set/replace selected policy")) {
            if (!HasSelectedArea(player)) {
                debug.status = "The selected cell/location scope is unavailable.";
            } else {
                const bool result = api->SetRespawnPolicy(
                    player,
                    area,
                    debug.policyBlockedMask,
                    debug.policyPersistent);
                debug.status = result ?
                    fmt::format(
                        "SetRespawnPolicy succeeded: scope={}, blocked=0x{:X}, persistent={}.",
                        scopes[debug.policyScope],
                        debug.policyBlockedMask,
                        debug.policyPersistent) :
                    "SetRespawnPolicy failed. Check the selected area and log.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear selected policy")) {
            if (!HasSelectedArea(player)) {
                debug.status = "The selected cell/location scope is unavailable.";
            } else {
                const bool result = api->ClearRespawnPolicy(player, area);
                debug.status = result ?
                    "ClearRespawnPolicy succeeded." :
                    "ClearRespawnPolicy returned false (no matching debug policy).";
            }
        }
        if (ImGui::Button("Clear every policy owned by Debug")) {
            const auto removed = RespawnPolicyManager::ClearPolicies(player);
            debug.status = fmt::format("Removed {} Debug policy entries.", removed);
        }
        ImGui::Text(
            "Current result after policies: available=0x%02X, active policies=%zu",
            api->GetAvailableRespawns(),
            policySnapshot.size());
    }

    if (ImGui::CollapsingHeader("Text templates and variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Combo(
            "Text slot",
            std::addressof(debug.textSlot),
            TEXT_SLOTS.data(),
            static_cast<int>(TEXT_SLOTS.size()));
        ImGui::InputText("Template", debug.textTemplate, std::size(debug.textTemplate));
        ImGui::InputInt("Priority", std::addressof(debug.textPriority));
        ImGui::Checkbox("Persist text override in co-save", std::addressof(debug.textPersistent));
        if (ImGui::Button("Set/replace text override")) {
            const bool result = api->SetTextOverride(
                player,
                TEXT_SLOTS[debug.textSlot],
                debug.textTemplate,
                debug.textPriority,
                debug.textPersistent);
            debug.status = result ?
                fmt::format("SetTextOverride succeeded for '{}'.", TEXT_SLOTS[debug.textSlot]) :
                "SetTextOverride failed.";
            RefreshMenuText();
        }

        ImGui::InputText("Variable key", debug.variableKey, std::size(debug.variableKey));
        ImGui::InputText("Variable value", debug.variableValue, std::size(debug.variableValue));
        ImGui::Checkbox("Persist variable in co-save", std::addressof(debug.variablePersistent));
        if (ImGui::Button("Set/replace text variable")) {
            const bool result = api->SetTextVariable(
                player,
                debug.variableKey,
                debug.variableValue,
                debug.variablePersistent);
            debug.status = result ?
                fmt::format("SetTextVariable succeeded for '{}'.", debug.variableKey) :
                "SetTextVariable failed.";
            RefreshMenuText();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear all Debug text data")) {
            const auto removed = api->ClearTextOverrides(player);
            debug.status = fmt::format("Removed {} Debug text overrides/variables.", removed);
            RefreshMenuText();
        }

        const auto preview = TextManager::ResolveSlot(
            TEXT_SLOTS[debug.textSlot],
            "<localized default; no active override>");
        ImGui::TextWrapped("Resolved slot preview: %s", preview.c_str());
        ImGui::TextUnformatted("Supported variables include {$player.name}, {$killer.name}, {$death.location}, {$death.cause}, {$death.weapon}, {$death.magic}, {$death.count}, {$last_sleep.name}, and {$checkpoint.name}.");
    }

    if (ImGui::CollapsingHeader("Damage protection and death presentation", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto info = DeathManager::GetDebugInfo();
        ImGui::Text(
            "State=%s | protected=%s | remaining=%lld ms",
            info.state.c_str(),
            info.damageProtectionActive ? "true" : "false",
            static_cast<long long>(info.protectionRemainingMilliseconds));
        ImGui::Text(
            "Ghost captured=%s | originally ghost=%s | protected health=%.2f",
            info.ghostCaptured ? "true" : "false",
            info.playerWasGhost ? "true" : "false",
            info.protectedHealth);
        ImGui::Text(
            "Physical cause=%s | presentation cause=%s",
            info.physicalCause.c_str(),
            info.presentationCause.c_str());
        ImGui::TextWrapped("Background template: %s", info.backgroundTemplate.c_str());
        ImGui::TextWrapped("Resolved background: %s", DeathManager::GetBackgroundText().c_str());

        for (const auto* cause : { "generic", "fall", "sword", "magic" }) {
            ImGui::PushID(cause);
            if (ImGui::Button(fmt::format("Preview {}", cause).c_str())) {
                DeathManager::DebugSelectDeathTextCause(cause);
                debug.status = fmt::format("Selected '{}' death-message preview.", cause);
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();
    }

    if (ImGui::CollapsingHeader("DeathTracker graph statistic", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::int32_t graphValue = -1;
        const bool graphReadable = player->GetGraphVariableInt(
            DeathTrackerManager::GRAPH_VARIABLE,
            graphValue);
        ImGui::Text(
            "Co-save value=%d | graph readable=%s | graph value=%d",
            DeathTrackerManager::GetCount(),
            graphReadable ? "true" : "false",
            graphValue);
        if (ImGui::Button("Increment DeathTracker")) {
            DeathTrackerManager::RecordDeath(player);
            debug.status = "DeathTracker incremented and graph synchronization requested.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset DeathTracker")) {
            DeathTrackerManager::SetCountForDebug(0);
            debug.status = "DeathTracker reset to zero.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Resync DeathTracker graph")) {
            const bool result = DeathTrackerManager::SyncGraph(player);
            if (!result) {
                DeathTrackerManager::ScheduleGraphSync();
            }
            debug.status = result ?
                "DeathTracker graph synchronized." :
                "Immediate graph sync failed; retry scheduled.";
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(
        "Checkpoint changes emit lifecycle events immediately. RespawnSelected/RespawnCompleted are tested by triggering a real defeat and choosing an enabled option.");
}

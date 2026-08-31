#include "PlayerLootManager.h"

#include "RespawnCostManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

namespace {
    struct DeathLocation {
        bool valid{ false };
        RE::FormID cell{ 0 };
        RE::NiPoint3 position;
        RE::NiPoint3 angle;
    };

    struct TransferStack {
        RE::TESBoundObject* object{ nullptr };
        RE::ExtraDataList* extra{ nullptr };
        std::int32_t count{ 0 };
    };

    std::mutex stateLock;
    DeathLocation deathLocation;
    std::vector<RE::FormID> trackedContainers;
    std::atomic_bool pruneScheduled{ false };

    struct RayHit {
        RE::NiPoint3 position;
        RE::COL_LAYER layer{ RE::COL_LAYER::kUnidentified };
    };

    std::optional<RayHit> CastRay(
        RE::TESObjectCELL* cell,
        const RE::NiPoint3& from,
        const RE::NiPoint3& to)
    {
        auto* world = cell ? cell->GetbhkWorld() : nullptr;
        if (!world) {
            return std::nullopt;
        }

        const auto worldScale = RE::bhkWorld::GetWorldScale();
        RE::bhkPickData pick{};
        pick.rayInput.from = RE::hkVector4(from * worldScale);
        pick.rayInput.to = RE::hkVector4(to * worldScale);
        pick.rayInput.enableShapeCollectionFilter = false;
        RE::CFilter filter{};
        filter.SetCollisionLayer(RE::COL_LAYER::kLOS);
        pick.rayInput.filterInfo = filter;
        pick.rayOutput.Reset();
        world->PickObject(pick);
        if (!pick.rayOutput.HasHit() || !std::isfinite(pick.rayOutput.hitFraction)) {
            return std::nullopt;
        }

        return RayHit{
            from + (to - from) * std::clamp(pick.rayOutput.hitFraction, 0.0F, 1.0F),
            pick.rayOutput.rootCollidable->GetCollisionLayer()
        };
    }

    bool IsStableSupport(RE::COL_LAYER layer) {
        switch (layer) {
        case RE::COL_LAYER::kStatic:
        case RE::COL_LAYER::kProps:
        case RE::COL_LAYER::kWater:
        case RE::COL_LAYER::kTerrain:
        case RE::COL_LAYER::kGround:
        case RE::COL_LAYER::kStairHelper:
        case RE::COL_LAYER::kCollisionBox:
            return true;
        default:
            return false;
        }
    }

    bool HasClearRay(
        RE::TESObjectCELL* cell,
        const RE::NiPoint3& from,
        const RE::NiPoint3& to)
    {
        return !CastRay(cell, from, to).has_value();
    }

    std::optional<RE::NiPoint3> FindSafePlacement(
        RE::TESObjectCELL* cell,
        RE::TESObjectCONT* containerBase,
        const RE::NiPoint3& deathPosition)
    {
        if (!cell || !containerBase || !cell->GetbhkWorld()) {
            return std::nullopt;
        }

        const auto& bounds = containerBase->boundData;
        const auto minZ = static_cast<float>(bounds.boundMin.z);
        const auto maxZ = static_cast<float>(bounds.boundMax.z);
        const auto horizontalExtent = std::clamp(
            std::max({
                std::abs(static_cast<float>(bounds.boundMin.x)),
                std::abs(static_cast<float>(bounds.boundMax.x)),
                std::abs(static_cast<float>(bounds.boundMin.y)),
                std::abs(static_cast<float>(bounds.boundMax.y)) }),
            16.0F,
            128.0F);
        const auto objectHeight = std::clamp(maxZ - minZ, 16.0F, 256.0F);
        const std::array<RE::NiPoint3, 8> directions{
            RE::NiPoint3{ 1.0F, 0.0F, 0.0F },
            RE::NiPoint3{ -1.0F, 0.0F, 0.0F },
            RE::NiPoint3{ 0.0F, 1.0F, 0.0F },
            RE::NiPoint3{ 0.0F, -1.0F, 0.0F },
            RE::NiPoint3{ 0.7071068F, 0.7071068F, 0.0F },
            RE::NiPoint3{ -0.7071068F, 0.7071068F, 0.0F },
            RE::NiPoint3{ 0.7071068F, -0.7071068F, 0.0F },
            RE::NiPoint3{ -0.7071068F, -0.7071068F, 0.0F }
        };

        std::vector<RE::NiPoint3> candidates;
        candidates.reserve(17);
        for (const auto radius : {
                 std::max(horizontalExtent + 40.0F, 72.0F),
                 std::max(horizontalExtent + 112.0F, 144.0F) }) {
            for (const auto& direction : directions) {
                candidates.push_back(deathPosition + direction * radius);
            }
        }
        candidates.push_back(deathPosition);

        for (auto candidate : candidates) {
            auto rayStart = candidate;
            auto rayEnd = candidate;
            rayStart.z = deathPosition.z + 128.0F;
            rayEnd.z = deathPosition.z - 512.0F;
            const auto support = CastRay(cell, rayStart, rayEnd);
            if (!support || !IsStableSupport(support->layer)) {
                continue;
            }

            candidate.z = support->position.z - minZ + 4.0F;
            const auto clearanceBottom = RE::NiPoint3{
                candidate.x,
                candidate.y,
                support->position.z + 8.0F
            };
            const auto clearanceTop = RE::NiPoint3{
                candidate.x,
                candidate.y,
                support->position.z + objectHeight + 16.0F
            };
            if (!HasClearRay(cell, clearanceBottom, clearanceTop)) {
                continue;
            }

            const auto middleZ = support->position.z + objectHeight * 0.5F + 8.0F;
            const RE::NiPoint3 center{ candidate.x, candidate.y, middleZ };
            const auto clearanceRadius = horizontalExtent + 12.0F;
            bool sidesClear = true;
            for (const auto& direction : std::array<RE::NiPoint3, 4>{
                     directions[0], directions[1], directions[2], directions[3] }) {
                if (!HasClearRay(cell, center, center + direction * clearanceRadius)) {
                    sidesClear = false;
                    break;
                }
            }
            if (sidesClear) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    void DeleteContainer(RE::TESObjectREFR* container) {
        if (!container || container->IsMarkedForDeletion()) {
            return;
        }
        container->Disable();
        container->SetDelete(true);
    }

    bool IsTracked(RE::FormID formID) {
        std::scoped_lock lock(stateLock);
        return std::ranges::find(trackedContainers, formID) != trackedContainers.end();
    }

    void Track(RE::FormID formID) {
        if (formID == 0) {
            return;
        }
        std::scoped_lock lock(stateLock);
        if (std::ranges::find(trackedContainers, formID) == trackedContainers.end()) {
            trackedContainers.push_back(formID);
        }
    }

    RE::FormID GetProtectedResource(Respawn::Option option) {
        const auto* cost = RespawnCostManager::GetCost(option);
        return cost && cost->enabled && cost->keepInPlayerOnLootDrop ? cost->resource : 0;
    }

    template <class Callback>
    std::size_t VisitTransferStacks(
        RE::PlayerCharacter* player,
        Settings::LootDropMode mode,
        RE::FormID protectedResource,
        Callback&& callback)
    {
        std::size_t visited = 0;
        if (!player) {
            return visited;
        }

        auto inventory = player->GetInventory();
        for (auto& [object, inventoryData] : inventory) {
            const auto totalCount = std::max(inventoryData.first, 0);
            auto* entry = inventoryData.second.get();
            if (!object || !entry || totalCount <= 0 ||
                (protectedResource != 0 && object->GetFormID() == protectedResource)) {
                continue;
            }

            std::int32_t extraCount = 0;
            if (entry->extraLists) {
                for (auto* extra : *entry->extraLists) {
                    if (!extra) {
                        continue;
                    }
                    const auto stackCount = std::min(
                        std::max(extra->GetCount(), 1),
                        totalCount - extraCount);
                    extraCount += stackCount;
                    if (stackCount <= 0) {
                        continue;
                    }
                    if (extra->HasQuestObjectAlias()) {
                        continue;
                    }
                    if (mode == Settings::LootDropMode::kUnequippedOnly && extra->GetWorn()) {
                        continue;
                    }
                    callback(TransferStack{ object, extra, stackCount });
                    ++visited;
                }
            }

            const auto plainCount = std::max(totalCount - extraCount, 0);
            if (plainCount > 0) {
                callback(TransferStack{ object, nullptr, plainCount });
                ++visited;
            }
        }
        return visited;
    }

    void ClearTemplateInventory(RE::TESObjectREFR* container) {
        if (!container) {
            return;
        }
        container->InitInventoryIfRequired();
        if (auto* changes = container->GetInventoryChanges()) {
            changes->RemoveAllItems(container, nullptr, false, false, false);
        }
    }

    void SchedulePrune() {
        bool expected = false;
        if (!pruneScheduled.compare_exchange_strong(expected, true)) {
            return;
        }
        const auto tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            pruneScheduled.store(false);
            return;
        }
        tasks->AddTask([] {
            pruneScheduled.store(false);
            PlayerLootManager::PruneEmptyContainers();
        });
    }

    class ContainerChangedSink final :
        public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (event && event->oldContainer != 0 && IsTracked(event->oldContainer)) {
                SchedulePrune();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    ContainerChangedSink containerChangedSink;

    template <class T>
    bool ReadValue(SKSE::SerializationInterface* serialization, T& value) {
        return serialization && serialization->ReadRecordData(value) == sizeof(value);
    }
}

const Settings::PlayerLootDropSetting* PlayerLootManager::GetSetting(Respawn::Option option) {
    switch (option) {
    case Respawn::Option::Here:
        return &Settings::Gameplay.respawnHereLoot;
    case Respawn::Option::LastCheckpoint:
        return &Settings::Gameplay.lastCheckpointLoot;
    case Respawn::Option::LastSleep:
        return &Settings::Gameplay.lastSleepLoot;
    default:
        return nullptr;
    }
}

PlayerLootManager::Status PlayerLootManager::GetStatus(Respawn::Option option) {
    Status status;
    const auto* setting = GetSetting(option);
    if (!setting || !setting->enabled) {
        return status;
    }
    status.configured = true;
    status.containerValid =
        RE::TESForm::LookupByID<RE::TESObjectCONT>(setting->container) != nullptr;
    return status;
}

void PlayerLootManager::CaptureDeathLocation(RE::PlayerCharacter* player) {
    std::scoped_lock lock(stateLock);
    deathLocation = {};
    if (!player || !player->GetParentCell()) {
        return;
    }
    deathLocation.valid = true;
    deathLocation.cell = player->GetParentCell()->GetFormID();
    deathLocation.position = player->GetPosition();
    deathLocation.angle = player->GetAngle();
}

void PlayerLootManager::ClearDeathLocation() {
    std::scoped_lock lock(stateLock);
    deathLocation = {};
}

void PlayerLootManager::HandleNewDeath() {
    if (!Settings::Gameplay.destroyLootContainersOnDeath) {
        return;
    }

    std::vector<RE::FormID> containers;
    {
        std::scoped_lock lock(stateLock);
        containers.swap(trackedContainers);
    }
    for (const auto formID : containers) {
        if (auto* container = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID)) {
            DeleteContainer(container);
        }
    }
}

PlayerLootManager::PreparedDrop PlayerLootManager::Prepare(
    Respawn::Option option,
    RE::PlayerCharacter* player)
{
    PreparedDrop prepared;
    const auto* setting = GetSetting(option);
    if (!setting || !setting->enabled || !player) {
        return prepared;
    }
    auto* containerBase = RE::TESForm::LookupByID<RE::TESObjectCONT>(setting->container);
    if (!containerBase) {
        return prepared;
    }
    const auto transferableStacks = VisitTransferStacks(
        player,
        setting->mode,
        GetProtectedResource(option),
        [](const TransferStack&) {});
    if (transferableStacks == 0) {
        return prepared;
    }
    prepared.needed = true;

    DeathLocation location;
    {
        std::scoped_lock lock(stateLock);
        location = deathLocation;
    }
    if (!location.valid) {
        prepared.failed = true;
        return prepared;
    }

    auto* deathCell = player->GetParentCell();
    if (!deathCell || deathCell->GetFormID() != location.cell) {
        prepared.failed = true;
        return prepared;
    }
    auto placement = location.position;
    if (setting->safePlacement) {
        if (const auto safePlacement = FindSafePlacement(
                deathCell,
                containerBase,
                location.position)) {
            placement = *safePlacement;
        }
    }

    prepared.container = player->PlaceObjectAtMe(containerBase, true);
    if (!prepared.container) {
        prepared.failed = true;
        logger::error("Could not create the configured player-loot container.");
        return prepared;
    }

    const auto* containerCell = prepared.container->GetParentCell();
    if (!containerCell || containerCell->GetFormID() != location.cell) {
        logger::error(
            "Could not create the player-loot container in the captured death cell {:08X}.",
            location.cell);
        DeleteContainer(prepared.container.get());
        prepared.container.reset();
        prepared.failed = true;
        return prepared;
    }
    prepared.container->SetPosition(placement);
    prepared.container->SetAngle(location.angle);
    prepared.container->SetOwner(nullptr);
    prepared.container->SetActivationBlocked(false);
    ClearTemplateInventory(prepared.container.get());
    return prepared;
}

bool PlayerLootManager::Commit(
    PreparedDrop& prepared,
    Respawn::Option option,
    RE::PlayerCharacter* player)
{
    const auto* setting = GetSetting(option);
    if (!setting || !setting->enabled) {
        Cancel(prepared);
        return true;
    }
    if (!prepared.needed) {
        return true;
    }
    if (!prepared.container || !player) {
        return false;
    }

    VisitTransferStacks(
        player,
        setting->mode,
        GetProtectedResource(option),
        [&](const TransferStack& stack) {
            if (stack.object && stack.count > 0) {
                player->RemoveItem(
                    stack.object,
                    stack.count,
                    RE::ITEM_REMOVE_REASON::kRemove,
                    stack.extra,
                    prepared.container.get());
            }
        });

    if (prepared.container->GetInventoryCount(false) <= 0) {
        Cancel(prepared);
        return true;
    }
    Track(prepared.container->GetFormID());
    prepared.container.reset();
    prepared.needed = false;
    prepared.failed = false;
    return true;
}

void PlayerLootManager::Cancel(PreparedDrop& prepared) {
    if (prepared.container) {
        DeleteContainer(prepared.container.get());
        prepared.container.reset();
    }
    prepared.needed = false;
    prepared.failed = false;
}

void PlayerLootManager::RegisterEvents() {
    if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
        source->AddEventSink(&containerChangedSink);
    }
}

void PlayerLootManager::PruneEmptyContainers() {
    std::vector<RE::FormID> snapshot;
    {
        std::scoped_lock lock(stateLock);
        snapshot = trackedContainers;
    }

    std::vector<RE::FormID> removeIDs;
    std::vector<RE::NiPointer<RE::TESObjectREFR>> deleteRefs;
    for (const auto formID : snapshot) {
        auto* container = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
        if (!container || container->IsMarkedForDeletion()) {
            removeIDs.push_back(formID);
            continue;
        }
        if (container->GetInventoryCount(false) <= 0) {
            removeIDs.push_back(formID);
            deleteRefs.emplace_back(container);
        }
    }

    if (!removeIDs.empty()) {
        std::scoped_lock lock(stateLock);
        std::erase_if(trackedContainers, [&removeIDs](RE::FormID formID) {
            return std::ranges::find(removeIDs, formID) != removeIDs.end();
        });
    }
    for (const auto& container : deleteRefs) {
        DeleteContainer(container.get());
    }
}

std::size_t PlayerLootManager::GetTrackedContainerCount() {
    std::scoped_lock lock(stateLock);
    return trackedContainers.size();
}

void PlayerLootManager::Save(SKSE::SerializationInterface* serialization) {
    if (!serialization || !serialization->OpenRecord(RECORD_TYPE, RECORD_VERSION)) {
        return;
    }
    std::scoped_lock lock(stateLock);
    const auto count = static_cast<std::uint32_t>(trackedContainers.size());
    serialization->WriteRecordData(count);
    for (const auto formID : trackedContainers) {
        serialization->WriteRecordData(formID);
    }
}

bool PlayerLootManager::LoadRecord(
    SKSE::SerializationInterface* serialization,
    std::uint32_t type,
    std::uint32_t version,
    std::uint32_t)
{
    if (!serialization || type != RECORD_TYPE) {
        return false;
    }
    if (version != RECORD_VERSION) {
        logger::warn("Ignored unsupported player-loot record version {}.", version);
        return true;
    }

    std::uint32_t count = 0;
    if (!ReadValue(serialization, count) || count > 10000) {
        logger::warn("Ignored invalid player-loot container record.");
        return true;
    }
    std::vector<RE::FormID> loaded;
    loaded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        RE::FormID savedID = 0;
        RE::FormID resolvedID = 0;
        if (!ReadValue(serialization, savedID)) {
            logger::warn("Ignored truncated player-loot container record.");
            return true;
        }
        if (serialization->ResolveFormID(savedID, resolvedID) && resolvedID != 0) {
            if (std::ranges::find(loaded, resolvedID) == loaded.end()) {
                loaded.push_back(resolvedID);
            }
        }
    }
    std::scoped_lock lock(stateLock);
    trackedContainers = std::move(loaded);
    return true;
}

void PlayerLootManager::Revert() {
    std::scoped_lock lock(stateLock);
    deathLocation = {};
    trackedContainers.clear();
    pruneScheduled.store(false);
}

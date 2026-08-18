#include "StrictAltbotHolder.h"

#include "AiObjectContext.h"
#include "BudgetValues.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "StrictAltbotMgr.h"
#include "Timer.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr float MaxVendorTripDistance = 5000.0f;
constexpr uint32 HunterAmmoRestockThreshold = 200;
constexpr uint32 QuestItemCheckIntervalMs = 5000;
constexpr uint32 FailedAmmoTripRetryMs = 30000;

enum class ServiceTripKind
{
    Sell,
    Ammo,
    QuestItem
};

struct ServiceTrip
{
    ServiceTripKind kind = ServiceTripKind::Sell;
    WorldPosition destination{};
    ObjectGuid target{};
    uint32 questId = 0;
    uint32 itemId = 0;
    uint32 requiredCount = 0;
};

struct QuestItemVendorMatch
{
    Creature* npc = nullptr;
    uint32 questId = 0;
    uint32 itemId = 0;
    uint32 requiredCount = 0;
};

struct MissingQuestItem
{
    uint32 questId = 0;
    uint32 requiredCount = 0;
};

std::unordered_map<ObjectGuid, ServiceTrip> ServiceTrips;
std::unordered_set<ObjectGuid> ServiceTripsWithSuppressedGrind;
std::unordered_map<ObjectGuid, uint32> LastServiceChecks;
std::unordered_map<ObjectGuid, uint32> LastQuestItemChecks;
std::unordered_map<ObjectGuid, uint32> LastFailedAmmoTrips;

uint8 GetBagSpace(PlayerbotAI* botAI)
{
    if (Value<uint8>* value = botAI->GetAiObjectContext()->GetValue<uint8>("bag space"))
        return value->Get();

    return 0;
}

uint32 GetFreeMoney(PlayerbotAI* botAI, NeedMoneyFor purpose)
{
    if (Value<uint32>* value = botAI->GetAiObjectContext()->GetValue<uint32>(
            "free money for", static_cast<int32>(purpose)))
    {
        return value->Get();
    }

    return 0;
}

uint32 GetItemCountForUsage(PlayerbotAI* botAI, ItemUsage usage)
{
    std::string qualifier = "usage " + std::to_string(static_cast<uint32>(usage));
    if (Value<uint32>* value = botAI->GetAiObjectContext()->GetValue<uint32>("item count", qualifier))
        return value->Get();

    return 0;
}

bool HasSellableItems(PlayerbotAI* botAI)
{
    return GetItemCountForUsage(botAI, ITEM_USAGE_VENDOR) + GetItemCountForUsage(botAI, ITEM_USAGE_AH) > 0;
}

bool NeedsToSell(PlayerbotAI* botAI)
{
    return GetBagSpace(botAI) > 80 && HasSellableItems(botAI);
}

uint32 GetHunterAmmoSubClass(Player* bot)
{
    if (!bot || bot->getClass() != CLASS_HUNTER)
        return 0;

    Item* rangedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!rangedWeapon || !rangedWeapon->GetTemplate())
        return 0;

    switch (rangedWeapon->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        default:
            return 0;
    }
}

uint32 GetHunterAmmoCount(PlayerbotAI* botAI)
{
    if (Value<uint32>* value = botAI->GetAiObjectContext()->GetValue<uint32>("item count", "ammo"))
        return value->Get();

    return 0;
}

bool NeedsAmmo(Player* bot, PlayerbotAI* botAI)
{
    return GetHunterAmmoSubClass(bot) != 0 && GetHunterAmmoCount(botAI) < HunterAmmoRestockThreshold;
}

bool CanRetryAmmoTrip(ObjectGuid guid)
{
    auto failed = LastFailedAmmoTrips.find(guid);
    return failed == LastFailedAmmoTrips.end() ||
           GetMSTimeDiffToNow(failed->second) >= FailedAmmoTripRetryMs;
}

void ClearStaleGrindTarget(Player* player, PlayerbotAI* botAI)
{
    if (player->IsInCombat())
        return;

    auto* currentTarget = botAI->GetAiObjectContext()->GetValue<Unit*>("current target");
    if (!currentTarget || !currentTarget->Get())
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);
    currentTarget->Set(nullptr);
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
}

void UpdateServiceTripStrategy(Player* player, PlayerbotAI* botAI)
{
    ObjectGuid guid = player->GetGUID();
    bool suppressed = ServiceTripsWithSuppressedGrind.contains(guid);

    if (!ServiceTrips.contains(guid))
    {
        if (suppressed)
        {
            botAI->ChangeStrategy("+grind", BOT_STATE_NON_COMBAT);
            ServiceTripsWithSuppressedGrind.erase(guid);
            LOG_INFO("server.loading", "StrictAltbotGuild: {} restored grinding after service trip", player->GetName());
        }
        return;
    }

    if (!suppressed && botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
    {
        botAI->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
        ServiceTripsWithSuppressedGrind.insert(guid);
        LOG_INFO("server.loading", "StrictAltbotGuild: {} suspended grinding for service trip", player->GetName());
    }

    ClearStaleGrindTarget(player, botAI);
}

bool IsCompatibleHunterAmmo(Player* bot, ItemTemplate const* item)
{
    uint32 ammoSubClass = GetHunterAmmoSubClass(bot);
    return ammoSubClass && item && item->Class == ITEM_CLASS_PROJECTILE && item->SubClass == ammoSubClass &&
           bot->CanUseItem(item) == EQUIP_ERR_OK;
}

bool CanAffordRepair(PlayerbotAI* botAI)
{
    Value<uint32>* value = botAI->GetAiObjectContext()->GetValue<uint32>("repair cost");
    uint32 repairCost = value ? value->Get() : 0;
    return repairCost > 0 && repairCost <= GetFreeMoney(botAI, NeedMoneyFor::repair);
}

bool IsUsableServiceNpc(Player* bot, Creature* npc)
{
    return npc && npc->IsInWorld() && !npc->IsDuringRemoveFromWorld() && npc->IsAlive() &&
           npc->GetReactionTo(bot) > REP_UNFRIENDLY;
}

bool IsQuestItemStillNeeded(Player* bot, ServiceTrip const& trip)
{
    if (trip.kind != ServiceTripKind::QuestItem || !trip.questId || !trip.itemId || !trip.requiredCount)
        return false;

    if (bot->GetQuestStatus(trip.questId) != QUEST_STATUS_INCOMPLETE)
        return false;

    return bot->GetItemCount(trip.itemId, false) < trip.requiredCount;
}

std::unordered_map<uint32, MissingQuestItem> GetMissingQuestItems(Player* bot)
{
    std::unordered_map<uint32, MissingQuestItem> missingItems;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint32 objective = 0; objective < QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
        {
            uint32 itemId = quest->RequiredItemId[objective];
            uint32 requiredCount = quest->RequiredItemCount[objective];
            if (!itemId || !requiredCount || bot->GetItemCount(itemId, false) >= requiredCount)
                continue;

            auto [it, inserted] = missingItems.try_emplace(itemId, MissingQuestItem{questId, requiredCount});
            if (!inserted && requiredCount > it->second.requiredCount)
                it->second = MissingQuestItem{questId, requiredCount};
        }
    }

    return missingItems;
}

uint32 GetQuestItemCheckOffset(ObjectGuid guid)
{
    return static_cast<uint32>(
        (static_cast<uint64>(guid.GetCounter()) * 2654435761ULL) % QuestItemCheckIntervalMs);
}

std::optional<QuestItemVendorMatch> FindNearbyQuestItemVendor(
    Player* bot, PlayerbotAI* botAI, GuidVector const& targets)
{
    if (GetBagSpace(botAI) >= 100)
        return std::nullopt;

    std::unordered_map<uint32, MissingQuestItem> missingItems = GetMissingQuestItems(bot);
    if (missingItems.empty())
        return std::nullopt;

    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::anything);

    for (ObjectGuid const& guid : targets)
    {
        Creature* npc = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
        if (!IsUsableServiceNpc(bot, npc) || !npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
            continue;

        VendorItemData const* items = npc->GetVendorItems();
        if (!items)
            continue;

        float discount = bot->GetReputationPriceDiscount(npc);

        for (VendorItem const* vendorItem : items->m_items)
        {
            if (!vendorItem || vendorItem->ExtendedCost)
                continue;

            auto missing = missingItems.find(vendorItem->item);
            if (missing == missingItems.end())
                continue;

            if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
                continue;

            ItemTemplate const* item = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!item)
                continue;

            uint32 price = static_cast<uint32>(std::floor(item->BuyPrice * discount));
            if (freeMoney < price)
                continue;

            return QuestItemVendorMatch{
                npc, missing->second.questId, vendorItem->item, missing->second.requiredCount};
        }
    }

    return std::nullopt;
}

bool BuyRequiredQuestItem(Player* bot, PlayerbotAI* botAI, Creature* npc, ServiceTrip const& trip)
{
    if (!npc || trip.kind != ServiceTripKind::QuestItem || !IsQuestItemStillNeeded(bot, trip) ||
        GetBagSpace(botAI) >= 100)
    {
        return false;
    }

    VendorItemData const* items = npc->GetVendorItems();
    if (!items)
        return false;

    float discount = bot->GetReputationPriceDiscount(npc);
    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::anything);

    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = items->GetItem(slot);
        if (!vendorItem || vendorItem->ExtendedCost || vendorItem->item != trip.itemId)
            continue;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(trip.itemId);
        if (!item)
            continue;

        uint32 price = static_cast<uint32>(std::floor(item->BuyPrice * discount));
        if (freeMoney < price)
            return false;

        uint32 boughtCount = 0;
        while (bot->GetItemCount(trip.itemId, false) < trip.requiredCount && freeMoney >= price)
        {
            if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
                break;

            uint32 oldCount = bot->GetItemCount(trip.itemId, false);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, trip.itemId, 1, NULL_BAG, NULL_SLOT);
            uint32 newCount = bot->GetItemCount(trip.itemId, false);
            if (newCount <= oldCount)
                break;

            boughtCount += newCount - oldCount;
            freeMoney -= price;
        }

        if (boughtCount)
        {
            LOG_INFO("server.loading", "StrictAltbotGuild: {} bought {} quest item {} at {} for quest {}",
                bot->GetName(), boughtCount, trip.itemId, npc->GetName(), trip.questId);
            return true;
        }

        return false;
    }

    return false;
}

bool TrainerHasAffordableSpell(Player* bot, PlayerbotAI* botAI, Creature* npc)
{
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
    if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
        return false;

    // Do not accidentally pick a brand-new profession merely because the bot wandered past
    // an unrestricted profession trainer. Trainers for professions it already knows remain valid.
    if (trainer->GetTrainerType() == Trainer::Type::Tradeskill && !trainer->GetTrainerRequirement())
        return false;

    float discount = bot->GetReputationPriceDiscount(npc);
    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::spells);

    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
        if (!trainerSpell || !trainer->CanTeachSpell(bot, trainerSpell))
            continue;

        uint32 cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * discount));
        if (freeMoney >= cost)
            return true;
    }

    return false;
}

std::optional<NeedMoneyFor> GetPurchaseBudget(ItemUsage usage)
{
    switch (usage)
    {
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_BAD_EQUIP:
        case ITEM_USAGE_BROKEN_EQUIP:
            return NeedMoneyFor::gear;
        case ITEM_USAGE_AMMO:
            return NeedMoneyFor::ammo;
        case ITEM_USAGE_QUEST:
            return NeedMoneyFor::anything;
        case ITEM_USAGE_USE:
            return NeedMoneyFor::consumables;
        case ITEM_USAGE_SKILL:
            return NeedMoneyFor::tradeskill;
        default:
            return std::nullopt;
    }
}

bool VendorHasCompatibleAmmo(Player* bot, Creature* npc)
{
    if (!npc)
        return false;

    VendorItemData const* items = npc->GetVendorItems();
    if (!items)
        return false;

    for (VendorItem const* vendorItem : items->m_items)
    {
        if (!vendorItem || vendorItem->ExtendedCost)
            continue;

        if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
            continue;

        if (IsCompatibleHunterAmmo(bot, sObjectMgr->GetItemTemplate(vendorItem->item)))
            return true;
    }

    return false;
}

bool VendorHasAffordableAmmo(Player* bot, PlayerbotAI* botAI, Creature* npc)
{
    if (!npc || GetBagSpace(botAI) >= 100)
        return false;

    VendorItemData const* items = npc->GetVendorItems();
    if (!items)
        return false;

    float discount = bot->GetReputationPriceDiscount(npc);
    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::ammo);

    for (VendorItem const* vendorItem : items->m_items)
    {
        if (!vendorItem || vendorItem->ExtendedCost)
            continue;

        if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
            continue;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!IsCompatibleHunterAmmo(bot, item))
            continue;

        uint32 price = static_cast<uint32>(std::floor(item->BuyPrice * discount));
        if (freeMoney >= price)
            return true;
    }

    return false;
}

bool BuyHunterAmmo(Player* bot, PlayerbotAI* botAI, Creature* npc)
{
    if (!VendorHasAffordableAmmo(bot, botAI, npc))
        return false;

    VendorItemData const* items = npc->GetVendorItems();
    float discount = bot->GetReputationPriceDiscount(npc);
    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::ammo);

    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = items->GetItem(slot);
        if (!vendorItem || vendorItem->ExtendedCost)
            continue;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!IsCompatibleHunterAmmo(bot, item))
            continue;

        uint32 price = static_cast<uint32>(std::floor(item->BuyPrice * discount));
        if (freeMoney < price)
            continue;

        bool boughtAny = false;
        for (uint32 purchase = 0; purchase < 10 && freeMoney >= price; ++purchase)
        {
            if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
                break;

            uint32 oldCount = bot->GetItemCount(item->ItemId, false);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item->ItemId, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(item->ItemId, false) <= oldCount)
                break;

            boughtAny = true;
            freeMoney -= price;
        }

        if (boughtAny)
        {
            LOG_INFO("server.loading", "StrictAltbotGuild: {} restocked hunter ammo at {}",
                bot->GetName(), npc->GetName());
            return true;
        }
    }

    return false;
}

bool VendorHasUsefulAffordableItem(Player* bot, PlayerbotAI* botAI, Creature* npc)
{
    if (GetBagSpace(botAI) >= 100)
        return false;

    VendorItemData const* items = npc->GetVendorItems();
    if (!items)
        return false;

    float discount = bot->GetReputationPriceDiscount(npc);
    for (VendorItem const* vendorItem : items->m_items)
    {
        if (!vendorItem || vendorItem->ExtendedCost)
            continue;

        if (vendorItem->maxcount && !npc->GetVendorItemCurrentCount(vendorItem))
            continue;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!item)
            continue;

        Value<ItemUsage>* usageValue = botAI->GetAiObjectContext()->GetValue<ItemUsage>(
            "item usage", std::to_string(vendorItem->item));
        if (!usageValue)
            continue;

        std::optional<NeedMoneyFor> budget = GetPurchaseBudget(usageValue->Get());
        if (!budget)
            continue;

        uint32 price = static_cast<uint32>(std::floor(item->BuyPrice * discount));
        if (GetFreeMoney(botAI, *budget) >= price)
            return true;
    }

    return false;
}

template <typename Predicate>
Creature* FindNpc(Player* bot, GuidVector const& targets, Predicate&& predicate)
{
    for (ObjectGuid const& guid : targets)
    {
        Creature* npc = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
        if (IsUsableServiceNpc(bot, npc) && predicate(npc))
            return npc;
    }

    return nullptr;
}

Creature* ChooseServiceNpc(Player* bot, PlayerbotAI* botAI, GuidVector const& targets, bool needsSell, bool needsAmmo)
{
    if (needsAmmo)
    {
        if (Creature* npc = FindNpc(bot, targets, [bot](Creature* candidate)
            {
                return candidate->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && VendorHasCompatibleAmmo(bot, candidate);
            }))
        {
            return npc;
        }
    }

    if (needsSell)
    {
        // Every normal vendor can buy items, so the closest one is the right one.
        return FindNpc(bot, targets, [](Creature* npc) { return npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR); });
    }

    if (CanAffordRepair(botAI))
    {
        if (Creature* npc = FindNpc(
                bot, targets, [](Creature* candidate) { return candidate->HasNpcFlag(UNIT_NPC_FLAG_REPAIR); }))
        {
            return npc;
        }
    }

    if (Creature* npc = FindNpc(bot, targets, [bot, botAI](Creature* candidate)
        {
            return candidate->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) &&
                   TrainerHasAffordableSpell(bot, botAI, candidate);
        }))
    {
        return npc;
    }

    return FindNpc(bot, targets, [bot, botAI](Creature* candidate)
    {
        return candidate->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) &&
               VendorHasUsefulAffordableItem(bot, botAI, candidate);
    });
}

std::optional<WorldPosition> FindNearestVendor(Player* bot)
{
    float bestDistance = MaxVendorTripDistance;
    std::optional<WorldPosition> bestPosition;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != bot->GetMapId() || !(data.phaseMask & bot->GetPhaseMask()))
            continue;

        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creatureInfo)
            continue;

        uint32 npcFlags = data.npcflag ? data.npcflag : creatureInfo->npcflag;
        if (!(npcFlags & UNIT_NPC_FLAG_VENDOR))
            continue;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creatureInfo->faction);
        if (!faction || Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), faction) <= REP_UNFRIENDLY)
            continue;

        WorldPosition position(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        float distance = bot->GetExactDist(position);
        if (distance >= bestDistance)
            continue;

        bestDistance = distance;
        bestPosition = position;
    }

    return bestPosition;
}

std::optional<WorldPosition> FindNearestAmmoVendor(Player* bot, PlayerbotAI* botAI)
{
    uint32 freeMoney = GetFreeMoney(botAI, NeedMoneyFor::ammo);
    if (!freeMoney)
        return std::nullopt;

    float bestDistance = MaxVendorTripDistance;
    std::optional<WorldPosition> bestPosition;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != bot->GetMapId() || !(data.phaseMask & bot->GetPhaseMask()))
            continue;

        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creatureInfo)
            continue;

        uint32 npcFlags = data.npcflag ? data.npcflag : creatureInfo->npcflag;
        if (!(npcFlags & UNIT_NPC_FLAG_VENDOR))
            continue;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creatureInfo->faction);
        if (!faction || Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), faction) <= REP_UNFRIENDLY)
            continue;

        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(data.id);
        if (!items)
            continue;

        bool sellsAffordableAmmo = false;
        for (VendorItem const* vendorItem : items->m_items)
        {
            if (!vendorItem || vendorItem->ExtendedCost)
                continue;

            ItemTemplate const* item = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (IsCompatibleHunterAmmo(bot, item) && freeMoney >= item->BuyPrice)
            {
                sellsAffordableAmmo = true;
                break;
            }
        }

        if (!sellsAffordableAmmo)
            continue;

        WorldPosition position(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        float distance = bot->GetExactDist(position);
        if (distance >= bestDistance)
            continue;

        bestDistance = distance;
        bestPosition = position;
    }

    return bestPosition;
}
}

StrictAltbotHolder* StrictAltbotHolder::instance()
{
    static StrictAltbotHolder instance;
    return &instance;
}

void StrictAltbotHolder::RecordFirstLogin(Player* bot)
{
    CharacterDatabase.Execute(
        "UPDATE `strict_altbots` SET `first_login_at` = NOW(), `first_login_played_seconds` = {} "
        "WHERE `character_guid` = {} AND `enabled` = 1 AND `retired_at` IS NULL "
        "AND `first_login_at` IS NULL",
        bot->GetTotalPlayedTime(), bot->GetGUID().GetCounter());
}

void StrictAltbotHolder::RecordLevelUp(Player* bot, uint8 oldLevel)
{
    uint8 newLevel = bot->GetLevel();
    if (newLevel <= oldLevel)
        return;

    if (!sStrictAltbotMgr->IsStrictAltbot(bot->GetGUID().GetCounter()))
        return;

    uint32 characterGuid = bot->GetGUID().GetCounter();
    uint32 totalPlayed = bot->GetTotalPlayedTime();
    uint32 firstLoginPlayed = totalPlayed;

    QueryResult result = CharacterDatabase.Query(
        "SELECT `first_login_played_seconds` FROM `strict_altbots` "
        "WHERE `character_guid` = {} AND `enabled` = 1 AND `retired_at` IS NULL "
        "AND `first_login_at` IS NOT NULL",
        characterGuid);

    if (result)
        firstLoginPlayed = result->Fetch()[0].Get<uint32>();
    else
        RecordFirstLogin(bot);

    uint32 playedSinceFirstLogin = totalPlayed >= firstLoginPlayed
        ? totalPlayed - firstLoginPlayed
        : 0;

    for (uint16 level = uint16(oldLevel) + 1; level <= newLevel; ++level)
    {
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO `strict_altbot_levelups` "
            "(`character_guid`, `level`, `total_played_seconds`, `played_since_first_login_seconds`) "
            "VALUES ({}, {}, {}, {})",
            characterGuid, level, totalPlayed, playedSinceFirstLogin);
    }
}

void StrictAltbotHolder::RecordQuestDrop(Player* bot, uint32 questId)
{
    if (!bot || !questId || !sStrictAltbotMgr->IsStrictAltbot(bot->GetGUID().GetCounter()))
        return;

    CharacterDatabase.Execute(
        "INSERT IGNORE INTO `strict_altbot_quest_drops` (`character_guid`, `quest_id`) "
        "VALUES ({}, {})",
        bot->GetGUID().GetCounter(), questId);
}

void StrictAltbotHolder::Update(uint32 diff)
{
    if (_shuttingDown || !sStrictAltbotMgr->IsEnabled())
        return;

    UpdateSessions();

    _updateTimer += diff;
    if (_updateTimer < 2000)
        return;

    _updateTimer = 0;
    uint32 onlineCount = 0;

    QueryResult roster = CharacterDatabase.Query(
        "SELECT `character_guid`, `account_id` FROM `strict_altbots` "
        "WHERE `enabled` = 1 AND `always_online` = 1 AND `retired_at` IS NULL");

    if (roster)
    {
        do
        {
            Field* fields = roster->Fetch();
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            uint32 accountId = fields[1].Get<uint32>();

            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                if (player->GetSession()->IsBot())
                {
                    if (!GetPlayerBot(guid))
                    {
                        PlayerbotHolder::OnBotLogin(player);
                        EnableAutonomy(player);
                    }
                    ++onlineCount;
                }
                continue;
            }

            LoginBot(guid, accountId);
        } while (roster->NextRow());
    }

    for (auto callback = _loginCallbacks.begin(); callback != _loginCallbacks.end();)
    {
        ObjectGuid guid = callback->first;
        if (!sStrictAltbotMgr->IsStrictAltbot(guid.GetCounter()))
        {
            callback = _loginCallbacks.erase(callback);
            continue;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot)
        {
            ++callback;
            continue;
        }

        auto pending = std::move(callback->second);
        callback = _loginCallbacks.erase(callback);
        pending(bot);
    }

    if (onlineCount != _lastOnlineCount)
    {
        _lastOnlineCount = onlineCount;
        LOG_INFO("server.loading", "StrictAltbotGuild: {} strict Altbot(s) online", onlineCount);
    }
}

void StrictAltbotHolder::LoginBot(ObjectGuid guid, uint32 accountId)
{
    if (_loading.contains(guid))
        return;

    auto holder = std::make_shared<LoginQueryHolder>(accountId, guid);
    if (!holder->Initialize())
        return;

    _loading.insert(guid);
    sWorld->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder))
        .AfterComplete([](SQLQueryHolderBase const& queryHolder)
        {
            LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
            StrictAltbotHolder* strictHolder = sStrictAltbotHolder;
            strictHolder->_loading.erase(loginHolder.GetGuid());

            if (strictHolder->_shuttingDown || ObjectAccessor::FindConnectedPlayer(loginHolder.GetGuid()))
                return;

            QueryResult activeRoster = CharacterDatabase.Query(
                "SELECT 1 FROM `strict_altbots` "
                "WHERE `character_guid` = {} AND `enabled` = 1 AND `retired_at` IS NULL",
                loginHolder.GetGuid().GetCounter());
            if (!activeRoster)
                return;

            WorldSession* session = new WorldSession(
                loginHolder.GetAccountId(), "", 0, nullptr, SEC_PLAYER,
                EXPANSION_WRATH_OF_THE_LICH_KING, time(nullptr),
                sWorld->GetDefaultDbcLocale(), 0, false, false, 0, true);

            session->HandlePlayerLoginFromDB(loginHolder);
            if (!session->GetPlayer())
            {
                session->LogoutPlayer(true);
                delete session;
            }
        });
}

void StrictAltbotHolder::OnBotLoginInternal(Player* bot)
{
    if (!sStrictAltbotMgr->IsStrictAltbot(bot->GetGUID().GetCounter()))
    {
        LOG_WARN("server.loading", "StrictAltbotGuild: ignoring login hook for inactive bot {}", bot->GetName());
        return;
    }

    RecordFirstLogin(bot);

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        botAI->SetMaster(nullptr);
        botAI->SetCheat(BotCheatMask::none);
    }

    bot->SetTaxiCheater(false);

    if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
    {
        if (auto const* member = guild->GetMember(bot->GetGUID());
            member && guild->HasRankRight(bot, GR_RIGHT_GCHATSPEAK))
        {
            uint32 rights = guild->GetRankRights(member->GetRankId());
            rights &= ~(GR_RIGHT_GCHATSPEAK ^ GR_RIGHT_EMPTY);
            guild->HandleSetRankInfo(member->GetRankId(), rights);
            LOG_INFO("server.loading", "StrictAltbotGuild: muted guild rank {}", member->GetRankId());
        }
    }

    LOG_INFO("server.loading", "StrictAltbotGuild: {} logged in", bot->GetName());
}

void StrictAltbotHolder::QueueOnBotLogin(ObjectGuid guid, std::function<void(Player*)> callback)
{
    if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
    {
        callback(bot);
        return;
    }

    _loginCallbacks[guid] = std::move(callback);
}

void StrictAltbotHolder::EnableAutonomy(Player* bot)
{
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        botAI->ChangeStrategy("+new rpg,+grind,+lfg", BOT_STATE_NON_COMBAT);
        LOG_INFO("server.loading", "StrictAltbotGuild: {} autonomous strategies enabled", bot->GetName());
    }
}

void StrictAltbotHolder::UpdateRpgServices(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    ObjectGuid guid = bot->GetGUID();
    bool needsAmmo = NeedsAmmo(bot, botAI);
    bool needsSell = NeedsToSell(botAI);

    if (!needsAmmo)
        LastFailedAmmoTrips.erase(guid);

    if (auto trip = ServiceTrips.find(guid); trip != ServiceTrips.end())
    {
        bool stillNeeded = false;
        switch (trip->second.kind)
        {
            case ServiceTripKind::Ammo:
                stillNeeded = needsAmmo;
                break;
            case ServiceTripKind::Sell:
                stillNeeded = needsSell;
                break;
            case ServiceTripKind::QuestItem:
                // Ammo is urgent and wins over a nearby optional quest-item detour.
                stillNeeded = !needsAmmo && IsQuestItemStillNeeded(bot, trip->second);
                break;
        }

        if (!stillNeeded)
            ServiceTrips.erase(trip);
    }

    if (!ServiceTrips.contains(guid) && !needsAmmo && !needsSell && bot->IsAlive() && !bot->IsInCombat() &&
        !bot->IsInFlight() && !bot->IsBeingTeleported())
    {
        auto [checkIt, inserted] = LastQuestItemChecks.try_emplace(guid);
        if (inserted)
            checkIt->second = getMSTime() - GetQuestItemCheckOffset(guid);

        uint32& lastCheck = checkIt->second;
        if (GetMSTimeDiffToNow(lastCheck) >= QuestItemCheckIntervalMs)
        {
            lastCheck = getMSTime();

            Value<GuidVector>* targetsValue =
                botAI->GetAiObjectContext()->GetValue<GuidVector>("possible new rpg targets");
            GuidVector targets = targetsValue ? targetsValue->Get() : GuidVector{};

            if (std::optional<QuestItemVendorMatch> match = FindNearbyQuestItemVendor(bot, botAI, targets))
            {
                ServiceTrip trip;
                trip.kind = ServiceTripKind::QuestItem;
                trip.target = match->npc->GetGUID();
                trip.questId = match->questId;
                trip.itemId = match->itemId;
                trip.requiredCount = match->requiredCount;
                ServiceTrips[guid] = trip;

                botAI->rpgInfo.ChangeToWanderNpc();
                LOG_INFO("server.loading", "StrictAltbotGuild: {} noticed nearby vendor {} sells quest item {} for quest {}",
                    bot->GetName(), match->npc->GetName(), match->itemId, match->questId);
            }
        }
    }

    UpdateServiceTripStrategy(bot, botAI);

    NewRpgStatus status = botAI->rpgInfo.GetStatus();

    if (needsAmmo && status != RPG_WANDER_NPC)
    {
        if (bot->IsInCombat())
            return;

        auto trip = ServiceTrips.find(guid);
        if (trip != ServiceTrips.end())
        {
            if (status != RPG_GO_CAMP)
                botAI->rpgInfo.ChangeToGoCamp(trip->second.destination);
            return;
        }

        if (!CanRetryAmmoTrip(guid))
            return;

        if (std::optional<WorldPosition> vendorPosition = FindNearestAmmoVendor(bot, botAI))
        {
            ServiceTrips[guid] = ServiceTrip{ServiceTripKind::Ammo, *vendorPosition};
            botAI->rpgInfo.ChangeToGoCamp(*vendorPosition);
            LOG_INFO("server.loading", "StrictAltbotGuild: {} low on hunter ammo ({}), heading to an ammo vendor",
                bot->GetName(), GetHunterAmmoCount(botAI));
        }
        return;
    }

    if (status != RPG_WANDER_NPC)
        return;

    uint32& lastCheck = LastServiceChecks[guid];
    if (lastCheck && GetMSTimeDiffToNow(lastCheck) < 1000)
        return;
    lastCheck = getMSTime();

    auto* wander = std::get_if<NewRpgInfo::WanderNpc>(&botAI->rpgInfo.data);
    if (!wander)
        return;

    Value<GuidVector>* targetsValue = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible new rpg targets");
    GuidVector targets = targetsValue ? targetsValue->Get() : GuidVector{};

    auto trip = ServiceTrips.find(guid);
    Creature* serviceNpc = nullptr;
    if (trip != ServiceTrips.end() && trip->second.kind == ServiceTripKind::QuestItem)
    {
        serviceNpc = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, trip->second.target);
        if (!IsUsableServiceNpc(bot, serviceNpc) || !serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
            serviceNpc = nullptr;
    }
    else
    {
        serviceNpc = ChooseServiceNpc(bot, botAI, targets, needsSell, needsAmmo);
    }

    if (!serviceNpc)
    {
        if ((needsSell || needsAmmo) && trip == ServiceTrips.end())
        {
            std::optional<WorldPosition> vendorPosition;
            ServiceTripKind kind = ServiceTripKind::Sell;

            if (needsAmmo && CanRetryAmmoTrip(guid))
            {
                vendorPosition = FindNearestAmmoVendor(bot, botAI);
                kind = ServiceTripKind::Ammo;
            }
            else if (needsSell)
            {
                vendorPosition = FindNearestVendor(bot);
            }

            if (vendorPosition)
            {
                ServiceTrips[guid] = ServiceTrip{kind, *vendorPosition};
                botAI->rpgInfo.ChangeToGoCamp(*vendorPosition);
                return;
            }
        }

        if (trip != ServiceTrips.end())
        {
            if (trip->second.kind == ServiceTripKind::Ammo)
            {
                LastFailedAmmoTrips[guid] = getMSTime();
                LOG_INFO("server.loading",
                    "StrictAltbotGuild: {} could not find a usable ammo vendor on arrival; retrying in {} seconds",
                    bot->GetName(), FailedAmmoTripRetryMs / 1000);
            }
            ServiceTrips.erase(trip);
        }

        botAI->rpgInfo.ChangeToIdle();
        return;
    }

    if (wander->npcOrGo != serviceNpc->GetGUID())
    {
        wander->npcOrGo = serviceNpc->GetGUID();
        wander->lastReach = 0;
    }

    if (!serviceNpc->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
        return;

    bot->SetTarget(serviceNpc->GetGUID());
    bot->SetFacingToObject(serviceNpc);

    if (trip != ServiceTrips.end() && trip->second.kind == ServiceTripKind::QuestItem)
    {
        BuyRequiredQuestItem(bot, botAI, serviceNpc, trip->second);
        ServiceTrips.erase(guid);
        botAI->rpgInfo.ChangeToIdle();
        return;
    }

    if ((needsSell || (needsAmmo && HasSellableItems(botAI))) && serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        botAI->DoSpecificAction("sell", Event("strict altbot rpg", "vendor"), true);

    bool boughtAmmo = false;
    if (needsAmmo && serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
    {
        boughtAmmo = BuyHunterAmmo(bot, botAI, serviceNpc);
        if (boughtAmmo)
            LastFailedAmmoTrips.erase(guid);
    }

    if (trip != ServiceTrips.end() && trip->second.kind == ServiceTripKind::Ammo && needsAmmo && !boughtAmmo)
    {
        LastFailedAmmoTrips[guid] = getMSTime();
        LOG_INFO("server.loading",
            "StrictAltbotGuild: {} could not restock hunter ammo at {} ({} ammo, {} copper available); retrying in {} seconds",
            bot->GetName(), serviceNpc->GetName(), GetHunterAmmoCount(botAI),
            GetFreeMoney(botAI, NeedMoneyFor::ammo), FailedAmmoTripRetryMs / 1000);
        ServiceTrips.erase(guid);
        botAI->rpgInfo.ChangeToIdle();
        return;
    }

    if (serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && CanAffordRepair(botAI))
        botAI->DoSpecificAction("repair", Event("strict altbot rpg"), true);

    if (serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) && TrainerHasAffordableSpell(bot, botAI, serviceNpc))
        botAI->DoSpecificAction("trainer", Event("strict altbot rpg", "learn"), true);

    if (serviceNpc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && VendorHasUsefulAffordableItem(bot, botAI, serviceNpc))
        botAI->DoSpecificAction("buy", Event("strict altbot rpg", "vendor"), true);

    ServiceTrips.erase(guid);
    botAI->rpgInfo.ChangeToIdle();
}

void StrictAltbotHolder::RemoveBot(ObjectGuid guid)
{
    ServiceTrips.erase(guid);
    ServiceTripsWithSuppressedGrind.erase(guid);
    LastServiceChecks.erase(guid);
    LastQuestItemChecks.erase(guid);
    LastFailedAmmoTrips.erase(guid);
    _loginCallbacks.erase(guid);
    RemoveFromPlayerbotsMap(guid);
}

void StrictAltbotHolder::Shutdown()
{
    _shuttingDown = true;
    _loading.clear();
    _loginCallbacks.clear();
    ServiceTrips.clear();
    ServiceTripsWithSuppressedGrind.clear();
    LastServiceChecks.clear();
    LastQuestItemChecks.clear();
    LastFailedAmmoTrips.clear();
    LogoutAllBots();
}

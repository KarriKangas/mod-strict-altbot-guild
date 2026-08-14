#include "AccountMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StrictAltbotMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "StrictAltbotHolder.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 StrictBotCount = 40;
constexpr char AquariumPrefix[] = "ALTBO_JSON ";
constexpr uint32 MaxStrictGuildSize = 1000;

struct StrictBotCandidate
{
    ObjectGuid guid;
    uint32 accountId = 0;
    uint32 currentGuildId = 0;
};

std::optional<TeamId> ParseFaction(std::string faction)
{
    std::transform(faction.begin(), faction.end(), faction.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    if (faction == "a" || faction == "alliance")
        return TEAM_ALLIANCE;
    if (faction == "h" || faction == "horde")
        return TEAM_HORDE;

    return std::nullopt;
}

bool IsValidGuildName(std::string const& name)
{
    if (name.empty() || name.size() > 24)
        return false;

    return !std::isspace(static_cast<unsigned char>(name.front())) &&
           !std::isspace(static_cast<unsigned char>(name.back()));
}

std::string EscapeJson(std::string_view value)
{
    std::ostringstream out;
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (character < 0x20)
                {
                    out << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                        << static_cast<uint32>(character) << std::dec;
                }
                else
                    out << character;
                break;
        }
    }

    return out.str();
}

void SendAquariumJson(ChatHandler* handler, std::string const& json)
{
    handler->SendSysMessage(std::string(AquariumPrefix) + json);
}

void AppendStringArray(std::ostringstream& out, std::vector<std::string> const& values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index)
            out << ',';
        out << '"' << EscapeJson(values[index]) << '"';
    }
    out << ']';
}

std::string GetItemIcon(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return {};

    ItemDisplayInfoEntry const* display = sItemDisplayInfoStore.LookupEntry(itemTemplate->DisplayInfoID);
    return display && display->inventoryIcon ? display->inventoryIcon : "";
}

void AppendArmorSubclass(std::ostringstream& out, ItemTemplate const* itemTemplate)
{
    if (itemTemplate && itemTemplate->Class == ITEM_CLASS_ARMOR)
        out << ",\"armorSubclass\":" << itemTemplate->SubClass;
}

void AppendItem(std::ostringstream& out, Item const* item)
{
    if (!item || !item->GetTemplate())
    {
        out << "null";
        return;
    }

    ItemTemplate const* itemTemplate = item->GetTemplate();
    out << "{\"id\":" << item->GetEntry()
        << ",\"name\":\"" << EscapeJson(itemTemplate->Name1) << '"'
        << ",\"count\":" << item->GetCount()
        << ",\"quality\":" << itemTemplate->Quality
        << ",\"icon\":\"" << EscapeJson(GetItemIcon(itemTemplate)) << '"'
        << ",\"itemLevel\":" << itemTemplate->ItemLevel
        << ",\"requiredLevel\":" << itemTemplate->RequiredLevel
        << ",\"armor\":" << itemTemplate->Armor;
    AppendArmorSubclass(out, itemTemplate);
    out << ",\"damageMin\":" << itemTemplate->Damage[0].DamageMin
        << ",\"damageMax\":" << itemTemplate->Damage[0].DamageMax
        << ",\"speed\":" << itemTemplate->Delay
        << ",\"durability\":" << item->GetUInt32Value(ITEM_FIELD_DURABILITY)
        << ",\"maxDurability\":" << item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)
        << ",\"vendorValue\":" << itemTemplate->SellPrice
        << ",\"soulbound\":" << (item->IsSoulBound() ? "true" : "false")
        << ",\"stats\":[";

    for (uint32 index = 0; index < itemTemplate->StatsCount; ++index)
    {
        if (index)
            out << ',';
        out << "{\"type\":" << itemTemplate->ItemStat[index].ItemStatType
            << ",\"value\":" << itemTemplate->ItemStat[index].ItemStatValue << '}';
    }

    out << "],\"enchants\":[";
    bool firstEnchant = true;
    for (EnchantmentSlot slot : { PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT })
    {
        uint32 enchantId = item->GetEnchantmentId(slot);
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchantId)
            continue;
        if (!firstEnchant)
            out << ',';
        firstEnchant = false;
        out << "{\"id\":" << enchantId << ",\"name\":\""
            << EscapeJson(enchant && enchant->description[LOCALE_enUS] ? enchant->description[LOCALE_enUS] : "Enchanted") << "\"}";
    }

    out << "],\"gems\":[";
    bool firstGem = true;
    for (uint32 slot = SOCK_ENCHANTMENT_SLOT; slot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS; ++slot)
    {
        uint32 enchantId = item->GetEnchantmentId(EnchantmentSlot(slot));
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant || !enchant->GemID)
            continue;
        ItemTemplate const* gem = sObjectMgr->GetItemTemplate(enchant->GemID);
        if (!firstGem)
            out << ',';
        firstGem = false;
        out << "{\"id\":" << enchant->GemID << ",\"name\":\""
            << EscapeJson(gem ? gem->Name1 : "Socketed gem") << "\"}";
    }
    out << "]}";
}

void AppendItemTemplate(std::ostringstream& out, ItemTemplate const* itemTemplate, uint32 count)
{
    if (!itemTemplate)
    {
        out << "null";
        return;
    }

    out << "{\"id\":" << itemTemplate->ItemId
        << ",\"name\":\"" << EscapeJson(itemTemplate->Name1) << '"'
        << ",\"count\":" << count
        << ",\"quality\":" << itemTemplate->Quality
        << ",\"icon\":\"" << EscapeJson(GetItemIcon(itemTemplate)) << '"'
        << ",\"itemLevel\":" << itemTemplate->ItemLevel
        << ",\"requiredLevel\":" << itemTemplate->RequiredLevel
        << ",\"armor\":" << itemTemplate->Armor;
    AppendArmorSubclass(out, itemTemplate);
    out << ",\"damageMin\":" << itemTemplate->Damage[0].DamageMin
        << ",\"damageMax\":" << itemTemplate->Damage[0].DamageMax
        << ",\"speed\":" << itemTemplate->Delay
        << ",\"durability\":0"
        << ",\"maxDurability\":0"
        << ",\"vendorValue\":" << itemTemplate->SellPrice
        << ",\"soulbound\":false"
        << ",\"stats\":[";

    for (uint32 index = 0; index < itemTemplate->StatsCount; ++index)
    {
        if (index)
            out << ',';
        out << "{\"type\":" << itemTemplate->ItemStat[index].ItemStatType
            << ",\"value\":" << itemTemplate->ItemStat[index].ItemStatValue << '}';
    }

    out << "],\"enchants\":[],\"gems\":[]}";
}

void GetBagUsage(Player* player, uint32& used, uint32& total)
{
    used = 0;
    total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        if (Bag const* bag = player->GetBagByPos(bagSlot))
        {
            total += bag->GetBagSize();
            used += bag->GetBagSize() - bag->GetFreeSlots();
        }
    }
}

uint32 GetRosterItemLevel(Player* player)
{
    uint32 totalItemLevel = 0;
    uint32 equippedItems = 0;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemTemplate const* itemTemplate = item ? item->GetTemplate() : nullptr;
        if (!itemTemplate || !itemTemplate->ItemLevel)
            continue;

        totalItemLevel += itemTemplate->ItemLevel;
        ++equippedItems;
    }

    if (!equippedItems)
        return 0;

    // Match the frontend's Math.round(sum / populated equipment slots).
    return (totalItemLevel + equippedItems / 2) / equippedItems;
}

std::string GetAreaName(Player* player)
{
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId()))
        return area->area_name[LOCALE_enUS];

    return {};
}

char const* GetFactionName(TeamId team)
{
    if (team == TEAM_HORDE)
        return "horde";
    if (team == TEAM_ALLIANCE)
        return "alliance";
    return "";
}

void AppendStrategies(std::ostringstream& out, PlayerbotAI* botAI)
{
    out << "{\"combat\":";
    AppendStringArray(out, botAI->GetStrategies(BOT_STATE_COMBAT));
    out << ",\"nonCombat\":";
    AppendStringArray(out, botAI->GetStrategies(BOT_STATE_NON_COMBAT));
    out << ",\"dead\":";
    AppendStringArray(out, botAI->GetStrategies(BOT_STATE_DEAD));
    out << '}';
}

void AppendObjectives(std::ostringstream& out, Quest const* quest, QuestStatusData const* status)
{
    out << '[';
    bool first = true;
    for (uint8 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
    {
        if (!quest->RequiredNpcOrGo[index] || !quest->RequiredNpcOrGoCount[index])
            continue;

        if (!first)
            out << ',';
        first = false;
        int32 rawId = quest->RequiredNpcOrGo[index];
        uint32 id = rawId < 0 ? static_cast<uint32>(-rawId) : static_cast<uint32>(rawId);
        out << "{\"kind\":\"" << (rawId < 0 ? "object" : "creature") << "\",\"id\":" << id
            << ",\"current\":" << (status ? status->CreatureOrGOCount[index] : 0)
            << ",\"required\":" << quest->RequiredNpcOrGoCount[index];

        if (rawId < 0)
        {
            GameObjectTemplate const* gameObject = sObjectMgr->GetGameObjectTemplate(id);
            if (gameObject)
                out << ",\"name\":\"" << EscapeJson(gameObject->name) << '"';
        }
        else
        {
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(id);
            if (creature)
                out << ",\"name\":\"" << EscapeJson(creature->Name) << '"';
        }

        out << '}';
    }

    for (uint8 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
    {
        if (!quest->RequiredItemId[index] || !quest->RequiredItemCount[index])
            continue;

        if (!first)
            out << ',';
        first = false;
        out << "{\"kind\":\"item\",\"id\":" << quest->RequiredItemId[index]
            << ",\"current\":" << (status ? status->ItemCount[index] : 0)
            << ",\"required\":" << quest->RequiredItemCount[index] << ",\"item\":";
        AppendItemTemplate(out, sObjectMgr->GetItemTemplate(quest->RequiredItemId[index]), 1);
        out << '}';
    }

    if (quest->GetPlayersSlain())
    {
        if (!first)
            out << ',';
        out << "{\"kind\":\"player\",\"id\":0,\"current\":"
            << (status ? status->PlayerCount : 0) << ",\"required\":" << quest->GetPlayersSlain() << '}';
    }
    out << ']';
}

void AppendQuestRewards(std::ostringstream& out, Quest const* quest)
{
    out << "{\"guaranteed\":[";
    bool first = true;
    for (uint32 index = 0; index < quest->GetRewItemsCount(); ++index)
    {
        uint32 itemId = quest->RewardItemId[index];
        ItemTemplate const* itemTemplate = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!itemTemplate)
            continue;
        if (!first)
            out << ',';
        first = false;
        AppendItemTemplate(out, itemTemplate, quest->RewardItemIdCount[index]);
    }

    out << "],\"choices\":[";
    first = true;
    for (uint32 index = 0; index < quest->GetRewChoiceItemsCount(); ++index)
    {
        uint32 itemId = quest->RewardChoiceItemId[index];
        ItemTemplate const* itemTemplate = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!itemTemplate)
            continue;
        if (!first)
            out << ',';
        first = false;
        AppendItemTemplate(out, itemTemplate, quest->RewardChoiceItemCount[index]);
    }
    out << "]}";
}

std::string BuildRosterJson()
{
    std::vector<uint32> roster(sStrictAltbotMgr->GetRoster().begin(), sStrictAltbotMgr->GetRoster().end());
    std::sort(roster.begin(), roster.end(), [](uint32 left, uint32 right)
    {
        CharacterCacheEntry const* leftEntry = sCharacterCache->GetCharacterCacheByGuid(
            ObjectGuid::Create<HighGuid::Player>(left));
        CharacterCacheEntry const* rightEntry = sCharacterCache->GetCharacterCacheByGuid(
            ObjectGuid::Create<HighGuid::Player>(right));
        if (!leftEntry || !rightEntry)
            return left < right;
        return leftEntry->Name < rightEntry->Name;
    });

    std::ostringstream out;
    out << "{\"version\":1,\"bots\":[";
    for (std::size_t index = 0; index < roster.size(); ++index)
    {
        if (index)
            out << ',';

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(roster[index]);
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        uint8 race = player ? player->getRace() : cache ? cache->Race : 0;
        TeamId team = player ? player->GetTeamId() : Player::TeamIdForRace(race);

        out << "{\"guid\":" << roster[index]
            << ",\"name\":\"" << EscapeJson(player ? player->GetName() : cache ? cache->Name : "Unknown") << '"'
            << ",\"level\":" << static_cast<uint32>(player ? player->GetLevel() : cache ? cache->Level : 0)
            << ",\"classId\":" << static_cast<uint32>(player ? player->getClass() : cache ? cache->Class : 0)
            << ",\"raceId\":" << static_cast<uint32>(race)
            << ",\"faction\":\"" << GetFactionName(team) << '"'
            << ",\"itemLevel\":" << (player ? GetRosterItemLevel(player) : 0)
            << ",\"online\":" << (player ? "true" : "false");

        if (player)
        {
            uint32 bagUsed = 0;
            uint32 bagTotal = 0;
            GetBagUsage(player, bagUsed, bagTotal);
            out << ",\"area\":\"" << EscapeJson(GetAreaName(player)) << '"'
                << ",\"gold\":" << player->GetMoney()
                << ",\"bagUsed\":" << bagUsed
                << ",\"bagTotal\":" << bagTotal
                << ",\"state\":\""
                << EscapeJson(botAI ? botAI->HandleRemoteCommand("state") : "unknown") << '"'
                << ",\"action\":\""
                << EscapeJson(botAI ? botAI->HandleRemoteCommand("action") : "") << '"';
        }
        else
            out << ",\"area\":\"\",\"gold\":0,\"bagUsed\":0,\"bagTotal\":0,\"state\":\"offline\",\"action\":\"\"";

        out << '}';
    }
    out << "]}";
    return out.str();
}

std::string BuildSnapshotJson(uint32 lowGuid)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
    if (!sStrictAltbotMgr->IsStrictAltbot(lowGuid))
        return "{\"error\":\"not_strict_altbot\"}";

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
        return "{\"error\":\"offline\",\"guid\":" + std::to_string(lowGuid) + '}';

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return "{\"error\":\"no_playerbot_ai\",\"guid\":" + std::to_string(lowGuid) + '}';

    uint32 bagUsed = 0;
    uint32 bagTotal = 0;
    GetBagUsage(player, bagUsed, bagTotal);
    Powers powerType = player->getPowerType();
    uint32 power = player->GetPower(powerType);
    uint32 maxPower = player->GetMaxPower(powerType);
    uint32 xp = player->GetUInt32Value(PLAYER_XP);
    uint32 nextXp = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);

    std::ostringstream out;
    out << "{\"version\":1,\"guid\":" << lowGuid
        << ",\"name\":\"" << EscapeJson(player->GetName()) << '"'
        << ",\"level\":" << static_cast<uint32>(player->GetLevel())
        << ",\"classId\":" << static_cast<uint32>(player->getClass())
        << ",\"raceId\":" << static_cast<uint32>(player->getRace())
        << ",\"gold\":" << player->GetMoney()
        << ",\"health\":{\"current\":" << player->GetHealth() << ",\"max\":" << player->GetMaxHealth() << '}'
        << ",\"power\":{\"type\":" << static_cast<uint32>(powerType) << ",\"current\":" << power
        << ",\"max\":" << maxPower << '}'
        << ",\"xp\":{\"current\":" << xp << ",\"next\":" << nextXp << '}'
        << ",\"position\":{\"map\":" << player->GetMapId() << ",\"zone\":" << player->GetZoneId()
        << ",\"area\":" << player->GetAreaId() << ",\"areaName\":\"" << EscapeJson(GetAreaName(player))
        << "\",\"x\":" << std::fixed << std::setprecision(2) << player->GetPositionX()
        << ",\"y\":" << player->GetPositionY() << ",\"z\":" << player->GetPositionZ() << '}'
        << ",\"bagsUsed\":" << bagUsed << ",\"bagsTotal\":" << bagTotal
        << ",\"ai\":{\"state\":\"" << EscapeJson(botAI->HandleRemoteCommand("state"))
        << "\",\"action\":\"" << EscapeJson(botAI->HandleRemoteCommand("action"))
        << "\",\"target\":\"" << EscapeJson(botAI->HandleRemoteCommand("target"))
        << "\",\"travel\":\"" << EscapeJson(botAI->HandleRemoteCommand("travel"))
        << "\",\"rpg\":\"" << EscapeJson(botAI->rpgInfo.ToString()) << "\",\"strategies\":";
    AppendStrategies(out, botAI);
    out << "},\"equipment\":[";

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot != EQUIPMENT_SLOT_START)
            out << ',';
        out << "{\"slot\":" << static_cast<uint32>(slot) << ",\"item\":";
        AppendItem(out, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        out << '}';
    }

    out << "],\"bags\":[{\"slot\":0,\"name\":\"Backpack\",\"size\":"
        << (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START) << ",\"items\":[";
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (slot != INVENTORY_SLOT_ITEM_START)
            out << ',';
        AppendItem(out, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    }
    out << "]}";

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = player->GetBagByPos(bagSlot);
        out << ",{\"slot\":" << static_cast<uint32>(bagSlot) << ",\"name\":\"";
        if (bag && bag->GetTemplate())
            out << EscapeJson(bag->GetTemplate()->Name1);
        else
            out << "Empty bag slot";
        out << "\",\"size\":" << (bag ? bag->GetBagSize() : 0) << ",\"items\":[";
        if (bag)
        {
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                if (slot)
                    out << ',';
                AppendItem(out, bag->GetItemByPos(slot));
            }
        }
        out << "]}";
    }

    out << "],\"quests\":[";
    bool firstQuest = true;
    QuestStatusMap const& statuses = player->getQuestStatusMap();
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;

        auto statusIt = statuses.find(questId);
        QuestStatusData const* status = statusIt != statuses.end() ? &statusIt->second : nullptr;
        if (!firstQuest)
            out << ',';
        firstQuest = false;
        out << "{\"id\":" << questId << ",\"title\":\"" << EscapeJson(quest->GetTitle())
            << "\",\"status\":" << static_cast<uint32>(player->GetQuestStatus(questId))
            << ",\"timerMs\":" << (status ? status->Timer : 0) << ",\"objectives\":";
        AppendObjectives(out, quest, status);
        out << ",\"rewards\":";
        AppendQuestRewards(out, quest);
        out << '}';
    }
    out << "]}";
    return out.str();
}

std::string BuildHistoryJson(uint32 lowGuid)
{
    if (!sStrictAltbotMgr->IsStrictAltbot(lowGuid))
        return "{\"error\":\"not_strict_altbot\"}";

    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(lowGuid));
    if (!player)
        return "{\"error\":\"offline\",\"guid\":" + std::to_string(lowGuid) + '}';

    std::vector<uint32> rewarded(player->getRewardedQuests().begin(), player->getRewardedQuests().end());
    std::sort(rewarded.begin(), rewarded.end());

    std::ostringstream out;
    out << "{\"version\":1,\"guid\":" << lowGuid << ",\"completed\":[";
    for (std::size_t index = 0; index < rewarded.size(); ++index)
    {
        if (index)
            out << ',';
        Quest const* quest = sObjectMgr->GetQuestTemplate(rewarded[index]);
        out << "{\"id\":" << rewarded[index] << ",\"title\":\""
            << EscapeJson(quest ? quest->GetTitle() : "Unknown quest") << "\"}";
    }
    out << "]}";
    return out.str();
}

std::string BuildBenchmarksJson()
{
    QueryResult bots = CharacterDatabase.Query(
        "SELECT sa.`character_guid`, c.`name`, c.`race`, c.`class`, c.`level`, "
        "UNIX_TIMESTAMP(sa.`first_login_at`), COALESCE(UNIX_TIMESTAMP(sa.`retired_at`), 0) "
        "FROM `strict_altbots` sa "
        "INNER JOIN `characters` c ON c.`guid` = sa.`character_guid` "
        "WHERE sa.`first_login_at` IS NOT NULL "
        "ORDER BY sa.`first_login_at`, sa.`character_guid`");

    std::ostringstream out;
    out << "{\"version\":1,\"bots\":[";
    bool first = true;
    if (bots)
    {
        do
        {
            Field* fields = bots->Fetch();
            if (!first)
                out << ',';
            first = false;
            out << "{\"guid\":" << fields[0].Get<uint32>()
                << ",\"name\":\"" << EscapeJson(fields[1].Get<std::string>()) << '"'
                << ",\"raceId\":" << static_cast<uint32>(fields[2].Get<uint8>())
                << ",\"classId\":" << static_cast<uint32>(fields[3].Get<uint8>())
                << ",\"level\":" << static_cast<uint32>(fields[4].Get<uint8>())
                << ",\"firstLoginAt\":" << fields[5].Get<uint32>()
                << ",\"retiredAt\":" << fields[6].Get<uint32>() << '}';
        } while (bots->NextRow());
    }

    out << "],\"levelups\":[";
    QueryResult levelups = CharacterDatabase.Query(
        "SELECT `character_guid`, `level`, `played_since_first_login_seconds` "
        "FROM `strict_altbot_levelups` ORDER BY `character_guid`, `level`");
    first = true;
    if (levelups)
    {
        do
        {
            Field* fields = levelups->Fetch();
            if (!first)
                out << ',';
            first = false;
            out << "{\"guid\":" << fields[0].Get<uint32>()
                << ",\"level\":" << static_cast<uint32>(fields[1].Get<uint8>())
                << ",\"seconds\":" << fields[2].Get<uint32>() << '}';
        } while (levelups->NextRow());
    }

    out << "]}";
    return out.str();
}

std::string GetAccountName(uint32 number)
{
    std::ostringstream name;
    name << "STRICTALT" << std::setfill('0') << std::setw(2) << number;
    return name.str();
}

void AddToRoster(uint32 characterGuid, uint32 accountId)
{
    CharacterDatabase.Execute(
        "INSERT INTO `strict_altbots` (`character_guid`, `account_id`, `enabled`, `always_online`) "
        "VALUES ({}, {}, 1, 1) ON DUPLICATE KEY UPDATE `account_id` = VALUES(`account_id`), "
        "`enabled` = IF(`retired_at` IS NULL, 1, `enabled`), "
        "`always_online` = IF(`retired_at` IS NULL, 1, `always_online`)",
        characterGuid, accountId);
}

std::vector<StrictBotCandidate> GetActiveFactionBots(TeamId team)
{
    std::vector<StrictBotCandidate> candidates;
    QueryResult roster = CharacterDatabase.Query(
        "SELECT `character_guid`, `account_id` FROM `strict_altbots` "
        "WHERE `enabled` = 1 AND `retired_at` IS NULL");

    if (!roster)
        return candidates;

    do
    {
        Field* fields = roster->Fetch();
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
        if (sCharacterCache->GetCharacterTeamByGuid(guid) != team)
            continue;

        candidates.push_back({
            guid,
            fields[1].Get<uint32>(),
            sCharacterCache->GetCharacterGuildIdByGuid(guid)
        });
    } while (roster->NextRow());

    std::stable_sort(candidates.begin(), candidates.end(), [](StrictBotCandidate const& left, StrictBotCandidate const& right)
    {
        bool leftHasGuild = left.currentGuildId != 0;
        bool rightHasGuild = right.currentGuildId != 0;
        if (leftHasGuild != rightHasGuild)
            return !leftHasGuild;
        return left.guid.GetCounter() < right.guid.GetCounter();
    });

    return candidates;
}

bool CreateFactionBots(ChatHandler* handler, TeamId team, uint32 amount)
{
    if (!amount)
        return true;

    std::vector<std::pair<uint32, std::string>> accounts;
    uint32 nextAccountNumber = 1;
    uint32 accountCreations = 0;
    uint32 attempts = 0;

    while (accounts.size() < amount && attempts++ < amount + 10000)
    {
        std::string accountName = GetAccountName(nextAccountNumber++);
        uint32 accountId = sAccountMgr->GetId(accountName);
        if (!accountId)
        {
            AccountOpResult result = sAccountMgr->CreateAccount(accountName, accountName);
            if (result != AOR_OK)
            {
                handler->PSendSysMessage("Could not create account {} (error {}).", accountName, uint32(result));
                continue;
            }

            accounts.emplace_back(0, std::move(accountName));
            ++accountCreations;
            continue;
        }

        QueryResult existing = CharacterDatabase.Query(
            "SELECT 1 FROM `characters` WHERE `account` = {} LIMIT 1", accountId);
        if (!existing)
            accounts.emplace_back(accountId, std::move(accountName));
    }

    if (accounts.size() < amount)
    {
        handler->PSendSysMessage(
            "Could only reserve {} of the {} requested account(s) for new {} bots.",
            accounts.size(), amount, team == TEAM_HORDE ? "Horde" : "Alliance");
        return false;
    }

    if (accountCreations)
    {
        handler->PSendSysMessage("Created {} account(s). Waiting for the database...", accountCreations);
        while (LoginDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static constexpr uint8 AllianceClasses[] =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE,
        CLASS_PRIEST, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };
    static constexpr uint8 HordeClasses[] =
    {
        CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
        CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };
    uint8 const* classes = team == TEAM_HORDE ? HordeClasses : AllianceClasses;
    std::size_t classCount = team == TEAM_HORDE ? std::size(HordeClasses) : std::size(AllianceClasses);
    bool alliance = team == TEAM_ALLIANCE;

    RandomPlayerbotFactory factory;
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> noNameCache;
    uint32 created = 0;

    for (auto const& [reservedAccountId, accountName] : accounts)
    {
        uint32 accountId = reservedAccountId ? reservedAccountId : sAccountMgr->GetId(accountName);
        if (!accountId)
        {
            handler->PSendSysMessage("Account {} was not found after creation.", accountName);
            continue;
        }

        WorldSession* session = new WorldSession(
            accountId, "", 0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
            time(nullptr), LOCALE_enUS, 0, false, false, 0, true);

        uint8 playerClass = classes[urand(0, classCount - 1)];
        Player* player = factory.CreateRandomBot(session, playerClass, noNameCache, alliance);
        if (!player)
        {
            handler->PSendSysMessage("Could not create a {} character for {}.",
                alliance ? "Alliance" : "Horde", accountName);
            delete session;
            continue;
        }

        player->SaveToDB(true, false);
        sCharacterCache->AddCharacterCacheEntry(
            player->GetGUID(), accountId, player->GetName(), player->getGender(),
            player->getRace(), player->getClass(), player->GetLevel());
        AddToRoster(player->GetGUID().GetCounter(), accountId);

        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        ++created;
    }

    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sStrictAltbotMgr->LoadRoster();
    handler->PSendSysMessage(
        "Created {} active {} bot(s).", created, alliance ? "Alliance" : "Horde");
    return created == amount;
}

bool IsStrictBotRecord(ObjectGuid guid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM `strict_altbots` WHERE `character_guid` = {}", guid.GetCounter());
    return static_cast<bool>(result);
}

bool IsActiveStrictBot(ObjectGuid guid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM `strict_altbots` "
        "WHERE `character_guid` = {} AND `enabled` = 1 AND `retired_at` IS NULL",
        guid.GetCounter());
    return static_cast<bool>(result);
}

void RetireBotRecord(ObjectGuid guid)
{
    CharacterDatabase.Execute(
        "UPDATE `strict_altbots` SET `enabled` = 0, `always_online` = 0, `retired_at` = NOW() "
        "WHERE `character_guid` = {} AND `retired_at` IS NULL",
        guid.GetCounter());
}

void LogoutRetiredBot(ObjectGuid guid)
{
    if (Player* bot = ObjectAccessor::FindConnectedPlayer(guid))
        if (bot->GetSession())
            bot->GetSession()->LogoutPlayer(true);
}

std::vector<ObjectGuid> GetActiveStrictBotsInGuild(Guild* guild)
{
    std::vector<ObjectGuid> bots;
    QueryResult result = CharacterDatabase.Query(
        "SELECT gm.`guid` FROM `guild_member` gm "
        "INNER JOIN `strict_altbots` sa ON sa.`character_guid` = gm.`guid` "
        "WHERE gm.`guildid` = {} AND sa.`enabled` = 1 AND sa.`retired_at` IS NULL",
        guild->GetId());

    if (result)
    {
        do
            bots.push_back(ObjectGuid::Create<HighGuid::Player>(result->Fetch()[0].Get<uint32>()));
        while (result->NextRow());
    }

    return bots;
}

bool CreateStrictBotGuild(Player* leader, std::string const& guildName, TeamId team,
                          std::vector<ObjectGuid> const& botGuids, uint32& finalCount)
{
    finalCount = 0;
    if (!leader || botGuids.empty() || leader->GetTeamId() != team ||
        !IsActiveStrictBot(leader->GetGUID()) || leader->GetGuildId())
    {
        return false;
    }

    Guild* guild = new Guild();
    if (!guild->Create(leader, guildName))
    {
        delete guild;
        return false;
    }

    sGuildMgr->AddGuild(guild);
    uint32 moved = 0;
    uint32 skipped = 0;

    for (ObjectGuid guid : botGuids)
    {
        if (guid == leader->GetGUID())
            continue;

        if (!IsActiveStrictBot(guid) || sCharacterCache->GetCharacterTeamByGuid(guid) != team)
        {
            ++skipped;
            continue;
        }

        uint32 currentGuildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
        Guild* currentGuild = currentGuildId ? sGuildMgr->GetGuildById(currentGuildId) : nullptr;
        if (currentGuild && currentGuild->GetLeaderGUID() == guid)
        {
            ++skipped;
            continue;
        }

        if (currentGuild)
            currentGuild->DeleteMember(guid, false, true);

        if (!guild->AddMember(guid))
        {
            if (currentGuild)
                currentGuild->AddMember(guid);
            ++skipped;
            continue;
        }

        if (currentGuild)
            ++moved;
    }

    finalCount = guild->GetMemberSize();
    LOG_INFO("server.loading", "StrictAltbotGuild: guild '{}' created for {} with {} member(s), {} moved, {} skipped",
        guildName, team == TEAM_HORDE ? "Horde" : "Alliance", finalCount, moved, skipped);
    return true;
}
}

class StrictAltbotCommandScript final : public CommandScript
{
public:
    StrictAltbotCommandScript() : CommandScript("StrictAltbotCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable aquariumTable =
        {
            { "roster",     HandleAquariumRoster,     SEC_ADMINISTRATOR, Console::Yes },
            { "inspect",    HandleAquariumInspect,    SEC_ADMINISTRATOR, Console::Yes },
            { "history",    HandleAquariumHistory,    SEC_ADMINISTRATOR, Console::Yes },
            { "benchmarks", HandleAquariumBenchmarks, SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable retireTable =
        {
            { "guild", HandleRetireGuild, SEC_ADMINISTRATOR, Console::Yes },
            { "bot",   HandleRetireBot,   SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable strictBotTable =
        {
            { "aquarium", aquariumTable },
            { "create",   HandleCreate, SEC_ADMINISTRATOR, Console::Yes },
            { "guild",    HandleGuild,  SEC_ADMINISTRATOR, Console::Yes },
            { "retire",   retireTable }
        };

        static ChatCommandTable commandTable =
        {
            { "strictbots", strictBotTable }
        };

        return commandTable;
    }

private:
    static bool HandleAquariumRoster(ChatHandler* handler)
    {
        SendAquariumJson(handler, BuildRosterJson());
        return true;
    }

    static bool HandleAquariumInspect(ChatHandler* handler, uint32 characterGuid)
    {
        SendAquariumJson(handler, BuildSnapshotJson(characterGuid));
        return true;
    }

    static bool HandleAquariumHistory(ChatHandler* handler, uint32 characterGuid)
    {
        SendAquariumJson(handler, BuildHistoryJson(characterGuid));
        return true;
    }

    static bool HandleAquariumBenchmarks(ChatHandler* handler)
    {
        SendAquariumJson(handler, BuildBenchmarksJson());
        return true;
    }

    static bool HandleGuild(ChatHandler* handler, std::string faction, QuotedString quotedGuildName, uint32 amount)
    {
        if (!sStrictAltbotMgr->IsEnabled())
        {
            handler->SendSysMessage("StrictAltbotGuild is disabled.");
            return true;
        }

        std::optional<TeamId> team = ParseFaction(std::move(faction));
        std::string guildName = quotedGuildName;
        if (!team)
        {
            handler->SendSysMessage("Faction must be A/Alliance or H/Horde.");
            return true;
        }

        if (!IsValidGuildName(guildName))
        {
            handler->SendSysMessage("Guild name must be 1-24 characters and may not start or end with whitespace.");
            return true;
        }

        if (!amount || amount > MaxStrictGuildSize)
        {
            handler->PSendSysMessage("Amount must be between 1 and {}.", MaxStrictGuildSize);
            return true;
        }

        if (sGuildMgr->GetGuildByName(guildName))
        {
            handler->PSendSysMessage("Guild '{}' already exists.", guildName);
            return true;
        }

        std::vector<StrictBotCandidate> candidates = GetActiveFactionBots(*team);
        if (candidates.size() < amount)
        {
            uint32 needed = amount - candidates.size();
            if (!CreateFactionBots(handler, *team, needed))
                return true;

            candidates = GetActiveFactionBots(*team);
        }

        if (candidates.size() < amount)
        {
            handler->PSendSysMessage(
                "Only {} active {} bot(s) are available; could not create a guild of {}.",
                candidates.size(), *team == TEAM_HORDE ? "Horde" : "Alliance", amount);
            return true;
        }

        candidates.resize(amount);
        std::vector<ObjectGuid> botGuids;
        botGuids.reserve(candidates.size());
        for (StrictBotCandidate const& candidate : candidates)
            botGuids.push_back(candidate.guid);

        ObjectGuid leaderGuid = botGuids.front();
        Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
        if (!leader)
        {
            sStrictAltbotHolder->QueueOnBotLogin(leaderGuid,
                [guildName = std::move(guildName), team = *team, botGuids = std::move(botGuids)](Player* onlineLeader)
                {
                    uint32 finalCount = 0;
                    if (CreateStrictBotGuild(onlineLeader, guildName, team, botGuids, finalCount))
                        LOG_INFO("server.loading", "StrictAltbotGuild: queued guild '{}' ready with {} member(s)",
                            guildName, finalCount);
                });

            handler->PSendSysMessage(
                "Guild '{}' queued: waiting for the first {} bot to log in. The operation will finish automatically.",
                guildName, *team == TEAM_HORDE ? "Horde" : "Alliance");
            return true;
        }

        uint32 finalCount = 0;
        if (!CreateStrictBotGuild(leader, guildName, *team, botGuids, finalCount))
        {
            handler->PSendSysMessage("Could not create guild '{}'.", guildName);
            return true;
        }

        handler->PSendSysMessage(
            "Guild '{}' ready for {}: {} final member(s).",
            guildName, *team == TEAM_HORDE ? "Horde" : "Alliance", finalCount);
        return true;
    }

    static bool HandleRetireBot(ChatHandler* handler, QuotedString quotedBotName, QuotedString quotedGuildName)
    {
        std::string botName = quotedBotName;
        std::string guildName = quotedGuildName;
        Guild* guild = sGuildMgr->GetGuildByName(guildName);
        if (!guild)
        {
            handler->PSendSysMessage("Guild '{}' was not found.", guildName);
            return true;
        }

        Guild::Member* member = guild->GetMember(botName);
        if (!member)
        {
            ObjectGuid namedGuid = sCharacterCache->GetCharacterGuidByName(botName);
            if (!namedGuid.IsEmpty())
                member = guild->GetMember(namedGuid);
        }

        if (!member)
        {
            handler->PSendSysMessage("Bot '{}' is not a member of guild '{}'.", botName, guildName);
            return true;
        }

        ObjectGuid guid = member->GetGUID();
        std::string canonicalBotName = member->GetName();
        if (!IsStrictBotRecord(guid))
        {
            handler->PSendSysMessage("'{}' is not a strict Altbot.", canonicalBotName);
            return true;
        }

        bool wasActive = IsActiveStrictBot(guid);
        bool wasGuildMaster = guild->GetLeaderGUID() == guid;
        RetireBotRecord(guid);

        if (wasGuildMaster)
            guild->Disband();
        else
            guild->DeleteMember(guid, false, true);

        LogoutRetiredBot(guid);
        sStrictAltbotMgr->LoadRoster();

        handler->PSendSysMessage(
            "Bot '{}' {} retired from guild '{}'.{}",
            canonicalBotName, wasActive ? "has been" : "was already", guildName,
            wasGuildMaster ? " The guild was disbanded because it was the guild master's guild." : "");
        return true;
    }

    static bool HandleRetireGuild(ChatHandler* handler, QuotedString quotedGuildName)
    {
        std::string guildName = quotedGuildName;
        Guild* guild = sGuildMgr->GetGuildByName(guildName);
        if (!guild)
        {
            handler->PSendSysMessage("Guild '{}' was not found.", guildName);
            return true;
        }

        std::vector<ObjectGuid> bots = GetActiveStrictBotsInGuild(guild);
        if (bots.empty())
        {
            handler->PSendSysMessage("Guild '{}' has no active strict Altbot members.", guildName);
            return true;
        }

        bool guildMasterRetired = std::find(bots.begin(), bots.end(), guild->GetLeaderGUID()) != bots.end();
        for (ObjectGuid guid : bots)
            RetireBotRecord(guid);

        if (guildMasterRetired)
            guild->Disband();
        else
            for (ObjectGuid guid : bots)
                guild->DeleteMember(guid, false, true);

        for (ObjectGuid guid : bots)
            LogoutRetiredBot(guid);

        sStrictAltbotMgr->LoadRoster();
        handler->PSendSysMessage(
            "Retired {} strict Altbot(s) from guild '{}'.{}",
            bots.size(), guildName,
            guildMasterRetired ? " The guild was disbanded because its guild master was retired." : "");
        return true;
    }

    static bool HandleCreate(ChatHandler* handler)
    {
        if (!sStrictAltbotMgr->IsEnabled())
        {
            handler->SendSysMessage("StrictAltbotGuild is disabled.");
            return true;
        }

        uint32 createdAccounts = 0;
        for (uint32 number = 1; number <= StrictBotCount; ++number)
        {
            std::string accountName = GetAccountName(number);
            if (sAccountMgr->GetId(accountName))
                continue;

            AccountOpResult result = sAccountMgr->CreateAccount(accountName, accountName);
            if (result == AOR_OK)
                ++createdAccounts;
            else
                handler->PSendSysMessage("Could not create account {} (error {}).", accountName, uint32(result));
        }

        if (createdAccounts)
        {
            handler->PSendSysMessage("Created {} accounts. Waiting for the database...", createdAccounts);
            while (LoginDatabase.QueueSize())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        static constexpr uint8 classes[] =
        {
            CLASS_WARRIOR,
            CLASS_PALADIN,
            CLASS_HUNTER,
            CLASS_ROGUE,
            CLASS_PRIEST,
            CLASS_SHAMAN,
            CLASS_MAGE,
            CLASS_WARLOCK,
            CLASS_DRUID
        };

        uint32 createdCharacters = 0;
        uint32 existingCharacters = 0;
        RandomPlayerbotFactory factory;
        std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> noNameCache;

        for (uint32 number = 1; number <= StrictBotCount; ++number)
        {
            std::string accountName = GetAccountName(number);
            uint32 accountId = sAccountMgr->GetId(accountName);
            if (!accountId)
            {
                handler->PSendSysMessage("Account {} was not found after creation.", accountName);
                continue;
            }

            QueryResult existing = CharacterDatabase.Query(
                "SELECT `guid` FROM `characters` WHERE `account` = {} ORDER BY `guid` LIMIT 1", accountId);
            if (existing)
            {
                AddToRoster(existing->Fetch()[0].Get<uint32>(), accountId);
                ++existingCharacters;
                continue;
            }

            WorldSession* session = new WorldSession(
                accountId, "", 0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                time(nullptr), LOCALE_enUS, 0, false, false, 0, true);

            uint8 playerClass = classes[urand(0, std::size(classes) - 1)];
            Player* player = factory.CreateRandomBot(session, playerClass, noNameCache);
            if (!player)
            {
                handler->PSendSysMessage("Could not create a character for {}.", accountName);
                delete session;
                continue;
            }

            player->SaveToDB(true, false);
            sCharacterCache->AddCharacterCacheEntry(
                player->GetGUID(), accountId, player->GetName(), player->getGender(),
                player->getRace(), player->getClass(), player->GetLevel());
            AddToRoster(player->GetGUID().GetCounter(), accountId);

            player->CleanupsBeforeDelete();
            delete player;
            delete session;
            ++createdCharacters;
        }

        while (CharacterDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        sStrictAltbotMgr->LoadRoster();
        handler->PSendSysMessage(
            "Strict Altbots ready: {} created, {} already existed, {} total. "
            "Account passwords match their account names.",
            createdCharacters, existingCharacters, sStrictAltbotMgr->GetRosterSize());
        return true;
    }
};

void AddSC_strict_altbot_commandscript()
{
    new StrictAltbotCommandScript();
}

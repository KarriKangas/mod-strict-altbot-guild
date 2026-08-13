#include "AccountMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
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

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 StrictBotCount = 40;
constexpr char AllianceStrictGuildName[] = "Strict Altbots";
constexpr char HordeStrictGuildName[] = "Strict Altbots Horde";
constexpr char AquariumPrefix[] = "ALTBO_JSON ";

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
        << ",\"armor\":" << itemTemplate->Armor
        << ",\"damageMin\":" << itemTemplate->Damage[0].DamageMin
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
        << ",\"armor\":" << itemTemplate->Armor
        << ",\"damageMin\":" << itemTemplate->Damage[0].DamageMin
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
            << ",\"itemLevel\":" << (botAI ? botAI->GetEquipGearScore(player) : 0)
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
        "`enabled` = 1, `always_online` = 1",
        characterGuid, accountId);
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
            { "roster",  HandleAquariumRoster,  SEC_ADMINISTRATOR, Console::Yes },
            { "inspect", HandleAquariumInspect, SEC_ADMINISTRATOR, Console::Yes },
            { "history", HandleAquariumHistory, SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable strictBotTable =
        {
            { "aquarium", aquariumTable },
            { "create",   HandleCreate, SEC_ADMINISTRATOR, Console::Yes },
            { "guild",    HandleGuild,  SEC_ADMINISTRATOR, Console::No  }
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

    static bool HandleGuild(ChatHandler* handler)
    {
        Player* owner = handler->GetPlayer();
        if (!owner)
            return false;

        TeamId ownerTeam = owner->GetTeamId();
        char const* strictGuildName = ownerTeam == TEAM_HORDE ? HordeStrictGuildName : AllianceStrictGuildName;
        char const* factionName = ownerTeam == TEAM_HORDE ? "Horde" : "Alliance";

        Guild* guild = sGuildMgr->GetGuildByName(strictGuildName);
        if (!guild)
        {
            if (owner->GetGuildId())
            {
                handler->PSendSysMessage(
                    "{} is already in a guild. Leave it before creating '{}'.",
                    owner->GetName(), strictGuildName);
                return true;
            }

            guild = new Guild();
            if (!guild->Create(owner, strictGuildName))
            {
                delete guild;
                handler->PSendSysMessage("Could not create guild '{}'.", strictGuildName);
                return true;
            }

            sGuildMgr->AddGuild(guild);
        }

        if (guild->GetLeaderGUID() != owner->GetGUID())
        {
            handler->PSendSysMessage(
                "Guild '{}' already exists, but {} is not its guild master.",
                strictGuildName, owner->GetName());
            return true;
        }

        QueryResult roster = CharacterDatabase.Query(
            "SELECT `character_guid` FROM `strict_altbots` WHERE `enabled` = 1");

        uint32 added = 0;
        uint32 moved = 0;
        uint32 alreadyMembers = 0;
        uint32 otherFaction = 0;
        uint32 skipped = 0;

        if (roster)
        {
            do
            {
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(roster->Fetch()[0].Get<uint32>());

                if (sCharacterCache->GetCharacterTeamByGuid(guid) != ownerTeam)
                {
                    ++otherFaction;
                    continue;
                }

                uint32 currentGuildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);

                if (currentGuildId == guild->GetId())
                {
                    ++alreadyMembers;
                    continue;
                }

                Guild* currentGuild = nullptr;
                if (currentGuildId)
                {
                    currentGuild = sGuildMgr->GetGuildById(currentGuildId);
                    if (!currentGuild || currentGuild->GetLeaderGUID() == guid)
                    {
                        ++skipped;
                        continue;
                    }

                    currentGuild->DeleteMember(guid, false, true);
                }

                if (!guild->AddMember(guid))
                {
                    if (currentGuild)
                        currentGuild->AddMember(guid);
                    ++skipped;
                    continue;
                }

                if (currentGuild)
                    ++moved;
                ++added;
            } while (roster->NextRow());
        }

        handler->PSendSysMessage(
            "Guild '{}' ready for {}: {} bot(s) added, {} moved from another guild, {} already members, "
            "{} other-faction bot(s) ignored, {} skipped, {} total members.",
            strictGuildName, factionName, added, moved, alreadyMembers, otherFaction, skipped, guild->GetMemberSize());
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

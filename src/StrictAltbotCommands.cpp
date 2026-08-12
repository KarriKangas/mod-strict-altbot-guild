#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StrictAltbotMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 StrictBotCount = 40;
constexpr char StrictGuildName[] = "Strict Altbots";

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
        static ChatCommandTable strictBotTable =
        {
            { "create", HandleCreate, SEC_ADMINISTRATOR, Console::Yes },
            { "guild",  HandleGuild,  SEC_ADMINISTRATOR, Console::No  }
        };

        static ChatCommandTable commandTable =
        {
            { "strictbots", strictBotTable }
        };

        return commandTable;
    }

private:
    static bool HandleGuild(ChatHandler* handler)
    {
        Player* owner = handler->GetPlayer();
        if (!owner)
            return false;

        Guild* guild = sGuildMgr->GetGuildByName(StrictGuildName);
        if (!guild)
        {
            if (owner->GetGuildId())
            {
                handler->PSendSysMessage(
                    "{} is already in a guild. Leave it before creating '{}'.",
                    owner->GetName(), StrictGuildName);
                return true;
            }

            guild = new Guild();
            if (!guild->Create(owner, StrictGuildName))
            {
                delete guild;
                handler->PSendSysMessage("Could not create guild '{}'.", StrictGuildName);
                return true;
            }

            sGuildMgr->AddGuild(guild);
        }

        if (guild->GetLeaderGUID() != owner->GetGUID())
        {
            handler->PSendSysMessage(
                "Guild '{}' already exists, but {} is not its guild master.",
                StrictGuildName, owner->GetName());
            return true;
        }

        QueryResult roster = CharacterDatabase.Query(
            "SELECT `character_guid` FROM `strict_altbots` WHERE `enabled` = 1");

        uint32 added = 0;
        uint32 alreadyMembers = 0;
        uint32 skipped = 0;

        if (roster)
        {
            do
            {
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(roster->Fetch()[0].Get<uint32>());
                uint32 currentGuildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);

                if (currentGuildId == guild->GetId())
                {
                    ++alreadyMembers;
                    continue;
                }

                if (currentGuildId || !guild->AddMember(guid))
                {
                    ++skipped;
                    continue;
                }

                ++added;
            } while (roster->NextRow());
        }

        handler->PSendSysMessage(
            "Guild '{}' ready: {} bot(s) added, {} already members, {} skipped, {} total members.",
            StrictGuildName, added, alreadyMembers, skipped, guild->GetMemberSize());
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

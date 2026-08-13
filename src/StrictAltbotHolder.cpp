#include "StrictAltbotHolder.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QueryResult.h"
#include "StrictAltbotMgr.h"
#include "World.h"
#include "WorldSession.h"

StrictAltbotHolder* StrictAltbotHolder::instance()
{
    static StrictAltbotHolder instance;
    return &instance;
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
        "WHERE `enabled` = 1 AND `always_online` = 1");

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
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        botAI->SetMaster(nullptr);
        botAI->SetCheat(BotCheatMask::none);
    }

    bot->SetTaxiCheater(false);
    LOG_INFO("server.loading", "StrictAltbotGuild: {} logged in", bot->GetName());
}

void StrictAltbotHolder::EnableAutonomy(Player* bot)
{
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        botAI->ChangeStrategy("+new rpg,+grind,+lfg", BOT_STATE_NON_COMBAT);
        LOG_INFO("server.loading", "StrictAltbotGuild: {} autonomous strategies enabled", bot->GetName());
    }
}

void StrictAltbotHolder::RemoveBot(ObjectGuid guid)
{
    RemoveFromPlayerbotsMap(guid);
}

void StrictAltbotHolder::Shutdown()
{
    _shuttingDown = true;
    _loading.clear();
    LogoutAllBots();
}

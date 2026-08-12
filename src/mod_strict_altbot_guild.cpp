#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StrictAltbotMgr.h"

void AddSC_strict_altbot_commandscript();

namespace
{
uint32 NormalBotCheatMask = 0;
}

class StrictAltbotGuildWorldScript final : public WorldScript
{
public:
    StrictAltbotGuildWorldScript() : WorldScript("StrictAltbotGuildWorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
            WORLDHOOK_ON_STARTUP
        })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        bool enabled = sConfigMgr->GetOption<bool>("StrictAltbotGuild.Enable", true);
        sStrictAltbotMgr->SetEnabled(enabled);

        LOG_INFO("server.loading", "StrictAltbotGuild: {}", enabled ? "enabled" : "disabled");

        if (reload && enabled)
            sStrictAltbotMgr->LoadRoster();
    }

    void OnBeforeWorldInitialized() override
    {
        if (!sStrictAltbotMgr->IsEnabled())
            return;

        NormalBotCheatMask = sPlayerbotAIConfig.botCheatMask;
        sPlayerbotAIConfig.botCheatMask = 0;

        LOG_INFO(
            "server.loading",
            "StrictAltbotGuild: strict Altbots have no cheats; normal bots keep cheat mask {}",
            NormalBotCheatMask);
    }

    void OnStartup() override
    {
        sStrictAltbotMgr->LoadRoster();
    }
};

class StrictAltbotGuildPlayerScript final : public PlayerScript
{
public:
    StrictAltbotGuildPlayerScript() : PlayerScript(
        "StrictAltbotGuildPlayerScript", { PLAYERHOOK_ON_UPDATE })
    {
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!sStrictAltbotMgr->IsEnabled())
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return;

        if (sStrictAltbotMgr->IsStrictAltbot(player->GetGUID().GetCounter()))
        {
            botAI->SetCheat(BotCheatMask::none);
            if (player->isTaxiCheater())
                player->SetTaxiCheater(false);
            return;
        }

        botAI->SetCheat(BotCheatMask(static_cast<uint32>(botAI->GetCheat()) | NormalBotCheatMask));
        if ((NormalBotCheatMask & static_cast<uint32>(BotCheatMask::taxi)) && !player->isTaxiCheater())
            player->SetTaxiCheater(true);
    }
};

void Addmod_strict_altbot_guildScripts()
{
    new StrictAltbotGuildWorldScript();
    new StrictAltbotGuildPlayerScript();
    AddSC_strict_altbot_commandscript();
}

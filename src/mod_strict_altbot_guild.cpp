#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "StrictAltbotMgr.h"

class StrictAltbotGuildWorldScript final : public WorldScript
{
public:
    StrictAltbotGuildWorldScript() : WorldScript("StrictAltbotGuildWorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
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

    void OnStartup() override
    {
        sStrictAltbotMgr->LoadRoster();
    }
};

void Addmod_strict_altbot_guildScripts()
{
    new StrictAltbotGuildWorldScript();
}

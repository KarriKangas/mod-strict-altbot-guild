#include "Config.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StrictAltbotHolder.h"
#include "StrictAltbotMgr.h"

#include <unordered_set>

void AddSC_strict_altbot_commandscript();

namespace
{
constexpr uint32 HunterAmmoRestockThreshold = 200;

uint32 NormalBotCheatMask = 0;
std::unordered_set<ObjectGuid> HunterAmmoTripsWithSuppressedGrind;

uint32 GetHunterAmmoCount(PlayerbotAI* botAI)
{
    auto* value = botAI->GetAiObjectContext()->GetValue<uint32>("item count", "ammo");
    return value ? value->Get() : 0;
}

bool HasHunterAmmoWeapon(Player* player)
{
    if (!player || player->getClass() != CLASS_HUNTER)
        return false;

    Item* rangedWeapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!rangedWeapon || !rangedWeapon->GetTemplate())
        return false;

    switch (rangedWeapon->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return true;
        default:
            return false;
    }
}

void EnsureHunterAmmoSelected(Player* player, PlayerbotAI* botAI)
{
    if (!HasHunterAmmoWeapon(player) || player->GetUInt32Value(PLAYER_AMMO_ID) != 0)
        return;

    Item* ammo = botAI->FindAmmo();
    if (!ammo)
        return;

    player->SetAmmo(ammo->GetEntry());
    LOG_INFO("server.loading", "StrictAltbotGuild: {} selected hunter ammo {} from inventory",
        player->GetName(), ammo->GetEntry());
}

bool IsHunterAmmoTrip(Player* player, PlayerbotAI* botAI)
{
    if (!HasHunterAmmoWeapon(player) || GetHunterAmmoCount(botAI) >= HunterAmmoRestockThreshold)
        return false;

    NewRpgStatus status = botAI->rpgInfo.GetStatus();
    return status == RPG_GO_CAMP || status == RPG_WANDER_NPC;
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

void UpdateHunterAmmoTripStrategy(Player* player, PlayerbotAI* botAI)
{
    ObjectGuid guid = player->GetGUID();
    bool suppressed = HunterAmmoTripsWithSuppressedGrind.contains(guid);

    if (!IsHunterAmmoTrip(player, botAI))
    {
        if (suppressed)
        {
            botAI->ChangeStrategy("+grind", BOT_STATE_NON_COMBAT);
            HunterAmmoTripsWithSuppressedGrind.erase(guid);
            LOG_INFO("server.loading", "StrictAltbotGuild: {} restored grinding after hunter ammo trip", player->GetName());
        }
        return;
    }

    if (!suppressed && botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
    {
        botAI->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
        HunterAmmoTripsWithSuppressedGrind.insert(guid);
        LOG_INFO("server.loading", "StrictAltbotGuild: {} suspended grinding for hunter ammo trip", player->GetName());
    }

    ClearStaleGrindTarget(player, botAI);
}
}

class StrictAltbotGuildWorldScript final : public WorldScript
{
public:
    StrictAltbotGuildWorldScript() : WorldScript("StrictAltbotGuildWorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
            WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_SHUTDOWN
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

    void OnUpdate(uint32 diff) override
    {
        sStrictAltbotHolder->Update(diff);
    }

    void OnShutdown() override
    {
        HunterAmmoTripsWithSuppressedGrind.clear();
        sStrictAltbotHolder->Shutdown();
    }
};

class StrictAltbotGuildPlayerScript final : public PlayerScript
{
public:
    StrictAltbotGuildPlayerScript() : PlayerScript(
        "StrictAltbotGuildPlayerScript",
        {
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_QUEST_ABANDON,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_CAN_PLAYER_USE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT
        })
    {
    }

    bool OnPlayerCanUseChat(Player* player, uint32, uint32, std::string&) override
    {
        return CanUseChat(player);
    }

    bool OnPlayerCanUseChat(Player* player, uint32, uint32, std::string&, Player*) override
    {
        return CanUseChat(player);
    }

    bool OnPlayerCanUseChat(Player* player, uint32, uint32, std::string&, Group*) override
    {
        return CanUseChat(player);
    }

    bool OnPlayerCanUseChat(Player* player, uint32, uint32, std::string&, Guild*) override
    {
        return CanUseChat(player);
    }

    bool OnPlayerCanUseChat(Player* player, uint32, uint32, std::string&, Channel*) override
    {
        return CanUseChat(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (player && sStrictAltbotMgr->IsStrictAltbot(player->GetGUID().GetCounter()))
            sStrictAltbotHolder->RecordLevelUp(player, oldLevel);
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        sStrictAltbotHolder->RecordQuestDrop(player, questId);
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
            player->CleanupChannels();
            if (player->isTaxiCheater())
                player->SetTaxiCheater(false);
            if (botAI->rpgInfo.GetStatus() == RPG_REST)
                botAI->rpgInfo.ChangeToWanderRandom();
            EnsureHunterAmmoSelected(player, botAI);
            UpdateHunterAmmoTripStrategy(player, botAI);
            sStrictAltbotHolder->UpdateRpgServices(player);
            return;
        }

        botAI->SetCheat(BotCheatMask(static_cast<uint32>(botAI->GetCheat()) | NormalBotCheatMask));
        if ((NormalBotCheatMask & static_cast<uint32>(BotCheatMask::taxi)) && !player->isTaxiCheater())
            player->SetTaxiCheater(true);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (sStrictAltbotMgr->IsStrictAltbot(player->GetGUID().GetCounter()))
        {
            HunterAmmoTripsWithSuppressedGrind.erase(player->GetGUID());
            sStrictAltbotHolder->RemoveBot(player->GetGUID());
        }
    }

private:
    static bool CanUseChat(Player* player)
    {
        return !player || !sStrictAltbotMgr->IsStrictAltbot(player->GetGUID().GetCounter());
    }
};

void Addmod_strict_altbot_guildScripts()
{
    new StrictAltbotGuildWorldScript();
    new StrictAltbotGuildPlayerScript();
    AddSC_strict_altbot_commandscript();
}

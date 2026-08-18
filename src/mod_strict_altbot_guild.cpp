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

#include <unordered_map>

void AddSC_strict_altbot_commandscript();

namespace
{
constexpr uint32 GearSafetyReconcileIntervalMs = 5 * 60 * 1000;
constexpr uint32 GearReconcileRetryMs = 10 * 1000;
constexpr uint32 GearDirtyStaggerBuckets = 30;
constexpr uint32 GearDirtyStaggerStepMs = 100;

uint32 NormalBotCheatMask = 0;

struct GearReconcileState
{
    bool dirty = false;
    uint32 dirtyDelayMs = 0;
    uint32 safetyDelayMs = 0;
};

std::unordered_map<ObjectGuid, GearReconcileState> GearReconcileStates;

GearReconcileState& GetGearReconcileState(ObjectGuid guid)
{
    auto [it, inserted] = GearReconcileStates.try_emplace(guid);

    if (inserted)
    {
        it->second.safetyDelayMs =
            (1 + static_cast<uint32>(guid.GetCounter() % 300)) * 1000;
    }

    return it->second;
}

void TickTimer(uint32& timer, uint32 diff)
{
    timer = diff >= timer ? 0 : timer - diff;
}

void MarkGearDirty(Player* player)
{
    GearReconcileState& state = GetGearReconcileState(player->GetGUID());

    state.dirty = true;
    state.dirtyDelayMs =
        static_cast<uint32>(player->GetGUID().GetCounter() % GearDirtyStaggerBuckets) *
        GearDirtyStaggerStepMs;
}

void UpdateGearReconciliation(Player* player, PlayerbotAI* botAI, uint32 diff)
{
    GearReconcileState& state = GetGearReconcileState(player->GetGUID());

    TickTimer(state.safetyDelayMs, diff);

    if (state.dirty)
        TickTimer(state.dirtyDelayMs, diff);

    bool const dirtyDue = state.dirty && state.dirtyDelayMs == 0;
    bool const safetyDue = state.safetyDelayMs == 0;

    if (!dirtyDue && !safetyDue)
        return;

    if (!player->IsAlive() ||
        player->IsInCombat() ||
        player->IsBeingTeleported() ||
        botAI->GetState() != BOT_STATE_NON_COMBAT)
    {
        return;
    }

    bool const success = botAI->DoSpecificAction("equip upgrade", Event(), true);

    if (success)
    {
        state.dirty = false;
        state.dirtyDelayMs = 0;

        if (safetyDue)
            state.safetyDelayMs = GearSafetyReconcileIntervalMs;
    }
    else
    {
        if (dirtyDue)
            state.dirtyDelayMs = GearReconcileRetryMs;

        if (safetyDue)
            state.safetyDelayMs = GearReconcileRetryMs;
    }
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
        GearReconcileStates.clear();
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
            PLAYERHOOK_ON_QUEST_REWARD_ITEM,
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

    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 /*count*/) override
    {
        if (!sStrictAltbotMgr->IsEnabled() || !player || !item)
            return;

        if (!sStrictAltbotMgr->IsStrictAltbot(player->GetGUID().GetCounter()))
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
            return;

        MarkGearDirty(player);
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        sStrictAltbotHolder->RecordQuestDrop(player, questId);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
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
            UpdateGearReconciliation(player, botAI, diff);
            EnsureHunterAmmoSelected(player, botAI);
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
            GearReconcileStates.erase(player->GetGUID());
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

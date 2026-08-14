#pragma once

#include "PlayerbotMgr.h"

#include <unordered_set>

class StrictAltbotHolder final : public PlayerbotHolder
{
public:
    static StrictAltbotHolder* instance();

    void Update(uint32 diff);
    void UpdateRpgServices(Player* bot);
    void RecordFirstLogin(Player* bot);
    void RecordLevelUp(Player* bot, uint8 oldLevel);
    void RemoveBot(ObjectGuid guid);
    void Shutdown();

protected:
    void OnBotLoginInternal(Player* bot) override;

private:
    void LoginBot(ObjectGuid guid, uint32 accountId);
    void EnableAutonomy(Player* bot);

    uint32 _updateTimer = 0;
    uint32 _lastOnlineCount = 0;
    bool _shuttingDown = false;
    std::unordered_set<ObjectGuid> _loading;
};

#define sStrictAltbotHolder StrictAltbotHolder::instance()

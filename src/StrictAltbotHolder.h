#pragma once

#include "PlayerbotMgr.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

class StrictAltbotHolder final : public PlayerbotHolder
{
public:
    static StrictAltbotHolder* instance();

    void Update(uint32 diff);
    void UpdateRpgServices(Player* bot);
    void RecordFirstLogin(Player* bot);
    void RecordLevelUp(Player* bot, uint8 oldLevel);
    void RecordQuestDrop(Player* bot, uint32 questId);
    void RemoveBot(ObjectGuid guid);
    void QueueOnBotLogin(ObjectGuid guid, std::function<void(Player*)> callback);
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
    std::unordered_map<ObjectGuid, std::function<void(Player*)>> _loginCallbacks;
};

#define sStrictAltbotHolder StrictAltbotHolder::instance()

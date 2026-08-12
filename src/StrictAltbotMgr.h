#pragma once

#include "Define.h"

#include <unordered_set>

class StrictAltbotMgr
{
public:
    static StrictAltbotMgr* instance();

    void SetEnabled(bool enabled);
    bool IsEnabled() const { return _enabled; }

    void LoadRoster();
    bool IsStrictAltbot(uint32 characterGuid) const;
    std::size_t GetRosterSize() const { return _strictAltbots.size(); }

private:
    bool _enabled = false;
    std::unordered_set<uint32> _strictAltbots;
};

#define sStrictAltbotMgr StrictAltbotMgr::instance()

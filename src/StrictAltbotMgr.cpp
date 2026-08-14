#include "StrictAltbotMgr.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

StrictAltbotMgr* StrictAltbotMgr::instance()
{
    static StrictAltbotMgr instance;
    return &instance;
}

void StrictAltbotMgr::SetEnabled(bool enabled)
{
    _enabled = enabled;
    if (!_enabled)
        _strictAltbots.clear();
}

void StrictAltbotMgr::LoadRoster()
{
    _strictAltbots.clear();

    if (!_enabled)
        return;

    QueryResult result = CharacterDatabase.Query(
        "SELECT `character_guid` FROM `strict_altbots` "
        "WHERE `enabled` = 1 AND `retired_at` IS NULL");

    if (result)
    {
        do
        {
            _strictAltbots.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "StrictAltbotGuild: loaded {} strict Altbot(s)", _strictAltbots.size());
}

bool StrictAltbotMgr::IsStrictAltbot(uint32 characterGuid) const
{
    return _enabled && _strictAltbots.contains(characterGuid);
}

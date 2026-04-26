/*
 * mod-dungeon-master — dm_allmap_script.cpp
 * Triggers dungeon population when a player enters the instance map.
 */

#include "ScriptMgr.h"
#include "Map.h"
#include "Player.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "Chat.h"
#include "Log.h"
#include <cstdio>

using namespace DungeonMaster;

class dm_allmap_script : public AllMapScript
{
public:
    dm_allmap_script() : AllMapScript("dm_allmap_script") {}

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        if (!sDMConfig->IsEnabled() || !map || !player)
            return;

        // Only care about dungeon maps
        if (!map->IsDungeon())
            return;

        Session* session = sDungeonMasterMgr->GetSessionByPlayer(player->GetGUID());
        if (!session)
        {
            LOG_DEBUG("module", "DungeonMaster: OnPlayerEnterAll — {} entered map {} but has no session",
                player->GetName(), map->GetId());
            return;
        }

        LOG_INFO("module", "DungeonMaster: OnPlayerEnterAll — {} entered map {} (session {} state {} mapId {} mobs {} bosses {})",
            player->GetName(), map->GetId(), session->SessionId,
            static_cast<int>(session->State), session->MapId,
            session->TotalMobs, session->TotalBosses);

        if (session->State != SessionState::InProgress)
            return;

        if (map->GetId() != session->MapId)
            return;

        // Only populate once — guard against duplicate triggers.
        // The Update tick also triggers populate as a reliable fallback.
        if (session->TotalMobs > 0 || session->TotalBosses > 0)
            return;

        InstanceMap* instance = map->ToInstanceMap();
        if (!instance)
            return;

        session->InstanceId = instance->GetInstanceId();
        sDungeonMasterMgr->RegisterSessionInstance(session->SessionId, session->InstanceId);

        // Do not populate here while the party is still teleporting into the instance.
        // ClearDungeonCreatures() performs a player-grid sweep, and running it from the
        // enter hook can race against the live player list during map-enter churn.
        // DungeonMasterMgr::Update() already has a deferred populate path once the map
        // has stabilized and at least one session player is safely present in-instance.
        LOG_INFO("module", "DungeonMaster: Session {} — population deferred from OnPlayerEnterAll (player {}, map {}, inst {})",
            session->SessionId, player->GetName(), map->GetId(), session->InstanceId);
    }
};

void AddSC_dm_allmap_script()
{
    new dm_allmap_script();
}

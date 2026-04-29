/*
 * mod-dungeon-master — dm_launch_shared.h
 * Shared validation and launch helpers for NPC gossip and player commands.
 */

#ifndef MOD_DUNGEON_MASTER_DM_LAUNCH_SHARED_H
#define MOD_DUNGEON_MASTER_DM_LAUNCH_SHARED_H

#include "Define.h"
#include <cstdint>
#include <string>

class Player;
class ObjectGuid;

namespace DungeonMaster
{

struct PlayerDMSelection
{
    uint32 DifficultyId = 0;
    uint32 ThemeId = 0;
    uint32 MapId = 0;
    bool ScaleToParty = true;
    bool IsRoguelike = false;
};

bool ValidateDungeonMasterParty(Player* player, std::string& reason);
bool HasNearbyDungeonMasterNpc(Player* player, float range = 12.0f);
bool StartChallengeFromSelection(Player* player, PlayerDMSelection const& selection, bool requireNearbyNpc = false);
bool StartRoguelikeFromSelection(Player* player, PlayerDMSelection const& selection, bool requireNearbyNpc = false);

} // namespace DungeonMaster

#endif

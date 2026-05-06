/*
 * mod-dungeon-master — dm_launch_shared.cpp
 * Shared validation and launch helpers for NPC gossip and player commands.
 */

#include "dm_launch_shared.h"

#include "Chat.h"
#include "DBCStores.h"
#include "DMConfig.h"
#include "DungeonMasterMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Group.h"
#include "RoguelikeMgr.h"
#ifdef MOD_PLAYERBOTS
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#endif

namespace
{
constexpr char const* DM_LOG_CATEGORY = "module.DungeonMaster";
constexpr char const* CHALLENGE_MODES_SOURCE = "mod-challenge-modes";

struct ChallengeModeInfo
{
    uint8 Setting;
    char const* Name;
};

constexpr ChallengeModeInfo CHALLENGE_MODE_SETTINGS[] = {
    { 0, "Hardcore" },
    { 1, "Semi-Hardcore" },
    { 2, "Self-Crafted" },
    { 3, "Low-Quality Equipment" },
    { 4, "Slow XP" },
    { 5, "Very Slow XP" },
    { 6, "Quest XP Only" },
    { 7, "Iron Man" }
};

std::string GetActiveChallengeModeName(Player* player)
{
    if (!player)
        return {};

    for (ChallengeModeInfo const& info : CHALLENGE_MODE_SETTINGS)
        if (player->GetPlayerSetting(CHALLENGE_MODES_SOURCE, info.Setting).value)
            return info.Name;

    return {};
}

uint32 CountEligiblePartyMembers(Player* player)
{
    if (!player)
        return 0;

    Group* group = player->GetGroup();
    if (!group)
        return 1;

    uint32 count = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->IsInWorld())
            ++count;
    }

    return std::max<uint32>(count, 1);
}

uint32 GetDungeonMaxPlayers(uint32 mapId)
{
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
    if (!mapEntry)
        return DungeonMaster::MAX_PARTY_SIZE;

    Difficulty difficulty = mapEntry->IsRaid() ? RAID_DIFFICULTY_10MAN_NORMAL : DUNGEON_DIFFICULTY_NORMAL;
    if (MapDifficulty const* mapDifficulty = GetMapDifficultyData(mapId, difficulty))
        if (mapDifficulty->maxPlayers)
            return mapDifficulty->maxPlayers;

    if (mapEntry->maxPlayers)
        return mapEntry->maxPlayers;

    return DungeonMaster::MAX_PARTY_SIZE;
}

bool ValidateSelectedDungeonGroupSize(Player* player, uint32 mapId, std::string& reason)
{
    if (!player)
    {
        reason = "invalid player session";
        return false;
    }

    uint32 partySize = CountEligiblePartyMembers(player);
    uint32 maxPlayers = GetDungeonMaxPlayers(mapId);
    if (partySize <= maxPlayers)
        return true;

    DungeonMaster::DungeonInfo const* dungeon = sDMConfig->GetDungeon(mapId);
    std::string dungeonName = dungeon ? dungeon->Name : Acore::StringFormat("map {}", mapId);
    reason = Acore::StringFormat("{} allows at most {} player{} but your group has {} eligible member{}",
        dungeonName,
        maxPlayers,
        maxPlayers == 1 ? "" : "s",
        partySize,
        partySize == 1 ? "" : "s");
    return false;
}

bool ValidateRoguelikeGroupSize(Player* player, std::string& reason)
{
    uint32 partySize = CountEligiblePartyMembers(player);
    if (partySize <= DungeonMaster::MAX_PARTY_SIZE)
        return true;

    reason = Acore::StringFormat("Roguelike runs allow at most {} players because the rotation uses 5-player dungeons", DungeonMaster::MAX_PARTY_SIZE);
    return false;
}

bool ValidateCommonLaunchPreconditions(Player* player, bool requireNearbyNpc, char const* label)
{
    if (!player || !player->GetSession())
        return false;

    ChatHandler handler(player->GetSession());

    if (!sDMConfig->IsEnabled())
    {
        handler.PSendSysMessage("|cFFFF0000[{}]|r Dungeon Master is currently unavailable.", label);
        return false;
    }

    if (requireNearbyNpc && !DungeonMaster::HasNearbyDungeonMasterNpc(player))
    {
        handler.PSendSysMessage("|cFFFF0000[{}]|r Move closer to a Dungeon Master NPC before launching from the AIO panel.", label);
        return false;
    }

    std::string validationError;
    if (!DungeonMaster::ValidateDungeonMasterParty(player, validationError))
    {
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: {} start blocked for {} — {}", label, player->GetName(), validationError);
        handler.PSendSysMessage("|cFFFF0000[{}]|r {}.", label, validationError);
        return false;
    }

    if (sDungeonMasterMgr->GetSessionIdByPlayer(player->GetGUID()) != 0)
    {
        handler.PSendSysMessage("|cFFFF0000[{}]|r You are already in an active challenge!", label);
        return false;
    }

    if (sRoguelikeMgr->IsPlayerInRun(player->GetGUID()))
    {
        handler.PSendSysMessage("|cFFFF0000[{}]|r You are already in an active roguelike run!", label);
        return false;
    }

    if (sDungeonMasterMgr->IsOnCooldown(player->GetGUID()))
    {
        uint32 remaining = sDungeonMasterMgr->GetRemainingCooldown(player->GetGUID());
        handler.PSendSysMessage(
            "|cFFFFFF00[{}]|r Wait |cFFFFFFFF{}|r min |cFFFFFFFF{}|r sec before your next challenge.",
            label, remaining / 60, remaining % 60);
        return false;
    }

    return true;
}
}

namespace DungeonMaster
{

bool ValidateDungeonMasterParty(Player* player, std::string& reason)
{
    if (!player || !player->GetSession())
    {
        reason = "invalid player session";
        return false;
    }

    if (!GetActiveChallengeModeName(player).empty())
    {
        reason = Acore::StringFormat("{} cannot use Dungeon Master while Challenge Modes is active ({})",
            player->GetName(), GetActiveChallengeModeName(player));
        return false;
    }

    Group* group = player->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetSession())
            continue;

        std::string challengeMode = GetActiveChallengeModeName(member);
        if (!challengeMode.empty())
        {
            reason = Acore::StringFormat("{} is using Challenge Modes ({}) and cannot enter Dungeon Master",
                member->GetName(), challengeMode);
            return false;
        }
    }

    return true;
}

bool HasNearbyDungeonMasterNpc(Player* player, float range)
{
    if (!player)
        return false;

    return player->FindNearestCreature(sDMConfig->GetNpcEntry(), range, true) != nullptr;
}

bool StartChallengeFromSelection(Player* player, PlayerDMSelection const& selection, bool requireNearbyNpc)
{
    if (!ValidateCommonLaunchPreconditions(player, requireNearbyNpc, "Dungeon Master"))
        return false;

    if (!sDungeonMasterMgr->CanCreateNewSession())
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Too many challenges running. Try again later.");
        return false;
    }

    DifficultyTier const* difficulty = sDMConfig->GetDifficulty(selection.DifficultyId);
    if (!difficulty || !difficulty->IsValidForLevel(player->GetLevel()))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Level requirement not met!");
        return false;
    }

    Theme const* theme = sDMConfig->GetTheme(selection.ThemeId);
    if (!theme)
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Invalid theme selection.");
        return false;
    }

    uint32 mapId = selection.MapId;
    if (mapId == 0)
    {
        mapId = sDungeonMasterMgr->SelectWeightedDungeon(selection.DifficultyId, selection.ThemeId);
        if (mapId == 0)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r No dungeons available!");
            return false;
        }
    }

    std::string groupSizeError;
    if (!ValidateSelectedDungeonGroupSize(player, mapId, groupSizeError))
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cFFFF0000[Dungeon Master]|r {}.", groupSizeError);
        return false;
    }

    Session* session = sDungeonMasterMgr->CreateSession(player, selection.DifficultyId, selection.ThemeId, mapId, selection.ScaleToParty);
    if (!session)
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Failed to create session!");
        return false;
    }

    if (!sDungeonMasterMgr->StartDungeon(session))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Failed to initialize dungeon!");
        sDungeonMasterMgr->AbandonSession(session->SessionId);
        return false;
    }

    if (!sDungeonMasterMgr->TeleportPartyIn(session))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Dungeon Master]|r Teleport failed!");
        sDungeonMasterMgr->AbandonSession(session->SessionId);
        return false;
    }

    if (sDMConfig->ShouldAnnounceCompletion())
    {
        DungeonInfo const* dungeon = sDMConfig->GetDungeon(mapId);
        char header[256];
        snprintf(header, sizeof(header),
            "|cFF00FF00[Dungeon Master]|r |cFFFFFFFF%s|r started a |cFFFFD700%s|r |cFF00FFFF%s|r challenge!",
            player->GetName().c_str(), difficulty->Name.c_str(), theme->Name.c_str());

        char detail[256];
        snprintf(detail, sizeof(detail),
            "|cFFFFD700[Dungeon Master]|r Difficulty: |cFF00FF00%s|r  Theme: |cFF00FF00%s|r  Dungeon: |cFF00FF00%s|r  Scaling: |cFF00FF00%s|r",
            difficulty->Name.c_str(),
            theme->Name.c_str(),
            dungeon ? dungeon->Name.c_str() : "Random",
            selection.ScaleToParty ? "Party Level" : "Dungeon Difficulty");

        for (auto const& playerData : session->Players)
            if (Player* member = ObjectAccessor::FindPlayer(playerData.PlayerGuid))
                if (member->GetSession())
                {
                    ChatHandler(member->GetSession()).SendSysMessage(header);
                    ChatHandler(member->GetSession()).SendSysMessage(detail);
                }
    }

    return true;
}

bool StartRoguelikeFromSelection(Player* player, PlayerDMSelection const& selection, bool requireNearbyNpc)
{
    if (!ValidateCommonLaunchPreconditions(player, requireNearbyNpc, "Roguelike"))
        return false;

    DifficultyTier const* difficulty = sDMConfig->GetDifficulty(selection.DifficultyId);
    if (!difficulty || !difficulty->IsValidForLevel(player->GetLevel()))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Level requirement not met!");
        return false;
    }

    if (!sDMConfig->GetTheme(selection.ThemeId))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Invalid theme selection.");
        return false;
    }

    std::string groupSizeError;
    if (!ValidateRoguelikeGroupSize(player, groupSizeError))
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cFFFF0000[Roguelike]|r {}.", groupSizeError);
        return false;
    }

    if (!sRoguelikeMgr->StartRun(player, selection.DifficultyId, selection.ThemeId, selection.ScaleToParty))
    {
        ChatHandler(player->GetSession()).SendSysMessage(
            "|cFFFF0000[Roguelike]|r Failed to start roguelike run!");
        return false;
    }

    ChatHandler(player->GetSession()).SendSysMessage(
        "|cFF00FFFF[Roguelike]|r Run started! Clear dungeons to progress. Good luck!");
    return true;
}

} // namespace DungeonMaster

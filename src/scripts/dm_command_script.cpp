/*
 * mod-dungeon-master — dm_command_script.cpp
 * GM commands: .dm reload, .dm status, .dm list, .dm end, .dm clearcooldown
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "Group.h"
#include "DungeonMasterMgr.h"
#include "DMConfig.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "dm_launch_shared.h"
#include <cstdio>

using namespace Acore::ChatCommands;
using namespace DungeonMaster;

class dm_command_script : public CommandScript
{
public:
    dm_command_script() : CommandScript("dm_command_script") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dmTable =
        {
            { "start",         HandlePlayerStart,    SEC_PLAYER,        Console::No  },
            { "roguelike",     HandlePlayerRogue,    SEC_PLAYER,        Console::No  },
            { "help",          HandlePlayerHelp,     SEC_PLAYER,        Console::No  },
            { "reload",        HandleReload,        SEC_ADMINISTRATOR,  Console::Yes },
            { "status",        HandleStatus,        SEC_GAMEMASTER,     Console::Yes },
            { "list",          HandleList,           SEC_GAMEMASTER,     Console::Yes },
            { "end",           HandleEnd,            SEC_ADMINISTRATOR,  Console::No  },
            { "clearcooldown", HandleClearCD,        SEC_GAMEMASTER,     Console::No  },
            { "",              HandlePlayerHelp,     SEC_PLAYER,         Console::No  },
        };
        static ChatCommandTable root = { { "dm", dmTable } };
        return root;
    }

    static bool HandlePlayerHelp(ChatHandler* h)
    {
        h->SendSysMessage("Dungeon Master player commands:");
        h->SendSysMessage("  .dm start <difficultyId> <themeId> <mapId|0> <party|tier>");
        h->SendSysMessage("  .dm roguelike <difficultyId> <themeId> <party|tier>");
        h->SendSysMessage("Use these near a Dungeon Master NPC. Map ID 0 means weighted random dungeon.");
        return true;
    }

    static bool ParseScaleMode(std::string_view token, bool& scaleToParty)
    {
        if (StringEqualI(token, "party") || StringEqualI(token, "scaled"))
        {
            scaleToParty = true;
            return true;
        }

        if (StringEqualI(token, "tier") || StringEqualI(token, "dungeon"))
        {
            scaleToParty = false;
            return true;
        }

        return false;
    }

    static bool HandlePlayerStart(ChatHandler* h, char const* args)
    {
        if (!h || !h->GetSession())
            return false;

        Player* player = h->GetSession()->GetPlayer();
        if (!player)
            return false;

        std::vector<std::string_view> tokens = Acore::Tokenize(args ? args : "", ' ', false);
        if (tokens.size() != 4)
            return HandlePlayerHelp(h);

        auto difficultyId = Acore::StringTo<uint32>(tokens[0]);
        auto themeId = Acore::StringTo<uint32>(tokens[1]);
        bool scaleToParty = true;

        if (!difficultyId || !themeId || !ParseScaleMode(tokens[3], scaleToParty))
        {
            h->SendSysMessage("DungeonMaster: invalid arguments. Usage: .dm start <difficultyId> <themeId> <mapId|0> <party|tier>");
            return false;
        }

        uint32 mapId = 0;
        if (!StringEqualI(tokens[2], "random"))
        {
            auto parsedMapId = Acore::StringTo<uint32>(tokens[2]);
            if (!parsedMapId)
            {
                h->SendSysMessage("DungeonMaster: invalid mapId. Use 0 or random for weighted random selection.");
                return false;
            }

            mapId = *parsedMapId;
        }

        PlayerDMSelection selection;
        selection.DifficultyId = *difficultyId;
        selection.ThemeId = *themeId;
        selection.MapId = mapId;
        selection.ScaleToParty = scaleToParty;
        selection.IsRoguelike = false;
        return StartChallengeFromSelection(player, selection, true);
    }

    static bool HandlePlayerRogue(ChatHandler* h, char const* args)
    {
        if (!h || !h->GetSession())
            return false;

        Player* player = h->GetSession()->GetPlayer();
        if (!player)
            return false;

        std::vector<std::string_view> tokens = Acore::Tokenize(args ? args : "", ' ', false);
        if (tokens.size() != 3)
            return HandlePlayerHelp(h);

        auto difficultyId = Acore::StringTo<uint32>(tokens[0]);
        auto themeId = Acore::StringTo<uint32>(tokens[1]);
        bool scaleToParty = true;

        if (!difficultyId || !themeId || !ParseScaleMode(tokens[2], scaleToParty))
        {
            h->SendSysMessage("DungeonMaster: invalid arguments. Usage: .dm roguelike <difficultyId> <themeId> <party|tier>");
            return false;
        }

        PlayerDMSelection selection;
        selection.DifficultyId = *difficultyId;
        selection.ThemeId = *themeId;
        selection.MapId = 0;
        selection.ScaleToParty = scaleToParty;
        selection.IsRoguelike = true;
        return StartRoguelikeFromSelection(player, selection, true);
    }

    static bool HandleReload(ChatHandler* h)
    {
        sDMConfig->LoadConfig(true);
        h->SendSysMessage("DungeonMaster: Configuration reloaded.");
        return true;
    }

    static bool HandleStatus(ChatHandler* h)
    {
        char buf[256];
        h->SendSysMessage("=== Dungeon Master Status ===");
        snprintf(buf, sizeof(buf), "Enabled: %s", sDMConfig->IsEnabled() ? "Yes" : "No");
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Active: %u / %u",
            sDungeonMasterMgr->GetActiveSessionCount(), sDMConfig->GetMaxConcurrentRuns());
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Level Band: +/-%u", sDMConfig->GetLevelBand());
        h->SendSysMessage(buf);
        snprintf(buf, sizeof(buf), "Difficulties: %u  Themes: %u  Dungeons: %u",
            uint32(sDMConfig->GetDifficulties().size()),
            uint32(sDMConfig->GetThemes().size()),
            uint32(sDMConfig->GetDungeons().size()));
        h->SendSysMessage(buf);
        return true;
    }

    static bool HandleList(ChatHandler* h)
    {
        uint32 n = sDungeonMasterMgr->GetActiveSessionCount();
        char buf[128];
        snprintf(buf, sizeof(buf), "Active DM sessions: %u", n);
        h->SendSysMessage(buf);
        return true;
    }

    static bool HandleEnd(ChatHandler* h, Optional<uint32> sessionId)
    {
        char buf[128];
        if (sessionId)
        {
            Session* s = sDungeonMasterMgr->GetSession(*sessionId);
            if (!s) { snprintf(buf, sizeof(buf), "Session %u not found.", *sessionId); h->SendSysMessage(buf); return false; }
            sDungeonMasterMgr->EndSession(*sessionId, false);
            snprintf(buf, sizeof(buf), "Session %u ended.", *sessionId); h->SendSysMessage(buf);
        }
        else
        {
            // Try the invoker's own session first
            Player* invoker = h->GetSession() ? h->GetSession()->GetPlayer() : nullptr;
            Session* s = invoker ? sDungeonMasterMgr->GetSessionByPlayer(invoker->GetGUID()) : nullptr;

            // Fall back to selected player's session
            if (!s)
            {
                Player* t = h->getSelectedPlayer();
                s = t ? sDungeonMasterMgr->GetSessionByPlayer(t->GetGUID()) : nullptr;
            }

            if (!s) { h->SendSysMessage("Not in a DM session. Select a player or provide session ID."); return false; }
            uint32 id = s->SessionId;
            sDungeonMasterMgr->EndSession(id, false);
            snprintf(buf, sizeof(buf), "Session %u ended (all players teleported out).", id); h->SendSysMessage(buf);
        }
        return true;
    }

    static bool HandleClearCD(ChatHandler* h)
    {
        Player* invoker = h->GetSession() ? h->GetSession()->GetPlayer() : nullptr;
        if (!invoker) { h->SendSysMessage("In-game only."); return false; }

        // If invoker is in a group, clear cooldown for ALL group members
        Group* g = invoker->GetGroup();
        if (g)
        {
            uint32 cleared = 0;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member)
                {
                    sDungeonMasterMgr->ClearCooldown(member->GetGUID());
                    ++cleared;
                }
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "Cooldown cleared for %u group member(s).", cleared);
            h->SendSysMessage(buf);
        }
        else
        {
            // Solo — clear for self or selected player
            Player* t = h->getSelectedPlayer();
            if (!t) t = invoker;
            sDungeonMasterMgr->ClearCooldown(t->GetGUID());
            char buf[128];
            snprintf(buf, sizeof(buf), "Cooldown cleared for %s.", t->GetName().c_str());
            h->SendSysMessage(buf);
        }
        return true;
    }
};

void AddSC_dm_command_script()
{
    new dm_command_script();
}

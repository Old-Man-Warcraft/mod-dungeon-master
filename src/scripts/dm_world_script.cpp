/*
 * mod-dungeon-master — dm_world_script.cpp
 * Server lifecycle hooks: config load, startup, update tick, shutdown.
 */

#include "ScriptMgr.h"
#include "DungeonMasterMgr.h"
#include "RoguelikeMgr.h"
#include "DMConfig.h"
#include "Log.h"

using namespace DungeonMaster;

namespace
{
constexpr char const* DM_LOG_CATEGORY = "module.DungeonMaster";
}

class dm_world_script : public WorldScript
{
public:
    dm_world_script() : WorldScript("dm_world_script") {}

    void OnAfterConfigLoad(bool reload) override
    {
        sDMConfig->LoadConfig(reload);
    }

    void OnStartup() override
    {
        if (!sDMConfig->IsEnabled())
        {
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Disabled in configuration.");
            return;
        }

        // Do NOT patch SpellInfo for 25898 (Greater Blessing of Kings). A global
        // StackAmount > 0 makes Unit::_TryStackingOrRefreshingExistingAura use
        // Aura::ModStackAmount(1) on refresh; for non-stacking spells the core
        // only clamps to 1 stack when DBC StackAmount is 0, so refreshes would
        // incorrectly climb into multi-stack "Greater Blessing of Kings".

        sDungeonMasterMgr->Initialize();
        sRoguelikeMgr->Initialize();

        LOG_INFO(DM_LOG_CATEGORY, "===============================================");
        LOG_INFO(DM_LOG_CATEGORY, " Dungeon Master Module — Ready");
        LOG_INFO(DM_LOG_CATEGORY, " {} difficulties | {} themes | {} dungeons",
            sDMConfig->GetDifficulties().size(),
            sDMConfig->GetThemes().size(),
            sDMConfig->GetDungeons().size());
        LOG_INFO(DM_LOG_CATEGORY, " Level band: +/-{} | Max concurrent: {}",
            sDMConfig->GetLevelBand(), sDMConfig->GetMaxConcurrentRuns());
        LOG_INFO(DM_LOG_CATEGORY, "===============================================");
    }

    void OnShutdown() override
    {
        if (!sDMConfig->IsEnabled()) return;
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Shutdown — {} sessions active.",
            sDungeonMasterMgr->GetActiveSessionCount());
    }

    void OnUpdate(uint32 diff) override
    {
        if (sDMConfig->IsEnabled())
        {
            sDungeonMasterMgr->Update(diff);
            sRoguelikeMgr->Update(diff);
        }
    }
};

void AddSC_dm_world_script()
{
    new dm_world_script();
}

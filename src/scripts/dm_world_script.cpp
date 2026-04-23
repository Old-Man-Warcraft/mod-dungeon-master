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
            LOG_INFO("module", "DungeonMaster: Disabled in configuration.");
            return;
        }

        // Do NOT patch SpellInfo for 25898 (Greater Blessing of Kings). A global
        // StackAmount > 0 makes Unit::_TryStackingOrRefreshingExistingAura use
        // Aura::ModStackAmount(1) on refresh; for non-stacking spells the core
        // only clamps to 1 stack when DBC StackAmount is 0, so refreshes would
        // incorrectly climb into multi-stack "Greater Blessing of Kings".

        sDungeonMasterMgr->Initialize();
        sRoguelikeMgr->Initialize();

        LOG_INFO("module", "===============================================");
        LOG_INFO("module", " Dungeon Master Module — Ready");
        LOG_INFO("module", " {} difficulties | {} themes | {} dungeons",
            sDMConfig->GetDifficulties().size(),
            sDMConfig->GetThemes().size(),
            sDMConfig->GetDungeons().size());
        LOG_INFO("module", " Level band: +/-{} | Max concurrent: {}",
            sDMConfig->GetLevelBand(), sDMConfig->GetMaxConcurrentRuns());
        LOG_INFO("module", "===============================================");
    }

    void OnShutdown() override
    {
        if (!sDMConfig->IsEnabled()) return;
        LOG_INFO("module", "DungeonMaster: Shutdown — {} sessions active.",
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

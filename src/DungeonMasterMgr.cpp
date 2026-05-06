/*
 * mod-dungeon-master — DungeonMasterMgr.cpp
 * Central session manager: creature pools, spawning, scaling, rewards, update loop.
 */

#include "DungeonMasterMgr.h"
#include "RoguelikeMgr.h"
#include "DMConfig.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Map.h"
#include "MapMgr.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "WorldSession.h"
#include "World.h"
#include "Chat.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Mail.h"
#include "Item.h"
#include "LootMgr.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "InstanceScript.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include <random>
#include <algorithm>
#include <set>
#include <unordered_set>
#include <cstdio>
#include <cmath>
#include <limits>

namespace
{
constexpr char const* DM_LOG_CATEGORY = "module.DungeonMaster";
constexpr float PHASE_DETECTION_RADIUS = 20.0f;

struct DungeonEntranceLocation
{
    uint32 TriggerId = 0;
    std::string Name;
    Position Pos;
};

static float GetDistanceSq(float ax, float ay, float az, float bx, float by, float bz)
{
    float dx = ax - bx;
    float dy = ay - by;
    float dz = az - bz;
    return dx * dx + dy * dy + dz * dz;
}

static std::vector<DungeonEntranceLocation> LoadDungeonEntrancesForMap(uint32 mapId)
{
    std::vector<DungeonEntranceLocation> entrances;

    char q[256];
    snprintf(q, sizeof(q),
        "SELECT id, name, target_position_x, target_position_y, target_position_z, target_orientation "
        "FROM areatrigger_teleport WHERE target_map = %u ORDER BY id",
        mapId);

    QueryResult result = WorldDatabase.Query(q);
    if (!result)
        return entrances;

    do
    {
        Field* f = result->Fetch();

        DungeonEntranceLocation entrance;
        entrance.TriggerId = f[0].Get<uint32>();
        entrance.Name = f[1].Get<std::string>();
        entrance.Pos.Relocate(
            f[2].Get<float>(),
            f[3].Get<float>(),
            f[4].Get<float>(),
            f[5].Get<float>());

        entrances.push_back(entrance);
    } while (result->NextRow());

    return entrances;
}

static bool BelongsToEntranceCluster(std::vector<DungeonEntranceLocation> const& entrances,
    size_t selectedIndex, float x, float y, float z)
{
    if (entrances.size() <= 1 || selectedIndex >= entrances.size())
        return true;

    DungeonEntranceLocation const& selected = entrances[selectedIndex];
    float selectedDistSq = GetDistanceSq(x, y, z,
        selected.Pos.GetPositionX(), selected.Pos.GetPositionY(), selected.Pos.GetPositionZ());

    for (size_t i = 0; i < entrances.size(); ++i)
    {
        if (i == selectedIndex)
            continue;

        DungeonEntranceLocation const& other = entrances[i];
        float otherDistSq = GetDistanceSq(x, y, z,
            other.Pos.GetPositionX(), other.Pos.GetPositionY(), other.Pos.GetPositionZ());

        if (otherDistSq + 1.0f < selectedDistSq)
            return false;
    }

    return true;
}
}

namespace DungeonMaster
{

// RNG helpers (thread-local for safety)
static thread_local std::mt19937 tRng{ std::random_device{}() };

template<typename T>
static T RandInt(T lo, T hi) { return std::uniform_int_distribution<T>(lo, hi)(tRng); }

// Aggressive AI for DM-spawned creatures; patrols 5 yd radius, active aggro, hooks JustDied for loot
class DungeonMasterCreatureAI : public CreatureAI
{
public:
    explicit DungeonMasterCreatureAI(Creature* creature)
        : CreatureAI(creature), _patrolStarted(false), _aggroScanTimer(0) {}

    // Active aggro detection — overrides the default which has many silent skips
    void MoveInLineOfSight(Unit* who) override
    {
        if (!who || !me->IsAlive() || me->IsInCombat() || me->HasReactState(REACT_PASSIVE))
            return;

        if (who->GetTypeId() != TYPEID_PLAYER)
            return;

        Player* player = who->ToPlayer();
        if (!player || !player->IsAlive() || player->IsGameMaster())
            return;

        float aggroRange = sDMConfig->GetAggroRadius();
        if (me->IsWithinDistInMap(player, aggroRange)
            && me->IsHostileTo(player)
            && me->IsWithinLOSInMap(player))
        {
            me->SetInCombatWith(player);
            player->SetInCombatWith(me);
            me->AddThreat(player, 1.0f);
            AttackStart(player);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
        {
            // Start random patrol movement when idle
            if (!_patrolStarted && me->IsAlive())
            {
                me->GetMotionMaster()->MoveRandom(5.0f);
                _patrolStarted = true;
            }

            // Fallback aggro scan every 1 second for cases where MoveInLineOfSight
            // doesn't fire (inactive grids, summoned creature edge cases)
            _aggroScanTimer += diff;
            if (_aggroScanTimer >= 1000 && me->IsAlive())
            {
                _aggroScanTimer = 0;
                float aggroRange = sDMConfig->GetAggroRadius();

                Map::PlayerList const& players = me->GetMap()->GetPlayers();
                float closest = aggroRange;
                Player* target = nullptr;

                for (auto const& itr : players)
                {
                    Player* p = itr.GetSource();
                    if (!p || !p->IsAlive() || p->IsGameMaster())
                        continue;

                    float dist = me->GetDistance(p);
                    if (dist < closest
                        && me->IsHostileTo(p)
                        && me->IsWithinLOSInMap(p))
                    {
                        closest = dist;
                        target = p;
                    }
                }

                if (target)
                {
                    me->SetInCombatWith(target);
                    target->SetInCombatWith(me);
                    me->AddThreat(target, 1.0f);
                    AttackStart(target);
                }
            }
            return;
        }
        DoMeleeAttackIfReady();
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _patrolStarted = false;
        CreatureAI::EnterEvadeMode();
    }

    void JustDied(Unit* killer) override
    {
        CreatureAI::JustDied(killer);
        // NOTE: Do NOT call FillCreatureLoot here.  JustDied fires inside
        // setDeathState / Unit::Kill, and the core will clear creature->loot
        // and remove UNIT_DYNFLAG_LOOTABLE after we return.  Loot is filled
        // later from HandleCreatureDeath (OnUnitDeath hook) which fires
        // AFTER the core's death processing is finished.
        sDungeonMasterMgr->OnCreatureDeathHook(me);
    }

private:
    bool   _patrolStarted;
    uint32 _aggroScanTimer;
};

// ---------------------------------------------------------------------------


// Session helper implementations (declared in DMTypes.h)
uint32 Session::GetAlivePlayerCount() const
{
    uint32 n = 0;
    for (const auto& pd : Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsAlive()) ++n;
    }
    return n;
}

bool Session::IsPartyWiped()    const { return GetAlivePlayerCount() == 0; }

bool Session::IsGroupInCombat() const
{
    for (const auto& pd : Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsAlive() && p->IsInCombat())
            return true;
    }
    return false;
}

Player* DungeonMasterMgr::GetReferencePlayer(Session const& session) const
{
    for (auto const& pd : session.Players)
    {
        Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (player && player->IsInWorld() && player->GetMapId() == session.MapId)
            return player;
    }

    for (auto const& pd : session.Players)
    {
        Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (player && player->IsInWorld())
            return player;
    }

    return nullptr;
}

PendingPhaseCheck DungeonMasterMgr::CreatePendingPhaseCheck(Session const& session,
    Creature const* creature, Player* refPlayer, uint32 origEntry) const
{
    PendingPhaseCheck ppc;
    ppc.DeathTime = GameTime::GetGameTime().count();
    ppc.OrigEntry = origEntry;
    ppc.Resolved = false;

    if (!creature)
        return ppc;

    ppc.DeathPos = { creature->GetPositionX(), creature->GetPositionY(),
                     creature->GetPositionZ(), creature->GetOrientation() };

    if (!refPlayer)
        refPlayer = GetReferencePlayer(session);

    if (!refPlayer || !refPlayer->IsInWorld())
        return ppc;

    std::set<ObjectGuid> trackedGuids;
    for (auto const& sc : session.SpawnedCreatures)
        trackedGuids.insert(sc.Guid);

    std::list<Creature*> nearby;
    refPlayer->GetCreatureListWithEntryInGrid(nearby, 0, 5000.0f);

    for (Creature* candidate : nearby)
    {
        if (!candidate || !candidate->IsAlive() || candidate->IsPet() || candidate->IsGuardian() || candidate->IsTotem())
            continue;
        if (candidate->GetEntry() == sDMConfig->GetNpcEntry())
            continue;
        if (trackedGuids.count(candidate->GetGUID()) > 0)
            continue;

        const CreatureTemplate* tmpl = candidate->GetCreatureTemplate();
        if (!tmpl || (tmpl->rank != 1 && tmpl->rank != 2 && tmpl->rank != 4))
            continue;

        float distSq = GetDistanceSq(candidate->GetPositionX(), candidate->GetPositionY(), candidate->GetPositionZ(),
            ppc.DeathPos.GetPositionX(), ppc.DeathPos.GetPositionY(), ppc.DeathPos.GetPositionZ());
        if (distSq > PHASE_DETECTION_RADIUS * PHASE_DETECTION_RADIUS)
            continue;

        ppc.NearbyEliteGuids.push_back(candidate->GetGUID());
    }

    return ppc;
}

// Singleton
DungeonMasterMgr::DungeonMasterMgr()  = default;
DungeonMasterMgr::~DungeonMasterMgr() = default;

DungeonMasterMgr* DungeonMasterMgr::Instance()
{
    static DungeonMasterMgr inst;
    return &inst;
}

// Initialization
void DungeonMasterMgr::Initialize()
{
    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Initializing...");
    LoadFromDB();
    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Ready — {} creature types, {} bosses, {} dungeon bosses, {} reward items, {} loot items.",
        _creaturesByType.size(), _bossCreatures.size(), _dungeonBossPool.size(), _rewardItems.size(), _lootPool.size());
}

void DungeonMasterMgr::LoadFromDB()
{
    LoadCreaturePools();
    LoadDungeonBossPool();
    LoadDungeonSelectionMetrics();
    LoadDungeonThemeProfiles();
    LoadClassLevelStats();
    LoadRewardItems();
    LoadLootPool();
    LoadAllPlayerStats();
}

// Load creature pools from world DB, split into trash (rank 0) and boss (rank 1/2/4)
void DungeonMasterMgr::LoadCreaturePools()
{
    _creaturesByType.clear();
    _bossCreatures.clear();

    // Type for theming, rank for boss/trash split, ScriptName='' to avoid scripted mobs
    QueryResult result = WorldDatabase.Query(
        "SELECT ct.entry, ct.type, ct.minlevel, ct.maxlevel, ct.`rank` "
        "FROM creature_template ct "
        "LEFT JOIN creature_template_movement ctm ON ct.entry = ctm.CreatureId "
        "WHERE ct.type > 0 AND ct.type <= 10 AND ct.type != 8 "       // combat types, skip Critter
        "AND ct.minlevel > 0 AND ct.maxlevel <= 83 "
        "AND ct.`rank` != 3 "                                          // not World Boss
        "AND (ctm.Ground IS NULL OR ctm.Ground != 0) "                // no water-only creatures
        "AND ct.VehicleId = 0 "                                        // not a vehicle/chair/cannon
        "AND ct.ScriptName = '' "                                      // no C++ scripts (they override our scaling)
        "AND ct.npcflag = 0 "                                          // no vendors/quest givers/gossip NPCs
        "AND (ct.unit_flags & 2) = 0 "                                 // no NON_ATTACKABLE
        "AND (ct.subname = '' OR ct.subname IS NULL) "                 // no guild/title text under name
        "AND ct.name NOT LIKE '%[UNUSED]%' "
        "AND ct.name NOT LIKE '%[PH]%' "
        "AND ct.name NOT LIKE '%Test %' "
        "AND ct.name NOT LIKE '%Test_%' "
        "AND ct.name NOT LIKE '%DVREF%' "
        "AND ct.name NOT LIKE '%[DNT]%' "
        "AND ct.name NOT LIKE '%Trigger%' "
        "AND ct.name NOT LIKE '%Invisible%' "
        "AND ct.name NOT LIKE '%Dummy%' "
        "AND ct.name NOT LIKE '%(%' "                                  // skip (1), (2) variant entries
        "AND ct.name NOT LIKE '%Debug%' "
        "AND ct.name NOT LIKE '%Template%' "
        "AND ct.name NOT LIKE '%Copy of%' "
        "AND ct.name NOT LIKE '% - DNT' "
        "AND ct.name NOT LIKE '%Placeholder%' "
        "AND ct.name NOT LIKE '%Visual%' "
        "AND ct.name NOT LIKE '%Server%' "
        "AND ct.name NOT LIKE '%Quest%' "                              // quest scripted mobs
        "AND ct.name NOT LIKE '%zzOLD%' "
        "ORDER BY ct.type, ct.minlevel");

    if (!result)
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: creature_template query returned NO results — check your world DB!");
        return;
    }

    uint32 trashCount = 0, bossCount = 0;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            CreaturePoolEntry e;
            e.Entry    = f[0].Get<uint32>();
            e.Type     = f[1].Get<uint32>();
            e.MinLevel = f[2].Get<uint8>();
            e.MaxLevel = f[3].Get<uint8>();
            uint8 rank = f[4].Get<uint8>();

            if (rank == 1 || rank == 2 || rank == 4)        // elite / rare-elite → boss pool
            {
                _bossCreatures[e.Type].push_back(e);
                ++bossCount;
            }
            else                                              // normal (rank 0) → trash pool
            {
                _creaturesByType[e.Type].push_back(e);
                ++trashCount;
            }
        } while (result->NextRow());
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Loaded {} trash creatures, {} potential bosses.",
        trashCount, bossCount);


    static const char* typeNames[] = {
        "None", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical", "NotSpecified"
    };
    for (const auto& [type, vec] : _creaturesByType)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster:   Trash type {} ({}): {} entries",
            type, name, vec.size());
    }
    for (const auto& [type, vec] : _bossCreatures)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster:   Boss  type {} ({}): {} entries",
            type, name, vec.size());
    }
}

// Load real dungeon bosses (scripted elites from all dungeon maps)
void DungeonMasterMgr::LoadDungeonBossPool()
{
    _dungeonBossPool.clear();
    _dungeonBossPoolByMap.clear();
    std::unordered_map<uint32, std::unordered_set<uint32>> globalBossEntriesByType;

    // Build comma-separated list of all dungeon map IDs
    const auto& dungeons = sDMConfig->GetDungeons();
    if (dungeons.empty())
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No dungeons configured — dungeon boss pool empty.");
        return;
    }

    std::string mapList;
    for (size_t i = 0; i < dungeons.size(); ++i)
    {
        if (i > 0) mapList += ",";
        mapList += std::to_string(dungeons[i].MapId);
    }

    // Query for scripted elite creatures that spawn in dungeon maps.
    char query[2048];
    snprintf(query, sizeof(query),
        "SELECT DISTINCT c.map, ct.entry, ct.name, ct.type, ct.minlevel, ct.maxlevel "
        "FROM creature_template ct "
        "JOIN creature c ON c.id1 = ct.entry "
        "LEFT JOIN creature_template_movement ctm ON ct.entry = ctm.CreatureId "
        "WHERE c.map IN (%s) "
        "AND ct.`rank` IN (1, 2) "
        "AND ct.ScriptName != '' "
        "AND ct.type > 0 AND ct.type <= 10 "
        "AND ct.minlevel > 0 "
        "AND ct.VehicleId = 0 "
        "AND (ctm.Ground IS NULL OR ctm.Ground != 0) "  // no water-only creatures
        "AND (ct.unit_flags & 2) = 0 "       // not NON_ATTACKABLE
        "AND ct.name NOT LIKE '%%Trigger%%' "
        "AND ct.name NOT LIKE '%%Invisible%%' "
        "AND ct.name NOT LIKE '%%Dummy%%' "
        "AND ct.name NOT LIKE '%%Visual%%' "
        "AND ct.name NOT LIKE '%%Server%%' "
        "ORDER BY ct.type, ct.minlevel",
        mapList.c_str());

    QueryResult result = WorldDatabase.Query(query);

    if (!result)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Dungeon boss pool query returned no results.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        CreaturePoolEntry e;
        uint32 mapId = f[0].Get<uint32>();
        e.Entry    = f[1].Get<uint32>();
        // f[2] = name (for logging only)
        e.Type     = f[3].Get<uint32>();
        e.MinLevel = f[4].Get<uint8>();
        e.MaxLevel = f[5].Get<uint8>();

        if (globalBossEntriesByType[e.Type].insert(e.Entry).second)
            _dungeonBossPool[e.Type].push_back(e);
        _dungeonBossPoolByMap[mapId][e.Type].push_back(e);
        ++count;

        LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Dungeon boss: {} (entry {}, map {}, type {}, level {}-{})",
            f[2].Get<std::string>(), e.Entry, mapId, e.Type, e.MinLevel, e.MaxLevel);
    } while (result->NextRow());

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Loaded {} real dungeon bosses into boss pool.", count);

    static const char* typeNames[] = {
        "None", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical", "NotSpecified"
    };
    for (const auto& [type, vec] : _dungeonBossPool)
    {
        const char* name = (type <= 10) ? typeNames[type] : "Unknown";
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster:   Dungeon boss type {} ({}): {} entries",
            type, name, vec.size());
    }

    for (const auto& [mapId, typePools] : _dungeonBossPoolByMap)
    {
        size_t mapBossCount = 0;
        for (const auto& [type, vec] : typePools)
            mapBossCount += vec.size();

        if (const DungeonInfo* dungeon = sDMConfig->GetDungeon(mapId))
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster:   Map-native boss pool {} ({}) -> {} candidates",
                mapId, dungeon->Name, mapBossCount);
        else
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster:   Map-native boss pool {} -> {} candidates",
                mapId, mapBossCount);
    }
}

void DungeonMasterMgr::LoadDungeonSelectionMetrics()
{
    _dungeonSelectionMetrics.clear();

    for (DungeonInfo const& dungeon : sDMConfig->GetDungeons())
    {
        DungeonSelectionMetrics metrics;

        auto mapBossPoolIt = _dungeonBossPoolByMap.find(dungeon.MapId);
        if (mapBossPoolIt != _dungeonBossPoolByMap.end())
            for (auto const& [type, bosses] : mapBossPoolIt->second)
                metrics.NativeBossCount += bosses.size();

        metrics.EntranceCount = LoadDungeonEntrancesForMap(dungeon.MapId).size();
        _dungeonSelectionMetrics[dungeon.MapId] = metrics;
    }

    if (sDMConfig->GetDungeons().empty())
        return;

    std::string mapList;
    for (size_t i = 0; i < sDMConfig->GetDungeons().size(); ++i)
    {
        if (i > 0)
            mapList += ",";
        mapList += std::to_string(sDMConfig->GetDungeons()[i].MapId);
    }

    char query[1024];
    snprintf(query, sizeof(query),
        "SELECT map, COUNT(*) FROM creature WHERE map IN (%s) GROUP BY map",
        mapList.c_str());

    QueryResult result = WorldDatabase.Query(query);
    if (!result)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Dungeon selection metrics query returned no creature counts.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 mapId = fields[0].Get<uint32>();
        uint32 spawnPoints = fields[1].Get<uint32>();
        _dungeonSelectionMetrics[mapId].SpawnPointCount = spawnPoints;
    } while (result->NextRow());

    for (auto const& [mapId, metrics] : _dungeonSelectionMetrics)
    {
        if (DungeonInfo const* dungeon = sDMConfig->GetDungeon(mapId))
            LOG_INFO(DM_LOG_CATEGORY,
                "DungeonMaster: Selection metrics {} ({}) -> spawns={}, nativeBosses={}, entrances={}",
                mapId, dungeon->Name, metrics.SpawnPointCount, metrics.NativeBossCount, metrics.EntranceCount);
    }
}

void DungeonMasterMgr::LoadDungeonThemeProfiles()
{
    _dungeonThemeProfiles.clear();

    if (sDMConfig->GetDungeons().empty())
        return;

    std::string mapList;
    for (size_t i = 0; i < sDMConfig->GetDungeons().size(); ++i)
    {
        if (i > 0)
            mapList += ",";
        mapList += std::to_string(sDMConfig->GetDungeons()[i].MapId);
    }

    char query[2048];
    snprintf(query, sizeof(query),
        "SELECT c.map, ct.type, COUNT(*) "
        "FROM creature c "
        "JOIN creature_template ct ON c.id1 = ct.entry "
        "LEFT JOIN creature_template_movement ctm ON ct.entry = ctm.CreatureId "
        "WHERE c.map IN (%s) "
        "AND ct.type > 0 AND ct.type <= 10 AND ct.type != 8 "
        "AND ct.VehicleId = 0 "
        "AND (ctm.Ground IS NULL OR ctm.Ground != 0) "
        "GROUP BY c.map, ct.type",
        mapList.c_str());

    QueryResult result = WorldDatabase.Query(query);
    if (!result)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Dungeon theme profile query returned no results.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 mapId = fields[0].Get<uint32>();
        uint32 type = fields[1].Get<uint32>();
        uint32 count = fields[2].Get<uint32>();

        _dungeonThemeProfiles[mapId].NativeTypeCounts[type] = count;
    } while (result->NextRow());

    for (auto const& [mapId, profile] : _dungeonThemeProfiles)
    {
        std::vector<std::pair<uint32, uint32>> dominantTypes(profile.NativeTypeCounts.begin(), profile.NativeTypeCounts.end());
        std::sort(dominantTypes.begin(), dominantTypes.end(),
            [](auto const& left, auto const& right)
            {
                return left.second > right.second;
            });

        std::string summary;
        for (size_t i = 0; i < std::min<size_t>(3, dominantTypes.size()); ++i)
        {
            if (!summary.empty())
                summary += ", ";

            summary += Acore::StringFormat("type {} -> {}", dominantTypes[i].first, dominantTypes[i].second);
        }

        if (DungeonInfo const* dungeon = sDMConfig->GetDungeon(mapId))
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Theme profile {} ({}) -> {}",
                mapId, dungeon->Name, summary.empty() ? "no dominant types" : summary);
    }
}

uint32 DungeonMasterMgr::CountMapBossCandidates(uint32 mapId, Theme const* theme, uint8 bandMin, uint8 bandMax) const
{
    auto mapIt = _dungeonBossPoolByMap.find(mapId);
    if (mapIt == _dungeonBossPoolByMap.end())
        return 0;

    std::set<uint32> allowedTypes;
    bool anyType = !theme;
    if (theme)
    {
        for (uint32 type : theme->CreatureTypes)
        {
            if (type == uint32(-1))
            {
                anyType = true;
                break;
            }

            allowedTypes.insert(type);
        }
    }

    auto typeAllowed = [&](uint32 type) -> bool
    {
        return anyType || allowedTypes.count(type) > 0;
    };

    auto overlapsBand = [&](CreaturePoolEntry const& entry) -> bool
    {
        return entry.MinLevel <= bandMax && entry.MaxLevel >= bandMin;
    };

    uint32 count = 0;
    for (auto const& [type, bosses] : mapIt->second)
    {
        if (!typeAllowed(type))
            continue;

        for (CreaturePoolEntry const& boss : bosses)
            if (overlapsBand(boss))
                ++count;
    }

    return count;
}

uint32 DungeonMasterMgr::SelectWeightedDungeon(uint32 difficultyId, uint32 themeId, uint32 previousMapId,
    uint8 bandMin, uint8 bandMax) const
{
    DifficultyTier const* difficulty = sDMConfig->GetDifficulty(difficultyId);
    uint8 minLevel = difficulty ? difficulty->MinLevel : 1;
    uint8 maxLevel = difficulty ? difficulty->MaxLevel : 80;
    bool usingExplicitBand = bandMin != 0 && bandMax != 0 && bandMin <= bandMax;
    if (usingExplicitBand)
    {
        minLevel = bandMin;
        maxLevel = bandMax;
    }

    Theme const* theme = sDMConfig->GetTheme(themeId);

    std::vector<DungeonInfo const*> dungeons = sDMConfig->GetDungeonsForLevel(minLevel, maxLevel);
    if (dungeons.empty())
        return 0;

    struct WeightedDungeonCandidate
    {
        uint32 MapId = 0;
        std::string Name;
        double Weight = 0.0;
        uint32 SpawnPoints = 0;
        uint32 NativeBossCandidates = 0;
    };

    bool anyNativeBossMap = false;
    if (usingExplicitBand)
    {
        for (DungeonInfo const* dungeon : dungeons)
        {
            if (!dungeon)
                continue;

            if (CountMapBossCandidates(dungeon->MapId, theme, minLevel, maxLevel) > 0)
            {
                anyNativeBossMap = true;
                break;
            }
        }
    }

    std::vector<WeightedDungeonCandidate> candidates;
    candidates.reserve(dungeons.size());

    uint32 filteredBosslessMaps = 0;

    double totalWeight = 0.0;
    for (DungeonInfo const* dungeon : dungeons)
    {
        if (!dungeon)
            continue;

        DungeonSelectionMetrics metrics;
        if (auto metricsIt = _dungeonSelectionMetrics.find(dungeon->MapId); metricsIt != _dungeonSelectionMetrics.end())
            metrics = metricsIt->second;

        uint32 spawnPoints = metrics.SpawnPointCount > 0 ? metrics.SpawnPointCount : 80;
        uint32 nativeBossCandidates = CountMapBossCandidates(dungeon->MapId, theme, minLevel, maxLevel);

        if (usingExplicitBand && anyNativeBossMap && nativeBossCandidates == 0)
        {
            ++filteredBosslessMaps;
            continue;
        }

        double sizeWeight = 1.0;
        if (spawnPoints < 40)
            sizeWeight = 0.70;
        else if (spawnPoints <= 220)
            sizeWeight = 1.30;
        else if (spawnPoints <= 420)
            sizeWeight = 1.00;
        else if (spawnPoints <= 700)
            sizeWeight = 0.72;
        else
            sizeWeight = 0.48;

        double bossWeight = 1.0 + std::min<uint32>(nativeBossCandidates, 6u) * 0.35;
        double entranceWeight = metrics.EntranceCount > 1 ? 0.95 : 1.05;
        double repeatWeight = (previousMapId != 0 && dungeon->MapId == previousMapId) ? 0.10 : 1.0;
        double finalWeight = std::max(0.01, sizeWeight * bossWeight * entranceWeight * repeatWeight);

        totalWeight += finalWeight;
        candidates.push_back({ dungeon->MapId, dungeon->Name, finalWeight, spawnPoints, nativeBossCandidates });
    }

    if (candidates.empty())
        return 0;

    if (filteredBosslessMaps > 0)
        LOG_INFO(DM_LOG_CATEGORY,
            "DungeonMaster: Weighted dungeon selection filtered {} bossless map(s) for exact band {}-{} and theme '{}' (previousMap={}).",
            filteredBosslessMaps, minLevel, maxLevel, theme ? theme->Name : "Random", previousMapId);

    if (totalWeight <= 0.0)
        return candidates[RandInt<size_t>(0, candidates.size() - 1)].MapId;

    std::sort(candidates.begin(), candidates.end(),
        [](WeightedDungeonCandidate const& left, WeightedDungeonCandidate const& right)
        {
            return left.Weight > right.Weight;
        });

    double roll = std::uniform_real_distribution<double>(0.0, totalWeight)(tRng);
    uint32 selectedMapId = candidates.back().MapId;
    for (WeightedDungeonCandidate const& candidate : candidates)
    {
        if (roll <= candidate.Weight)
        {
            selectedMapId = candidate.MapId;
            break;
        }

        roll -= candidate.Weight;
    }

    std::string themeName = theme ? theme->Name : "Random";
    for (size_t i = 0; i < std::min<size_t>(3, candidates.size()); ++i)
    {
        WeightedDungeonCandidate const& candidate = candidates[i];
        LOG_DEBUG(DM_LOG_CATEGORY,
            "DungeonMaster: Weighted dungeon candidate #{} -> {} ({}) weight={:.2f}, spawns={}, nativeBossCandidates={}, previousMap={}",
            i + 1, candidate.Name, candidate.MapId, candidate.Weight, candidate.SpawnPoints,
            candidate.NativeBossCandidates, previousMapId);
    }

    if (DungeonInfo const* selected = sDMConfig->GetDungeon(selectedMapId))
        LOG_INFO(DM_LOG_CATEGORY,
            "DungeonMaster: Weighted dungeon selection -> {} ({}) for difficulty {}, theme '{}', band {}-{}, previousMap={}",
            selected->Name, selectedMapId, difficultyId, themeName, minLevel, maxLevel, previousMapId);

    return selectedMapId;
}

// Cache creature_classlevelstats for force-scaling
void DungeonMasterMgr::LoadClassLevelStats()
{
    _classLevelStats.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT level, class, basehp0, damage_base, basearmor, attackpower "
        "FROM creature_classlevelstats "
        "WHERE level > 0 AND level <= 83 "
        "ORDER BY class, level");

    if (!result)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: creature_classlevelstats not found — "
                 "creature scaling will use template defaults.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        uint8  level     = f[0].Get<uint8>();
        uint8  unitClass = f[1].Get<uint8>();
        ClassLevelStatEntry e;
        e.BaseHP       = std::max(1u, f[2].Get<uint32>());
        e.BaseDamage   = std::max(1.0f, f[3].Get<float>());
        e.BaseArmor    = f[4].Get<uint32>();
        e.AttackPower  = f[5].Get<uint32>();
        _classLevelStats[{unitClass, level}] = e;
        ++count;
    } while (result->NextRow());

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: {} class-level stat entries cached.", count);
}

// Look up cached base stats
const ClassLevelStatEntry* DungeonMasterMgr::GetBaseStatsForLevel(
    uint8 unitClass, uint8 level) const
{
    auto it = _classLevelStats.find({unitClass, level});
    if (it != _classLevelStats.end())
        return &it->second;

    // Fallback: try Warrior (class 1) at this level
    it = _classLevelStats.find({1, level});
    if (it != _classLevelStats.end())
        return &it->second;

    return nullptr;
}

// Cache equippable reward items (green/blue/purple)
void DungeonMasterMgr::LoadRewardItems()
{
    _rewardItems.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT entry, RequiredLevel, Quality, InventoryType, class, subclass, "
        "AllowableClass, ItemLevel "
        "FROM item_template "
        "WHERE Quality >= 2 AND Quality <= 4 "
        "AND RequiredLevel > 0 AND RequiredLevel <= 80 "
        "AND InventoryType > 0 AND InventoryType <= 26 "
        "AND InventoryType NOT IN (18, 19, 24) "
        "AND class IN (2, 4) AND (Flags & 0x8) = 0 "
        "AND AllowableClass != 0 "
        "AND RequiredReputationFaction = 0 "
        "AND RequiredHonorRank = 0 "
        "AND name NOT LIKE '%Test%' "
        "AND name NOT LIKE '%Deprecated%' "
        "AND name NOT LIKE '%[PH]%' "
        "AND name NOT LIKE '%OLD%' "
        "AND name NOT LIKE '%Monster -%' "
        "AND name NOT LIKE '%zzOLD%' "
        "ORDER BY RequiredLevel, Quality");

    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            RewardItem ri;
            ri.Entry         = f[0].Get<uint32>();
            ri.MinLevel      = f[1].Get<uint8>();
            ri.MaxLevel      = ri.MinLevel + 5;
            ri.Quality       = f[2].Get<uint8>();
            ri.InventoryType = f[3].Get<uint32>();
            ri.Class         = f[4].Get<uint32>();
            ri.SubClass      = f[5].Get<uint32>();
            ri.AllowableClass = f[6].Get<int32>();
            ri.ItemLevel     = f[7].Get<uint16>();
            _rewardItems.push_back(ri);
        } while (result->NextRow());
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: {} reward items cached.", _rewardItems.size());
}

// Cache items for mob loot drops
void DungeonMasterMgr::LoadLootPool()
{
    _lootPool.clear();

    // Grey junk, white consumables, green/blue/purple equipment
    QueryResult result = WorldDatabase.Query(
        "SELECT entry, RequiredLevel, Quality, class, subclass, AllowableClass, ItemLevel "
        "FROM item_template "
        "WHERE Quality <= 4 "
        "AND ItemLevel <= 300 "
        "AND SellPrice > 0 "
        "AND class IN (0, 2, 4, 7, 15) "
        "AND (Flags & 0x8) = 0 "
        "AND AllowableClass != 0 "
        "AND RequiredReputationFaction = 0 "
        "AND RequiredHonorRank = 0 "
        "AND (RequiredLevel > 0 OR class NOT IN (2, 4)) "               // equipment must have a required level
        "AND name NOT LIKE '%Test%' "
        "AND name NOT LIKE '%Deprecated%' "
        "AND name NOT LIKE '%[PH]%' "
        "AND name NOT LIKE '%OLD%' "
        "AND name NOT LIKE '%Monster -%' "
        "AND name NOT LIKE '%zzOLD%' "
        "AND name NOT LIKE '%Debug%' "
        "ORDER BY RequiredLevel, Quality");

    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            LootPoolItem li;
            li.Entry          = f[0].Get<uint32>();
            li.MinLevel       = f[1].Get<uint8>();
            li.Quality        = f[2].Get<uint8>();
            li.ItemClass      = f[3].Get<uint8>();
            li.SubClass       = f[4].Get<uint8>();
            li.AllowableClass = f[5].Get<int32>();
            li.ItemLevel      = f[6].Get<uint16>();
            _lootPool.push_back(li);
        } while (result->NextRow());
    }


    uint32 counts[5] = {};
    for (const auto& li : _lootPool)
        if (li.Quality <= 4) ++counts[li.Quality];

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: {} mob loot items cached "
        "(grey={}, white={}, green={}, blue={}, epic={}).",
        _lootPool.size(), counts[0], counts[1], counts[2], counts[3], counts[4]);
}

// Compute group average level
uint8 DungeonMasterMgr::ComputeEffectiveLevel(Player* leader) const
{
    if (!leader)
        return 1;

    Group* group = leader->GetGroup();
    if (!group)
        return leader->GetLevel();

    uint32 totalLevel = 0;
    uint32 count      = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (m && m->IsInWorld())
        {
            totalLevel += m->GetLevel();
            ++count;
        }
    }

    return count > 0
        ? static_cast<uint8>(totalLevel / count)
        : leader->GetLevel();
}

uint8 DungeonMasterMgr::ComputeEffectiveLevelForDifficulty(Player* leader, uint32 difficultyId) const
{
    const DifficultyTier* diff = sDMConfig->GetDifficulty(difficultyId);
    uint8 raw = ComputeEffectiveLevel(leader);
    if (!diff)
        return raw;
    return diff->ClampLevel(raw);
}

// SESSION LIFECYCLE

Session* DungeonMasterMgr::CreateSession(Player* leader, uint32 difficultyId,
                                          uint32 themeId, uint32 mapId,
                                          bool scaleToParty)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);

    const DifficultyTier* diff  = sDMConfig->GetDifficulty(difficultyId);
    const Theme*          theme = sDMConfig->GetTheme(themeId);
    const DungeonInfo*    dg    = sDMConfig->GetDungeon(mapId);
    if (!diff || !theme || !dg)
        return nullptr;

    std::lock_guard<std::mutex> lock(_sessionMutex);

    // Check capacity under the lock to avoid race conditions
    if (!CanCreateNewSession())
        return nullptr;

    Session s;
    s.SessionId    = _nextSessionId++;
    s.LeaderGuid   = leader->GetGUID();
    s.State        = SessionState::Preparing;
    s.DifficultyId = difficultyId;
    s.ThemeId      = themeId;
    s.MapId        = mapId;
    s.ScaleToParty = scaleToParty;
    s.StartTime    = GameTime::GetGameTime().count();

    if (sDMConfig->IsTimeLimitEnabled())
        s.TimeLimit = sDMConfig->GetTimeLimitMinutes() * 60;


    if (scaleToParty)
    {
        // Scale to party: base level from group average, then clamp to tier so
        // e.g. Novice (10-19) never spawns skull/?? mobs because of high-level bots in the group.
        s.EffectiveLevel = diff->ClampLevel(ComputeEffectiveLevel(leader));

        uint8 band = sDMConfig->GetLevelBand();
        // Level window for template picks: effective ± band, then intersected with tier.
        s.LevelBandMin = (s.EffectiveLevel > band) ? (s.EffectiveLevel - band) : diff->MinLevel;
        uint16 bandMaxWide = uint16(s.EffectiveLevel) + band;
        s.LevelBandMax = static_cast<uint8>(std::min<uint16>(bandMaxWide, 83));

        s.LevelBandMin = std::max(s.LevelBandMin, diff->MinLevel);
        s.LevelBandMax = std::min(s.LevelBandMax, diff->MaxLevel);
    }
    else
    {
        // Use tier's natural level range — no party scaling.
        // EffectiveLevel = midpoint of the tier; band = full tier range.
        s.EffectiveLevel = static_cast<uint8>((uint16(diff->MinLevel) + uint16(diff->MaxLevel)) / 2);
        s.LevelBandMin   = diff->MinLevel;
        s.LevelBandMax   = diff->MaxLevel;
    }

    // Ensure min <= max after clamping (misconfigured tier or extreme band vs narrow tier).
    if (s.LevelBandMin > s.LevelBandMax)
        s.LevelBandMin = s.LevelBandMax;

    // Keep effective level inside the final band so stats / messaging match template overlap.
    s.EffectiveLevel = std::min(s.LevelBandMax, std::max(s.LevelBandMin, s.EffectiveLevel));

    PlayerSessionData ld;
    ld.PlayerGuid  = leader->GetGUID();
    ld.ReturnMapId = leader->GetMapId();
    ld.ReturnPosition = { leader->GetPositionX(), leader->GetPositionY(),
                          leader->GetPositionZ(), leader->GetOrientation() };
    s.Players.push_back(ld);


    if (Group* g = leader->GetGroup())
    {
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (m && m != leader && m->IsInWorld())
            {
                PlayerSessionData md;
                md.PlayerGuid  = m->GetGUID();
                md.ReturnMapId = m->GetMapId();
                md.ReturnPosition = { m->GetPositionX(), m->GetPositionY(),
                                      m->GetPositionZ(), m->GetOrientation() };
                s.Players.push_back(md);
            }
        }
    }

    _activeSessions[s.SessionId] = s;
    for (const auto& pd : s.Players)
        _playerToSession[pd.PlayerGuid] = s.SessionId;

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} — leader {}, party {}, diff {}, level band {}-{}, scale={}",
        s.SessionId, leader->GetName(), s.Players.size(),
        diff->Name, s.LevelBandMin, s.LevelBandMax, scaleToParty ? "party" : "tier");

    return &_activeSessions[s.SessionId];
}

Session* DungeonMasterMgr::GetSession(uint32 id)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(id);
    return it != _activeSessions.end() ? &it->second : nullptr;
}

Session* DungeonMasterMgr::GetSessionByInstance(uint32 instId)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _instanceToSession.find(instId);
    if (it != _instanceToSession.end())
    {
        auto sit = _activeSessions.find(it->second);
        return sit != _activeSessions.end() ? &sit->second : nullptr;
    }
    return nullptr;
}

bool DungeonMasterMgr::HasActiveSessionForInstance(uint32 instanceId) const
{
    if (!instanceId)
        return false;

    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto instanceIt = _instanceToSession.find(instanceId);
    if (instanceIt == _instanceToSession.end())
        return false;

    auto sessionIt = _activeSessions.find(instanceIt->second);
    if (sessionIt == _activeSessions.end())
        return false;

    return sessionIt->second.IsActive();
}

void DungeonMasterMgr::RegisterSessionInstance(uint32 sessionId, uint32 instanceId)
{
    if (!instanceId)
        return;

    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto sessionIt = _activeSessions.find(sessionId);
    if (sessionIt == _activeSessions.end())
        return;

    sessionIt->second.InstanceId = instanceId;
    _instanceToSession[instanceId] = sessionId;
}

Session* DungeonMasterMgr::GetSessionByPlayer(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _playerToSession.find(guid);
    if (it != _playerToSession.end())
    {
        auto sit = _activeSessions.find(it->second);
        return sit != _activeSessions.end() ? &sit->second : nullptr;
    }
    return nullptr;
}

uint32 DungeonMasterMgr::GetSessionIdByPlayer(ObjectGuid guid) const
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _playerToSession.find(guid);
    return it != _playerToSession.end() ? it->second : 0;
}

bool DungeonMasterMgr::GetSessionSnapshot(uint32 sessionId, Session& snapshot) const
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(sessionId);
    if (it == _activeSessions.end())
        return false;

    snapshot = it->second;
    return true;
}

bool DungeonMasterMgr::GetSessionSnapshotByPlayer(ObjectGuid guid, Session& snapshot) const
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto playerIt = _playerToSession.find(guid);
    if (playerIt == _playerToSession.end())
        return false;

    auto sessionIt = _activeSessions.find(playerIt->second);
    if (sessionIt == _activeSessions.end())
        return false;

    snapshot = sessionIt->second;
    return true;
}

// StartDungeon / TeleportPartyIn / TeleportPartyOut

bool DungeonMasterMgr::StartDungeon(Session* session)
{
    if (!session) return false;

    session->EntrancePos = GetDungeonEntrance(session->MapId);
    if (session->EntrancePos.GetPositionX() == 0 &&
        session->EntrancePos.GetPositionY() == 0 &&
        session->EntrancePos.GetPositionZ() == 0)
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: No entrance coords for map {}", session->MapId);
        return false;
    }
    return true;
}

bool DungeonMasterMgr::TeleportPartyIn(Session* session)
{
    if (!session) return false;
    const DungeonInfo* dg = sDMConfig->GetDungeon(session->MapId);
    if (!dg) return false;

    Position ent = session->EntrancePos;
    uint32 ok = 0;

    for (auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p) continue;

        pd.ReturnMapId    = p->GetMapId();
        pd.ReturnPosition = { p->GetPositionX(), p->GetPositionY(),
                              p->GetPositionZ(), p->GetOrientation() };

        // Resurrect dead players so they can teleport (roguelike floor transitions, etc.)
        if (!p->IsAlive())
        {
            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }

        if (p->TeleportTo(session->MapId, ent.GetPositionX(), ent.GetPositionY(),
                          ent.GetPositionZ(), ent.GetOrientation()))
        {
            ++ok;
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: TeleportTo queued for {} → map {} ({:.1f}, {:.1f}, {:.1f})",
                p->GetName(), session->MapId, ent.GetPositionX(), ent.GetPositionY(), ent.GetPositionZ());
            char buf[256];
            snprintf(buf, sizeof(buf),
                "|cFF00FF00[Dungeon Master]|r Welcome to |cFFFFFFFF%s|r! "
                "Defeat the boss to claim your reward.",
                dg->Name.c_str());
            ChatHandler(p->GetSession()).SendSysMessage(buf);

            if (session->RoguelikeRunId != 0 && sRoguelikeMgr->HasActiveAffixes(session->RoguelikeRunId))
            {
                std::string affixNames = sRoguelikeMgr->GetActiveAffixNames(session->RoguelikeRunId);
                char affixBuf[512];
                snprintf(affixBuf, sizeof(affixBuf),
                    "|cFF00FFFF[Roguelike]|r Active affixes: %s", affixNames.c_str());
                ChatHandler(p->GetSession()).SendSysMessage(affixBuf);
            }
        }
        else
        {
            LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: TeleportTo FAILED for {} → map {} ({:.1f}, {:.1f}, {:.1f})",
                p->GetName(), session->MapId, ent.GetPositionX(), ent.GetPositionY(), ent.GetPositionZ());
            ChatHandler(p->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r Teleport failed! You may lack access to this dungeon.");
        }
    }

    if (ok > 0)
    {
        session->State = SessionState::InProgress;
        // InstanceId is set when a player actually arrives on the map
        // (via the allmap script or the Update tick populate logic).
        return true;
    }
    return false;
}

void DungeonMasterMgr::TeleportPartyOut(Session* session)
{
    if (!session) return;
    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;
        p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
        if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
        p->TeleportTo(pd.ReturnMapId, pd.ReturnPosition.GetPositionX(),
                      pd.ReturnPosition.GetPositionY(), pd.ReturnPosition.GetPositionZ(),
                      pd.ReturnPosition.GetOrientation());
    }
}

// Dungeon entrance lookup from areatrigger_teleport
Position DungeonMasterMgr::GetDungeonEntrance(uint32 mapId)
{
    std::vector<DungeonEntranceLocation> entrances = LoadDungeonEntrancesForMap(mapId);
    if (!entrances.empty())
        return entrances.front().Pos;

    LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No areatrigger_teleport for map {}", mapId);
    return { 0, 0, 0, 0 };
}

uint32 DungeonMasterMgr::CalculateTrashSpawnBudget(Session const& session, size_t availableTrashPoints) const
{
    if (availableTrashPoints == 0)
        return 0;

    const DifficultyTier* difficulty = sDMConfig->GetDifficulty(session.DifficultyId);
    float mobCountMultiplier = difficulty ? difficulty->MobCountMultiplier : 1.0f;
    mobCountMultiplier = std::max(0.25f, mobCountMultiplier);
    float globalBudgetMultiplier = std::max(0.10f, sDMConfig->GetTrashBudgetGlobalMultiplier());

    if (availableTrashPoints <= 12)
        return static_cast<uint32>(availableTrashPoints);

    float normalizedBase = (sDMConfig->GetTrashBudgetBase()
        + (std::sqrt(static_cast<float>(availableTrashPoints)) * sDMConfig->GetTrashBudgetSqrtScale())
        + ((session.Players.size() > 1)
            ? static_cast<float>(session.Players.size() - 1) * sDMConfig->GetTrashBudgetPerExtraPlayer()
            : 0.0f)
        + (session.RoguelikeRunId != 0 ? sDMConfig->GetTrashBudgetRoguelikeBonus() : 0.0f))
        * globalBudgetMultiplier;

    uint32 budget = static_cast<uint32>(std::lround(normalizedBase * mobCountMultiplier));

    uint32 staticMinBudget = sDMConfig->GetTrashBudgetMinSpawns();
    uint32 percentMinBudget = static_cast<uint32>(std::ceil(
        static_cast<float>(availableTrashPoints) * std::max(0.0f, sDMConfig->GetTrashBudgetMinPercent())));
    uint32 minBudget = std::min<uint32>(static_cast<uint32>(availableTrashPoints), std::max(staticMinBudget, percentMinBudget));
    uint32 maxBudget = std::min<uint32>(static_cast<uint32>(availableTrashPoints), std::max(sDMConfig->GetTrashBudgetMaxSpawns(), minBudget));

    if (maxBudget < minBudget)
        maxBudget = minBudget;

    return std::clamp(budget, minBudget, maxBudget);
}

std::vector<SpawnPoint> DungeonMasterMgr::SelectTrashSpawnPoints(Session const& session,
    std::vector<SpawnPoint> const& availableTrashPoints) const
{
    std::vector<SpawnPoint> selected;

    uint32 budget = CalculateTrashSpawnBudget(session, availableTrashPoints.size());
    if (budget == 0)
        return selected;

    if (budget >= availableTrashPoints.size())
        return availableTrashPoints;

    selected.reserve(budget);
    std::unordered_set<size_t> usedIndices;

    float stride = static_cast<float>(availableTrashPoints.size()) / static_cast<float>(budget);
    for (uint32 i = 0; i < budget; ++i)
    {
        size_t bucketStart = static_cast<size_t>(std::floor(i * stride));
        size_t bucketEnd = static_cast<size_t>(std::ceil((i + 1) * stride));
        if (bucketStart >= availableTrashPoints.size())
            bucketStart = availableTrashPoints.size() - 1;
        if (bucketEnd == 0)
            bucketEnd = 1;
        if (bucketEnd > availableTrashPoints.size())
            bucketEnd = availableTrashPoints.size();
        if (bucketEnd <= bucketStart)
            bucketEnd = std::min(availableTrashPoints.size(), bucketStart + 1);

        size_t pick = bucketStart;
        if (bucketEnd > bucketStart + 1)
            pick = RandInt<size_t>(bucketStart, bucketEnd - 1);

        while (usedIndices.count(pick) > 0 && pick + 1 < bucketEnd)
            ++pick;
        while (usedIndices.count(pick) > 0 && pick > bucketStart)
            --pick;
        if (usedIndices.count(pick) > 0)
            continue;

        usedIndices.insert(pick);
        selected.push_back(availableTrashPoints[pick]);
    }

    std::sort(selected.begin(), selected.end(),
        [](SpawnPoint const& left, SpawnPoint const& right)
        {
            return left.DistanceFromEntrance < right.DistanceFromEntrance;
        });

    return selected;
}

// Spawn-point collection
std::vector<SpawnPoint> DungeonMasterMgr::GetSpawnPointsForMap(uint32 mapId, Position const& entrancePos)
{
    std::vector<SpawnPoint> pts;

    std::vector<DungeonEntranceLocation> entrances = LoadDungeonEntrancesForMap(mapId);
    Position ent = entrancePos;

    if (ent.GetPositionX() == 0.0f && ent.GetPositionY() == 0.0f && ent.GetPositionZ() == 0.0f)
    {
        if (!entrances.empty())
            ent = entrances.front().Pos;
        else
            ent = GetDungeonEntrance(mapId);
    }

    bool useEntranceClusters = entrances.size() > 1;
    size_t selectedEntranceIndex = 0;

    if (useEntranceClusters)
    {
        float bestDistSq = std::numeric_limits<float>::max();
        for (size_t i = 0; i < entrances.size(); ++i)
        {
            DungeonEntranceLocation const& entrance = entrances[i];
            float distSq = GetDistanceSq(
                ent.GetPositionX(), ent.GetPositionY(), ent.GetPositionZ(),
                entrance.Pos.GetPositionX(), entrance.Pos.GetPositionY(), entrance.Pos.GetPositionZ());

            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                selectedEntranceIndex = i;
            }
        }
    }

    char q[256];
    snprintf(q, sizeof(q),
        "SELECT position_x, position_y, position_z, orientation "
        "FROM creature WHERE map = %u", mapId);
    QueryResult result = WorldDatabase.Query(q);
    if (!result) return pts;

    float ex = ent.GetPositionX(), ey = ent.GetPositionY(), ez = ent.GetPositionZ();
    uint32 filteredSpawnCount = 0;

    do
    {
        Field* f = result->Fetch();
        float x = f[0].Get<float>(), y = f[1].Get<float>(),
              z = f[2].Get<float>(), o = f[3].Get<float>();

        if (useEntranceClusters && !BelongsToEntranceCluster(entrances, selectedEntranceIndex, x, y, z))
        {
            ++filteredSpawnCount;
            continue;
        }

        SpawnPoint sp;
        sp.Pos.Relocate(x, y, z, o);
        float dx = x - ex, dy = y - ey, dz = z - ez;
        sp.DistanceFromEntrance = std::sqrt(dx*dx + dy*dy + dz*dz);
        pts.push_back(sp);
    } while (result->NextRow());

    // Sort near → far
    std::sort(pts.begin(), pts.end(),
        [](const SpawnPoint& a, const SpawnPoint& b)
        { return a.DistanceFromEntrance < b.DistanceFromEntrance; });

    // Find boss positions from creature data
    bool bossFound = false;

    char bq[512];
    snprintf(bq, sizeof(bq),
        "SELECT c.position_x, c.position_y, c.position_z, c.orientation, "
        "COALESCE(ci.MechanicsMask, 0), ct.`rank`, ct.name "
        "FROM creature c "
        "JOIN creature_template ct ON c.id1 = ct.entry "
        "LEFT JOIN creature_immunities ci ON ci.ID = ct.CreatureImmunitiesId "
        "WHERE c.map = %u "
        "AND COALESCE(ci.MechanicsMask, 0) > 0 "
        "AND ct.`rank` >= 1 "
        "ORDER BY COALESCE(ci.MechanicsMask, 0) DESC",
        mapId);
    QueryResult bossResult = WorldDatabase.Query(bq);

    if (bossResult)
    {
        // Collect all boss candidates, pick the farthest from entrance
        struct BossCandidate {
            float x, y, z, o;
            float dist;
            uint64 immuneMask;
            std::string name;
        };
        std::vector<BossCandidate> bosses;

        do
        {
            Field* f = bossResult->Fetch();
            BossCandidate bc;
            bc.x = f[0].Get<float>();
            bc.y = f[1].Get<float>();
            bc.z = f[2].Get<float>();
            bc.o = f[3].Get<float>();
            bc.immuneMask = f[4].Get<uint64>();
            bc.name = f[6].Get<std::string>();

            if (useEntranceClusters && !BelongsToEntranceCluster(entrances, selectedEntranceIndex, bc.x, bc.y, bc.z))
                continue;

            float dx = bc.x - ex, dy = bc.y - ey, dz = bc.z - ez;
            bc.dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            bosses.push_back(bc);
        } while (bossResult->NextRow());

        if (!bosses.empty())
        {
            // The "last boss" is the farthest boss-type creature from the entrance.
            std::sort(bosses.begin(), bosses.end(),
                [](const BossCandidate& a, const BossCandidate& b)
                { return a.dist > b.dist; });

            const BossCandidate& lastBoss = bosses[0];

            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Map {} — found {} boss candidate(s). "
                "Last boss: '{}' at ({:.1f}, {:.1f}, {:.1f}), immuneMask={}, dist={:.1f}",
                mapId, bosses.size(), lastBoss.name,
                lastBoss.x, lastBoss.y, lastBoss.z,
                lastBoss.immuneMask, lastBoss.dist);

            // Create boss spawn point(s) at the actual boss location(s).
            uint32 bc = sDMConfig->GetBossCount();
            for (uint32 i = 0; i < bc && i < bosses.size(); ++i)
            {
                SpawnPoint bsp;
                bsp.Pos.Relocate(bosses[i].x, bosses[i].y, bosses[i].z, bosses[i].o);
                bsp.DistanceFromEntrance = bosses[i].dist;
                bsp.IsBossPosition = true;
                pts.push_back(bsp);
            }
            bossFound = true;
        }
    }

    if (useEntranceClusters)
    {
        DungeonEntranceLocation const& entrance = entrances[selectedEntranceIndex];
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Map {} — restricted shared-map spawns to entrance '{}' (trigger {}), kept {} spawn points and filtered {}.",
            mapId, entrance.Name, entrance.TriggerId, pts.size(), filteredSpawnCount);
    }

    // Fallback: if no actual boss found in DB, use farthest spawn point(s)
    if (!bossFound)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Map {} — no boss creatures found in DB, "
            "falling back to farthest spawn points.", mapId);

        // Re-sort since we may have added boss points
        std::sort(pts.begin(), pts.end(),
            [](const SpawnPoint& a, const SpawnPoint& b)
            { return a.DistanceFromEntrance < b.DistanceFromEntrance; });

        uint32 bc = sDMConfig->GetBossCount();
        for (uint32 i = 0; i < bc && i < pts.size(); ++i)
            pts[pts.size() - 1 - i].IsBossPosition = true;
    }

    return pts;
}

// Instance population
void DungeonMasterMgr::ClearDungeonCreatures(InstanceMap* map)
{
    if (!map) return;

    uint32 npcEntry = sDMConfig->GetNpcEntry();
    uint32 totalRemoved = 0;

    // Phase 1: despawn our tracked creatures
    uint32 instanceId = map->GetInstanceId();
    auto guidIt = _instanceCreatureGuids.find(instanceId);
    std::vector<ObjectGuid> trackedGuids;
    if (guidIt != _instanceCreatureGuids.end())
    {
        trackedGuids = guidIt->second;
        guidIt->second.clear();
    }

    for (ObjectGuid const& guid : trackedGuids)
    {
        Creature* c = map->GetCreature(guid);
        if (c && c->IsInWorld())
        {
            c->DespawnOrUnsummon();
            ++totalRemoved;
        }
    }

    uint32 dbRemoved = 0;

    // Phase 2: despawn DB-spawned creatures
    auto const& store = map->GetCreatureBySpawnIdStore();
    std::vector<ObjectGuid> dbCreatureGuids;
    dbCreatureGuids.reserve(store.size());

    for (auto const& pair : store)
    {
        Creature* c = pair.second;
        if (c && c->IsInWorld() && !c->IsPet() && !c->IsGuardian()
            && !c->IsTotem() && c->GetEntry() != npcEntry)
            dbCreatureGuids.push_back(c->GetGUID());
    }

    for (ObjectGuid const& guid : dbCreatureGuids)
    {
        Creature* c = map->GetCreature(guid);
        if (c && c->IsInWorld() && !c->IsPet() && !c->IsGuardian()
            && !c->IsTotem() && c->GetEntry() != npcEntry)
        {
            c->SetRespawnTime(7 * DAY);
            c->DespawnOrUnsummon();
            ++dbRemoved;
        }
    }

    // Phase 3: grid sweep for script-spawned creatures
    uint32 gridRemoved = 0;
    Map::PlayerList const& players = map->GetPlayers();
    for (auto const& itr : players)
    {
        Player* p = itr.GetSource();
        if (!p || !p->IsInWorld()) continue;

        std::list<Creature*> gridCreatures;
        std::vector<ObjectGuid> gridCreatureGuids;
        p->GetCreatureListWithEntryInGrid(gridCreatures, 0, 5000.0f);

        for (Creature* c : gridCreatures)
        {
            if (!c || !c->IsInWorld()) continue;
            if (c->IsPet() || c->IsGuardian() || c->IsTotem()) continue;
            if (c->GetEntry() == npcEntry) continue;

            gridCreatureGuids.push_back(c->GetGUID());
        }

        for (ObjectGuid const& guid : gridCreatureGuids)
        {
            Creature* c = map->GetCreature(guid);
            if (!c || !c->IsInWorld()) continue;
            if (c->IsPet() || c->IsGuardian() || c->IsTotem()) continue;
            if (c->GetEntry() == npcEntry) continue;

            c->SetRespawnTime(7 * DAY);
            c->DespawnOrUnsummon();
            ++gridRemoved;
        }

        // Only need one player's grid sweep (the grid search radius
        // is large enough to cover the entire dungeon)
        break;
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Cleared {} tracked + {} DB + {} grid creatures from map {} (inst {})",
        totalRemoved, dbRemoved, gridRemoved, map->GetId(), instanceId);
}

void DungeonMasterMgr::OpenAllDoors(InstanceMap* map)
{
    if (!map) return;

    std::vector<ObjectGuid> doorGuids;
    auto const& store = map->GetGameObjectBySpawnIdStore();
    for (auto const& pair : store)
    {
        GameObject* go = pair.second;
        if (!go || !go->IsInWorld()) continue;
        if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR || go->GetGoType() == GAMEOBJECT_TYPE_BUTTON)
            doorGuids.push_back(go->GetGUID());
    }

    for (ObjectGuid const& guid : doorGuids)
    {
        GameObject* go = map->GetGameObject(guid);
        if (go && go->IsInWorld())
            go->Delete();
    }

    LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Removed {} doors from instance.", doorGuids.size());
}

// Populate dungeon with themed creatures and bosses
void DungeonMasterMgr::PopulateDungeon(Session* session, InstanceMap* map)
{
    if (!session || !map) return;

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: PopulateDungeon ENTRY — session {} map {} instId {} mobs {} bosses {}",
        session->SessionId, session->MapId, map->GetInstanceId(),
        session->TotalMobs, session->TotalBosses);

    const DifficultyTier* diff  = sDMConfig->GetDifficulty(session->DifficultyId);
    const Theme*          theme = sDMConfig->GetTheme(session->ThemeId);
    if (!diff || !theme) return;

    auto failPopulation = [&](std::string const& reason)
    {
        session->State = SessionState::Failed;
        session->EndTime = GameTime::GetGameTime().count();

        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: Session {} population failed on map {} — {}",
            session->SessionId, session->MapId, reason);

        for (const auto& pd : session->Players)
            if (Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                if (player->GetSession())
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cFFFF0000[Dungeon Master]|r Challenge setup failed: {}",
                        reason);
    };

    ClearDungeonCreatures(map);
    OpenAllDoors(map);

    // Mark all boss encounters as DONE so scripts don't interfere
    if (InstanceScript* script = map->GetInstanceScript())
    {
        uint32 bossesNeutralized = 0;
        // Loop through boss indices.  GetBossState returns TO_BE_DECIDED (5)
        // for indices past the end of the encounter list — use as sentinel.
        for (uint32 i = 0; i < 25; ++i)
        {
            EncounterState state = script->GetBossState(i);
            if (state == TO_BE_DECIDED)
                break;   // Past the last boss — stop

            if (state != DONE)
            {
                script->SetBossState(i, DONE);
                ++bossesNeutralized;
            }
        }
        if (bossesNeutralized > 0)
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Neutralized {} InstanceScript boss encounter(s) in map {}",
                bossesNeutralized, map->GetId());
    }

    // Purge lingering debuffs from despawned creatures
    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld()) continue;

        std::vector<uint32> toRemove;
        for (auto const& pair : p->GetAppliedAuras())
        {
            if (Aura* aura = pair.second->GetBase())
            {
                // Caster is gone (despawned) and it wasn't self-cast — lingering debuff
                if (!aura->GetCaster() && aura->GetCasterGUID() != p->GetGUID())
                    toRemove.push_back(aura->GetId());
            }
        }
        for (uint32 spellId : toRemove)
            p->RemoveAura(spellId);

        if (!toRemove.empty())
            LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Purged {} lingering debuff(s) from {}",
                toRemove.size(), p->GetName());
    }

    session->SpawnPoints = GetSpawnPointsForMap(session->MapId, session->EntrancePos);
    if (session->SpawnPoints.empty())
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: No spawn points for map {}", session->MapId);
        failPopulation("no valid spawn points were found for this dungeon");
        return;
    }

    std::vector<SpawnPoint> availableTrashPoints;
    std::vector<SpawnPoint> bossSpawnPoints;
    availableTrashPoints.reserve(session->SpawnPoints.size());
    bossSpawnPoints.reserve(sDMConfig->GetBossCount());

    for (SpawnPoint const& spawnPoint : session->SpawnPoints)
    {
        if (spawnPoint.IsBossPosition)
            bossSpawnPoints.push_back(spawnPoint);
        else
            availableTrashPoints.push_back(spawnPoint);
    }

    std::vector<SpawnPoint> selectedTrashPoints = SelectTrashSpawnPoints(*session, availableTrashPoints);
    if (selectedTrashPoints.empty() && !availableTrashPoints.empty())
    {
        failPopulation("no playable trash route could be built for this dungeon");
        return;
    }

    session->SpawnPoints.clear();
    session->SpawnPoints.reserve(selectedTrashPoints.size() + bossSpawnPoints.size());
    session->SpawnPoints.insert(session->SpawnPoints.end(), selectedTrashPoints.begin(), selectedTrashPoints.end());
    session->SpawnPoints.insert(session->SpawnPoints.end(), bossSpawnPoints.begin(), bossSpawnPoints.end());

    const DifficultyTier* difficulty = sDMConfig->GetDifficulty(session->DifficultyId);
    LOG_INFO(DM_LOG_CATEGORY,
        "DungeonMaster: Session {} spawn budget on map {} — kept {} of {} trash points, {} boss points, mobMultiplier={:.2f}, partySize={}, roguelike={}",
        session->SessionId, session->MapId, selectedTrashPoints.size(), availableTrashPoints.size(), bossSpawnPoints.size(),
        difficulty ? difficulty->MobCountMultiplier : 1.0f, session->Players.size(), session->RoguelikeRunId != 0 ? "yes" : "no");

    float hpMult  = CalculateHealthMultiplier(session);
    float dmgMult = CalculateDamageMultiplier(session);

    uint8 bandMin = session->LevelBandMin;
    uint8 bandMax = session->LevelBandMax;
    uint8 targetLevel = session->EffectiveLevel;


    uint32 instanceId = map->GetInstanceId();
    auto& guidList = _instanceCreatureGuids[instanceId];
    guidList.clear();

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Populating session {} — theme '{}', band {}-{}, target lvl {}, HP x{:.2f}, DMG x{:.2f}",
        session->SessionId, theme->Name, bandMin, bandMax, targetLevel, hpMult, dmgMult);

    // Force-scale creature to target level
    // Compute a boss-specific damage multiplier that only includes party scaling,
    // NOT the difficulty tier's DamageMultiplier (to avoid double-stacking).
    float bossOnlyDmgMult;
    {
        uint32 n = session->Players.size();
        if (n <= 1) bossOnlyDmgMult = sDMConfig->GetSoloMultiplier();
        else        bossOnlyDmgMult = 1.0f + (n - 1) * sDMConfig->GetPerPlayerDamageMult();
        if (session->RoguelikeRunId != 0)
            bossOnlyDmgMult *= sRoguelikeMgr->GetTierDamageMultiplier(session->RoguelikeRunId);
    }

    auto applyLevelAndStats = [&](Creature* c, float extraHpMult, float extraDmgMult, bool isBoss)
    {
    
        c->SetLevel(targetLevel);

    
        if (isBoss)
        {
            c->SetByteValue(UNIT_FIELD_BYTES_0, 2, 1);  // Elite rank → gold dragon frame
            c->SetObjectScale(1.3f);                      // 30% larger than normal
        }

        uint8 unitClass = c->GetCreatureTemplate()->unit_class;
        const ClassLevelStatEntry* baseStats = GetBaseStatsForLevel(unitClass, targetLevel);

    
        float finalHP;
        if (baseStats)
            finalHP = static_cast<float>(baseStats->BaseHP) * hpMult * extraHpMult;
        else
            finalHP = c->GetMaxHealth() * hpMult * extraHpMult;

        uint32 hp = std::max(1u, static_cast<uint32>(finalHP));
        c->SetMaxHealth(hp);
        c->SetHealth(hp);

        // For bosses, use party-only scaling (bossOnlyDmgMult) instead of the full
        // tier+party dmgMult to prevent double-stacking tier DamageMultiplier with BossDamageMult
        float effectiveDmgMult = isBoss ? bossOnlyDmgMult : dmgMult;

        if (baseStats)
        {
            float dmgBase  = baseStats->BaseDamage;
            float apBonus  = static_cast<float>(baseStats->AttackPower) / 14.0f;
            float atkTime  = static_cast<float>(c->GetCreatureTemplate()->BaseAttackTime) / 1000.0f;
            if (atkTime <= 0.0f) atkTime = 2.0f;

            float minDmg = (dmgBase + apBonus) * atkTime * effectiveDmgMult * extraDmgMult;
            float maxDmg = ((dmgBase * 1.15f) + apBonus) * atkTime * effectiveDmgMult * extraDmgMult;

            minDmg = std::max(1.0f, minDmg);
            maxDmg = std::max(minDmg, maxDmg);

            c->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
            c->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
            c->UpdateDamagePhysical(BASE_ATTACK);
        }

        // --- Armor (from classlevelstats for the TARGET level) ---
        if (baseStats && baseStats->BaseArmor > 0)
            c->SetArmor(baseStats->BaseArmor);

        // --- Roguelike: additional armor scaling from tier progression ---
        if (session->RoguelikeRunId != 0)
        {
            float armorMult = sRoguelikeMgr->GetTierArmorMultiplier(session->RoguelikeRunId);
            if (armorMult > 1.0f)
                c->SetArmor(static_cast<uint32>(c->GetArmor() * armorMult));
        }

        // --- Clear ALL spell resistances (original template values are for original level) ---
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            c->SetResistance(SpellSchools(school), 0);

        // --- Clear mechanic immunities ---
        for (uint32 mech = 1; mech < MAX_MECHANIC; ++mech)
            c->ApplySpellImmune(0, IMMUNITY_MECHANIC, mech, false);

        // --- Clear spell immunities that might come from the template ---
        c->ApplySpellImmune(0, IMMUNITY_SCHOOL, SPELL_SCHOOL_MASK_ALL, false);

        // --- Movement ---
        if (isBoss)
        {
            // Bosses stay at their spawn point until engaged.
            c->SetWanderDistance(0.0f);
            c->SetDefaultMovementType(IDLE_MOTION_TYPE);
        }
        else
        {
            // Trash mobs patrol a 5 yd radius around their spawn point
            c->SetWanderDistance(5.0f);
            c->SetDefaultMovementType(RANDOM_MOTION_TYPE);
            c->GetMotionMaster()->MoveRandom(5.0f);
        }

        // --- AI ---
        // Trash / rare spawns use DungeonMasterCreatureAI.
        // Bosses keep their native AI so scripted abilities and phase changes still work.
        if (!isBoss)
            c->SetAI(new DungeonMasterCreatureAI(c));

        // Force visibility refresh or client won't see the creature
        c->UpdateObjectVisibility(true);

        // Track this GUID for future cleanup
        guidList.push_back(c->GetGUID());
    };

    // Spawn trash mobs
    uint32 spawnedMobs = 0;
    for (auto& sp : session->SpawnPoints)
    {
        if (sp.IsBossPosition) continue;

        uint32 entry = SelectCreatureForTheme(session->MapId, theme, false, bandMin, bandMax);
        if (!entry) continue;

        Creature* c = map->SummonCreature(entry, sp.Pos);
        if (!c) continue;

        c->SetFaction(14);               // hostile to all
        c->SetReactState(REACT_AGGRESSIVE);
        c->SetObjectScale(1.0f);
        c->SetCorpseDelay(300);          // 5 min corpse before despawn
        c->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                        | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                        | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                        | UNIT_FLAG_NOT_SELECTABLE);
        c->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
        c->SetImmuneToPC(false);
        c->SetImmuneToNPC(false);
        c->setActive(true);             // Keep creature in grid update cycle for aggro detection

        bool isElite = (RandInt<uint32>(1, 100) <= sDMConfig->GetEliteChance());

        // Roguelike affix multipliers for trash
        float affixHpMult = 1.0f, affixDmgMult = 1.0f, affixEliteMult = 1.0f;
        if (session->RoguelikeRunId != 0)
        {
            sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                false, isElite, affixHpMult, affixDmgMult, affixEliteMult);
            // Savage affix: boosted elite chance
            if (affixEliteMult > 1.0f && !isElite)
            {
                uint32 boostedChance = static_cast<uint32>(sDMConfig->GetEliteChance() * affixEliteMult);
                isElite = (RandInt<uint32>(1, 100) <= boostedChance);
            }
        }

        float eliteHpMult  = isElite ? sDMConfig->GetEliteHealthMult() : 1.0f;

        if (isElite)
        {
            c->SetByteValue(UNIT_FIELD_BYTES_0, 2, CREATURE_ELITE_ELITE);
            c->SetObjectScale(1.1f);
        }

        float eliteDmgMult = isElite ? sDMConfig->GetEliteDamageMult() : 1.0f;

        applyLevelAndStats(c, eliteHpMult * affixHpMult, eliteDmgMult * affixDmgMult, false);

        SpawnedCreature sc;
        sc.Guid = c->GetGUID(); sc.Entry = entry;
        sc.IsElite = isElite; sc.IsBoss = false;
        session->SpawnedCreatures.push_back(sc);
        ++spawnedMobs;
    }
    session->TotalMobs = spawnedMobs;

    // --- Rare spawn (configurable chance, max 1 per run) ---
    if (sDMConfig->GetRareSpawnChance() > 0 &&
        RandInt<uint32>(1, 100) <= sDMConfig->GetRareSpawnChance())
    {
        // Pick non-boss spawn points for rare placement (prefer middle of dungeon)
        std::vector<size_t> validRarePoints;
        for (size_t i = 0; i < session->SpawnPoints.size(); ++i)
            if (!session->SpawnPoints[i].IsBossPosition)
                validRarePoints.push_back(i);

        if (!validRarePoints.empty())
        {
            size_t startIdx = validRarePoints.size() / 3;
            size_t endIdx   = std::max(startIdx, validRarePoints.size() * 2 / 3);
            if (endIdx >= validRarePoints.size()) endIdx = validRarePoints.size() - 1;
            size_t pickIdx  = validRarePoints[RandInt<size_t>(startIdx, endIdx)];
            SpawnPoint& rareSP = session->SpawnPoints[pickIdx];

            uint32 rareEntry = SelectCreatureForTheme(session->MapId, theme, true, bandMin, bandMax);
            if (rareEntry)
            {
                Creature* r = map->SummonCreature(rareEntry, rareSP.Pos);
                if (r)
                {
                    r->SetFaction(14);
                    r->SetReactState(REACT_AGGRESSIVE);
                    r->SetCorpseDelay(300);
                    r->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                                    | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                                    | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                                    | UNIT_FLAG_NOT_SELECTABLE);
                    r->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
                    r->SetImmuneToPC(false);
                    r->SetImmuneToNPC(false);
                    r->setActive(true);

                    // Silver dragon portrait (rank 4 = rare)
                    r->SetByteValue(UNIT_FIELD_BYTES_0, 2, 4);
                    r->SetObjectScale(1.15f);

                    float rareHpMult  = sDMConfig->GetRareHealthMult();
                    float rareDmgMult = sDMConfig->GetRareDamageMult();

                    // Apply roguelike affix multipliers to rares
                    float affixHpM = 1.0f, affixDmgM = 1.0f, affixEliteM = 1.0f;
                    if (session->RoguelikeRunId != 0)
                        sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                            false, true, affixHpM, affixDmgM, affixEliteM);

                    applyLevelAndStats(r, rareHpMult * affixHpM, rareDmgMult * affixDmgM, false);

                    SpawnedCreature sc;
                    sc.Guid = r->GetGUID(); sc.Entry = rareEntry;
                    sc.IsElite = true; sc.IsBoss = false; sc.IsRare = true;
                    session->SpawnedCreatures.push_back(sc);

                    for (const auto& pd : session->Players)
                        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                            if (p->GetSession())
                                ChatHandler(p->GetSession()).SendSysMessage(
                                    "|cFFFFD700[Dungeon Master]|r A |cFFFF8800rare enemy|r lurks in this dungeon!");

                    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Rare creature spawned — entry {} at ({:.1f}, {:.1f}, {:.1f})",
                        rareEntry, rareSP.Pos.GetPositionX(), rareSP.Pos.GetPositionY(), rareSP.Pos.GetPositionZ());
                }
            }
        }
    }

    // Spawn bosses (real dungeon bosses)
    uint32 bossesSpawned = 0;
    for (auto& sp : session->SpawnPoints)
    {
        if (!sp.IsBossPosition || bossesSpawned >= sDMConfig->GetBossCount())
            continue;

        uint32 entry = SelectDungeonBoss(session->MapId, theme, bandMin, bandMax);
        if (!entry) { LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No boss candidate."); continue; }

        Creature* b = map->SummonCreature(entry, sp.Pos);
        if (!b) continue;

        b->SetFaction(14);
        b->SetReactState(REACT_AGGRESSIVE);
        b->SetCorpseDelay(600);          // 10 min corpse for bosses
        b->setActive(true);             // Keep creature in grid update cycle for aggro detection

        // Roguelike affix multipliers for bosses
        float bossAffixHpMult = 1.0f, bossAffixDmgMult = 1.0f, _unused = 1.0f;
        if (session->RoguelikeRunId != 0)
            sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                true, true, bossAffixHpMult, bossAffixDmgMult, _unused);

        applyLevelAndStats(b,
            sDMConfig->GetBossHealthMult() * bossAffixHpMult,
            sDMConfig->GetBossDamageMult() * bossAffixDmgMult, true);

        SpawnedCreature sc;
        sc.Guid = b->GetGUID(); sc.Entry = entry;
        sc.IsElite = true; sc.IsBoss = true;
        session->SpawnedCreatures.push_back(sc);
        ++bossesSpawned;

        CreatureTemplate const* bossTemplate = b->GetCreatureTemplate();
        std::string aiLabel = "native/default";
        if (bossTemplate)
        {
            if (bossTemplate->ScriptID != 0)
                aiLabel = sObjectMgr->GetScriptName(bossTemplate->ScriptID);
            else if (!bossTemplate->AIName.empty())
                aiLabel = bossTemplate->AIName;
        }

        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Boss spawned — entry {}, name '{}', "
            "AI: {}, ReactState: {}, Level: {}",
            b->GetEntry(), b->GetName(),
            aiLabel,
            static_cast<int>(b->GetReactState()),
            b->GetLevel());
    }
    session->TotalBosses = bossesSpawned;

    if (bossesSpawned == 0)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Session {} spawned zero boss creatures on map {}. Attempting emergency fallback boss promotion.",
            session->SessionId, session->MapId);

        float fallbackBossAffixHpMult = 1.0f;
        float fallbackBossAffixDmgMult = 1.0f;
        float unusedEliteMult = 1.0f;
        if (session->RoguelikeRunId != 0)
            sRoguelikeMgr->GetAffixMultipliers(session->RoguelikeRunId,
                true, true, fallbackBossAffixHpMult, fallbackBossAffixDmgMult, unusedEliteMult);

        for (auto it = session->SpawnedCreatures.rbegin(); it != session->SpawnedCreatures.rend(); ++it)
        {
            if (it->IsBoss || it->IsDead)
                continue;

            Creature* fallbackBoss = map->GetCreature(it->Guid);
            if (!fallbackBoss || !fallbackBoss->IsInWorld() || !fallbackBoss->IsAlive())
                continue;

            fallbackBoss->SetCorpseDelay(600);
            fallbackBoss->SetFaction(14);
            fallbackBoss->SetReactState(REACT_AGGRESSIVE);
            fallbackBoss->setActive(true);

            applyLevelAndStats(fallbackBoss,
                sDMConfig->GetBossHealthMult() * fallbackBossAffixHpMult,
                sDMConfig->GetBossDamageMult() * fallbackBossAffixDmgMult,
                true);

            it->IsBoss = true;
            it->IsElite = true;
            ++bossesSpawned;
            session->TotalBosses = bossesSpawned;

            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Session {} promoted '{}' (entry {}, guid {}) to fallback boss status to keep the run completable.",
                session->SessionId, fallbackBoss->GetName(), fallbackBoss->GetEntry(), fallbackBoss->GetGUID().GetCounter());

            for (const auto& pd : session->Players)
                if (Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                    if (player->GetSession())
                        ChatHandler(player->GetSession()).PSendSysMessage(
                            "|cFFFFFF00[Dungeon Master]|r No valid boss template was available here, so |cFFFFFFFF{}|r has been promoted to the final boss.",
                            fallbackBoss->GetName());

            break;
        }
    }

    if (bossesSpawned == 0)
    {
        failPopulation("no valid final boss could be created for this dungeon");
        return;
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} — {} mobs, {} bosses spawned.",
        session->SessionId, session->TotalMobs, session->TotalBosses);

    // --- Reset encounter states to NOT_STARTED so boss AIs can engage properly ---
    // We set all encounters to DONE earlier (line ~1049) to clear original dungeon
    // bosses. Now that our custom bosses are spawned, reset encounters so their
    // ScriptName AIs do not think the encounter is already defeated.
    if (InstanceScript* script = map->GetInstanceScript())
    {
        uint32 encountersReset = 0;
        for (uint32 i = 0; i < 25; ++i)
        {
            EncounterState state = script->GetBossState(i);
            if (state == TO_BE_DECIDED)
                break;
            if (state == DONE)
            {
                script->SetBossState(i, NOT_STARTED);
                ++encountersReset;
            }
        }
        if (encountersReset > 0)
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Reset {} encounter(s) to NOT_STARTED for boss AI activation.",
                encountersReset);
    }

    // --- Clear boss flags AFTER encounter reset so scripted AIs cannot re-apply them ---
    for (auto& sc2 : session->SpawnedCreatures)
    {
        if (!sc2.IsBoss) continue;
        Creature* b = map->GetCreature(sc2.Guid);
        if (!b || !b->IsInWorld()) continue;
        b->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                        | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED
                                        | UNIT_FLAG_STUNNED | UNIT_FLAG_FLEEING
                                        | UNIT_FLAG_NOT_SELECTABLE);
        b->SetUInt32Value(UNIT_FIELD_FLAGS_2, 0);
        b->SetImmuneToPC(false);
        b->SetImmuneToNPC(false);
        b->SetReactState(REACT_AGGRESSIVE);
    }

    // --- Spawn roguelike vendor NPC at entrance ---
    if (session->RoguelikeRunId != 0 && sDMConfig->IsRoguelikeVendorEnabled())
    {
        static constexpr uint32 DM_VENDOR_NPC_ENTRY = 500001;

        // Small offset from entrance so vendor doesn't overlap player spawn point
        float vendorX = session->EntrancePos.GetPositionX() + 3.0f;
        float vendorY = session->EntrancePos.GetPositionY() + 2.0f;
        float vendorZ = session->EntrancePos.GetPositionZ();
        float vendorO = session->EntrancePos.GetOrientation();

        Creature* vendor = map->SummonCreature(DM_VENDOR_NPC_ENTRY,
            { vendorX, vendorY, vendorZ, vendorO });
        if (vendor)
        {
            vendor->SetFaction(35);           // friendly to all
            vendor->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
            vendor->SetImmuneToPC(true);
            vendor->SetImmuneToNPC(true);
            vendor->SetWanderDistance(0.0f);
            vendor->SetDefaultMovementType(IDLE_MOTION_TYPE);
            vendor->GetMotionMaster()->MoveIdle();
            vendor->setActive(true);
            vendor->UpdateObjectVisibility(true);

            // Track for cleanup — ClearDungeonCreatures() will despawn it
            guidList.push_back(vendor->GetGUID());

            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Spawned roguelike vendor at ({:.1f}, {:.1f}, {:.1f}) for session {}",
                vendorX, vendorY, vendorZ, session->SessionId);
        }
        else
        {
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Failed to spawn roguelike vendor for session {}",
                session->SessionId);
        }
    }
}

// Select a creature matching the theme, filtered to the session's level band.
// bandMin/bandMax come from session->LevelBandMin/Max so trash levels stay
// close to the players' level. The level band is a hard constraint: relax the
// theme if necessary, but never spawn an out-of-band template.
uint32 DungeonMasterMgr::SelectCreatureForTheme(uint32 mapId, const Theme* theme, bool isBoss,
                                                  uint8 bandMin, uint8 bandMax)
{
    if (!theme) return 0;

    std::set<uint32> types;
    bool anyType = false;
    for (uint32 t : theme->CreatureTypes)
    {
        if (t == uint32(-1)) anyType = true;
        else types.insert(t);
    }

    auto typeMatch = [&](uint32 cType) -> bool
    {
        return anyType || types.count(cType);
    };

    // Returns true when a pool entry overlaps the requested level band.
    auto levelMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return e.MinLevel <= bandMax && e.MaxLevel >= bandMin;
    };

    auto strictBandMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return e.MinLevel >= bandMin && e.MaxLevel <= bandMax;
    };

    uint8 bandWidth = (bandMax >= bandMin) ? (bandMax - bandMin) : 0;
    uint8 maxSpan = std::max<uint8>(5, std::min<uint8>(10, static_cast<uint8>(bandWidth + 2)));

    auto narrowOverlapMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return levelMatch(e) && (e.MaxLevel - e.MinLevel) <= maxSpan;
    };

    struct WeightedCandidate
    {
        uint32 Entry = 0;
        uint32 Type = 0;
        double Weight = 1.0;
    };

    std::vector<WeightedCandidate> candidates;

    auto profileIt = _dungeonThemeProfiles.find(mapId);
    DungeonThemeProfile const* profile = profileIt != _dungeonThemeProfiles.end() ? &profileIt->second : nullptr;

    auto getTypeWeight = [&](uint32 type, bool requireTheme) -> double
    {
        uint32 nativeCount = 0;
        if (profile)
            if (auto countIt = profile->NativeTypeCounts.find(type); countIt != profile->NativeTypeCounts.end())
                nativeCount = countIt->second;

        double nativeBias = nativeCount > 0 ? 1.0 + std::min<double>(3.0, std::log1p(static_cast<double>(nativeCount)) / 2.0) : 0.65;
        if (!requireTheme && nativeCount == 0)
            nativeBias *= 0.75;
        return nativeBias;
    };

    auto collect = [&](const std::unordered_map<uint32, std::vector<CreaturePoolEntry>>& pool,
                       bool requireTheme, auto&& match)
    {
        for (const auto& [type, vec] : pool)
        {
            if (requireTheme && !typeMatch(type))
                continue;

            for (const auto& e : vec)
                if (match(e))
                    candidates.push_back({ e.Entry, type, getTypeWeight(type, requireTheme) });
        }
    };

    if (isBoss)
    {
        collect(_bossCreatures, true, strictBandMatch);
        if (candidates.empty() && !anyType)
            collect(_bossCreatures, false, strictBandMatch);

        if (candidates.empty())
            collect(_bossCreatures, true, narrowOverlapMatch);
        if (candidates.empty() && !anyType)
            collect(_bossCreatures, false, narrowOverlapMatch);

        if (candidates.empty())
            collect(_creaturesByType, true, strictBandMatch);   // promote trash within band
        if (candidates.empty() && !anyType)
            collect(_creaturesByType, false, strictBandMatch);

        if (candidates.empty())
            collect(_creaturesByType, true, narrowOverlapMatch);
        if (candidates.empty() && !anyType)
            collect(_creaturesByType, false, narrowOverlapMatch);

        if (candidates.empty())
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No in-band boss candidate for theme '{}' in band {}-{}.",
                theme->Name, bandMin, bandMax);
    }
    else
    {
        collect(_creaturesByType, true, strictBandMatch);
        if (candidates.empty() && !anyType)
        {
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No strict in-band '{}' creatures found in band {}-{}; falling back to any type within band.",
                theme->Name, bandMin, bandMax);
            collect(_creaturesByType, false, strictBandMatch);
        }

        if (candidates.empty())
            collect(_creaturesByType, true, narrowOverlapMatch);
        if (candidates.empty() && !anyType)
        {
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No narrow in-band '{}' creatures found in band {}-{}; falling back to any type within band.",
                theme->Name, bandMin, bandMax);
            collect(_creaturesByType, false, narrowOverlapMatch);
        }

        if (candidates.empty())
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No in-band trash candidate for theme '{}' in band {}-{}.",
                theme->Name, bandMin, bandMax);
    }

    if (!candidates.empty())
    {
        double totalWeight = 0.0;
        for (WeightedCandidate const& candidate : candidates)
            totalWeight += candidate.Weight;

        if (totalWeight <= 0.0)
            return candidates[RandInt<size_t>(0, candidates.size() - 1)].Entry;

        double roll = std::uniform_real_distribution<double>(0.0, totalWeight)(tRng);
        for (WeightedCandidate const& candidate : candidates)
        {
            if (roll <= candidate.Weight)
            {
                LOG_DEBUG(DM_LOG_CATEGORY,
                    "DungeonMaster: {} weighted candidates for theme '{}' on map {} (boss={}, band {}-{}) -> picked entry {} type {} weight={:.2f}",
                    candidates.size(), theme->Name, mapId, isBoss, bandMin, bandMax,
                    candidate.Entry, candidate.Type, candidate.Weight);
                return candidate.Entry;
            }

            roll -= candidate.Weight;
        }

        return candidates.back().Entry;
    }

    LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: ZERO candidates for theme '{}' on map {} (boss={})",
        theme->Name, mapId, isBoss);
    return 0;
}


uint32 DungeonMasterMgr::SelectDungeonBoss(uint32 mapId, const Theme* theme, uint8 bandMin, uint8 bandMax)
{
    if (!theme) return 0;

    std::set<uint32> types;
    bool anyType = false;
    for (uint32 t : theme->CreatureTypes)
    {
        if (t == uint32(-1)) anyType = true;
        else types.insert(t);
    }

    auto typeMatch = [&](uint32 cType) -> bool
    {
        return anyType || types.count(cType);
    };

    auto levelMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return e.MinLevel <= bandMax && e.MaxLevel >= bandMin;
    };

    auto strictBandMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return e.MinLevel >= bandMin && e.MaxLevel <= bandMax;
    };

    uint8 bandWidth = (bandMax >= bandMin) ? (bandMax - bandMin) : 0;
    uint8 maxSpan = std::max<uint8>(5, std::min<uint8>(10, static_cast<uint8>(bandWidth + 2)));

    auto narrowOverlapMatch = [&](const CreaturePoolEntry& e) -> bool
    {
        return levelMatch(e) && (e.MaxLevel - e.MinLevel) <= maxSpan;
    };

    std::vector<uint32> candidates;

    auto collectCandidates = [&](auto const& poolByType, bool requireTheme, auto&& predicate)
    {
        for (const auto& [type, vec] : poolByType)
        {
            if (requireTheme && !typeMatch(type))
                continue;

            for (CreaturePoolEntry const& entry : vec)
                if (predicate(entry))
                    candidates.push_back(entry.Entry);
        }
    };

    std::string selectionSource;
    auto tryStage = [&](char const* label, auto const& poolByType, bool requireTheme, auto&& predicate) -> bool
    {
        candidates.clear();
        collectCandidates(poolByType, requireTheme, predicate);
        if (candidates.empty())
            return false;

        selectionSource = label;
        return true;
    };

    auto mapIt = _dungeonBossPoolByMap.find(mapId);
    bool haveMapPool = mapIt != _dungeonBossPoolByMap.end();

    if (haveMapPool && tryStage("map-native themed strict", mapIt->second, true, strictBandMatch))
    {
    }
    else if (haveMapPool && tryStage("map-native themed overlap", mapIt->second, true, narrowOverlapMatch))
    {
    }
    else if (haveMapPool && !anyType && tryStage("map-native relaxed strict", mapIt->second, false, strictBandMatch))
    {
    }
    else if (haveMapPool && !anyType && tryStage("map-native relaxed overlap", mapIt->second, false, narrowOverlapMatch))
    {
    }
    else if (tryStage("global themed strict", _dungeonBossPool, true, strictBandMatch))
    {
    }
    else if (tryStage("global themed overlap", _dungeonBossPool, true, narrowOverlapMatch))
    {
    }
    else if (!anyType && tryStage("global relaxed strict", _dungeonBossPool, false, strictBandMatch))
    {
    }
    else if (!anyType && tryStage("global relaxed overlap", _dungeonBossPool, false, narrowOverlapMatch))
    {
    }
    else
    {
        LOG_WARN(DM_LOG_CATEGORY,
            "DungeonMaster: No dungeon-boss candidate for map {}, theme '{}', band {}-{} (mapPool={}, themeMode={}); falling back to generic in-band boss selection.",
            mapId, theme->Name, bandMin, bandMax, haveMapPool ? "yes" : "no", anyType ? "any-type" : "typed");
        return SelectCreatureForTheme(mapId, theme, true, bandMin, bandMax);
    }

    uint32 entry = candidates[RandInt<size_t>(0, candidates.size() - 1)];
    LOG_DEBUG(DM_LOG_CATEGORY,
        "DungeonMaster: Selected dungeon boss entry {} from {} candidates via {} (map {}, theme '{}', band {}-{})",
        entry, candidates.size(), selectionSource, mapId, theme->Name, bandMin, bandMax);
    return entry;
}

// Death handling
void DungeonMasterMgr::HandleCreatureDeath(Creature* creature, ObjectGuid playerGuid)
{
    if (!creature)
        return;

    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);

    uint32 sessionId = 0;
    Session sessionSnapshot;
    bool haveSessionSnapshot = false;
    bool shouldFillLoot = false;
    bool shouldCreatePhaseCheck = false;
    bool isBoss = false;

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);

        auto playerIt = _playerToSession.find(playerGuid);
        if (playerIt == _playerToSession.end())
            return;

        sessionId = playerIt->second;
        auto sessionIt = _activeSessions.find(sessionId);
        if (sessionIt == _activeSessions.end())
            return;

        Session& session = sessionIt->second;
        if (!session.IsActive() || creature->GetMapId() != session.MapId)
            return;

        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: HandleCreatureDeath called for {} (GUID: {}) in session {}",
            creature->GetName(), creature->GetGUID().GetCounter(), session.SessionId);

        for (auto& sc : session.SpawnedCreatures)
        {
            if (sc.Guid != creature->GetGUID())
                continue;

            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Processing death for {} (Boss: {}, Elite: {}, LootFilled: {}, KillCredited: {})",
                creature->GetName(), sc.IsBoss, sc.IsElite, sc.LootFilled, sc.KillCredited);

            sc.IsDead = true;
            isBoss = sc.IsBoss;

            if (!sc.LootFilled)
            {
                sc.LootFilled = true;
                shouldFillLoot = true;
            }

            if (!sc.KillCredited)
            {
                sc.KillCredited = true;
                GiveKillXP(&session, sc.IsBoss, sc.IsElite);

                if (sc.IsBoss)
                {
                    shouldCreatePhaseCheck = true;
                }
                else
                {
                    ++session.MobsKilled;
                    for (auto& pd : session.Players)
                        ++pd.MobsKilled;
                }
            }

            sessionSnapshot = session;
            haveSessionSnapshot = true;

            break;
        }
    }

    if (shouldFillLoot)
        FillCreatureLoot(creature, &sessionSnapshot, isBoss);

    if (shouldCreatePhaseCheck && haveSessionSnapshot)
    {
        PendingPhaseCheck phaseCheck = CreatePendingPhaseCheck(sessionSnapshot, creature, nullptr, creature->GetEntry());

        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto sessionIt = _activeSessions.find(sessionId);
        if (sessionIt != _activeSessions.end())
        {
            Session& liveSession = sessionIt->second;
            for (auto const& sc : liveSession.SpawnedCreatures)
            {
                if (sc.Guid == creature->GetGUID() && sc.IsBoss && sc.IsDead)
                {
                    liveSession.PendingPhaseChecks.push_back(std::move(phaseCheck));
                    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Boss '{}' died — deferred phase check queued for session {}",
                        creature->GetName(), liveSession.SessionId);
                    break;
                }
            }
        }
    }
}

void DungeonMasterMgr::HandleBossDeath(Session* session)
{
    if (!session) return;
    for (const auto& pd : session->Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
        {
            if (session->BossesKilled < session->TotalBosses && p->GetSession())
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "|cFFFFFF00[Dungeon Master]|r Boss defeated! |cFFFFFFFF%u|r remaining.",
                    session->TotalBosses - session->BossesKilled);
                ChatHandler(p->GetSession()).SendSysMessage(buf);
            }
        }
}

    // Called from JustDied hook — fills loot before corpse is opened
void DungeonMasterMgr::OnCreatureDeathHook(Creature* creature)
{
    if (!creature) return;

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: OnCreatureDeathHook called for {} (GUID: {})",
        creature->GetName(), creature->GetGUID().GetCounter());

    std::lock_guard<std::mutex> lock(_sessionMutex);

    for (auto& [sid, session] : _activeSessions)
    {
        if (!session.IsActive())
            continue;
        if (creature->GetMapId() != session.MapId)
            continue;

        for (auto& sc : session.SpawnedCreatures)
        {
            if (sc.Guid == creature->GetGUID())
            {
                if (sc.IsDead)
                {
                    LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: OnCreatureDeathHook - creature {} already marked as dead",
                        creature->GetGUID().GetCounter());
                    return;
                }

                sc.IsDead = true;
                LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: OnCreatureDeathHook marked {} dead (Boss: {}, Elite: {})",
                    creature->GetName(), sc.IsBoss, sc.IsElite);

                LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Creature {} (entry {}) death marked via JustDied "
                    "(session {}, boss={}). Loot and kill credit deferred to OnUnitDeath/Update.",
                    creature->GetGUID().ToString(), creature->GetEntry(),
                    sid, sc.IsBoss);
                return;
            }
        }
    }
}

void DungeonMasterMgr::HandlePlayerDeath(Player* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);

    uint32 sessionId = 0;
    Session sessionSnapshot;

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto playerIt = _playerToSession.find(player->GetGUID());
        if (playerIt == _playerToSession.end())
            return;

        sessionId = playerIt->second;
        auto sessionIt = _activeSessions.find(sessionId);
        if (sessionIt == _activeSessions.end())
            return;

        Session& session = sessionIt->second;
        if (!session.IsActive() || player->GetMapId() != session.MapId)
            return;

        if (PlayerSessionData* pd = session.GetPlayerData(player->GetGUID()))
            ++pd->Deaths;

        sessionSnapshot = session;
    }

    // Block release-spirit; auto-rez instead
    player->SetFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
    player->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_RELEASE_TIMER);

    if (sessionSnapshot.IsPartyWiped())
    {
        Session liveSession;
        bool shouldHandleWipe = false;

        {
            std::lock_guard<std::mutex> lock(_sessionMutex);
            auto sessionIt = _activeSessions.find(sessionId);
            if (sessionIt == _activeSessions.end())
                return;

            Session& session = sessionIt->second;
            if (!session.IsActive())
                return;

            ++session.Wipes;

            session.State = SessionState::Failed;
            session.EndTime = GameTime::GetGameTime().count();
            liveSession = session;
            shouldHandleWipe = true;
        }

        if (!shouldHandleWipe)
            return;

        if (liveSession.RoguelikeRunId != 0)
        {
            sRoguelikeMgr->OnPartyWipe(liveSession.RoguelikeRunId);
            return;
        }

        for (const auto& psd : liveSession.Players)
        {
            Player* p = ObjectAccessor::FindPlayer(psd.PlayerGuid);
            if (!p) continue;
            p->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            if (!p->IsAlive()) { p->ResurrectPlayer(1.0f); p->SpawnCorpseBones(); }
            if (p->GetSession())
                ChatHandler(p->GetSession()).SendSysMessage(
                    "|cFFFF0000[Dungeon Master]|r Total party wipe! Challenge failed.");
            p->TeleportTo(psd.ReturnMapId, psd.ReturnPosition.GetPositionX(),
                psd.ReturnPosition.GetPositionY(), psd.ReturnPosition.GetPositionZ(),
                psd.ReturnPosition.GetOrientation());
        }
    }
    else
    {
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFFFF00[Dungeon Master]|r You have fallen! "
                "You will be revived when your group leaves combat.");
    }
}

// Rewards
uint32 DungeonMasterMgr::CalculateCompletionGold(Session const* session) const
{
    if (!session)
        return 0;

    DifficultyTier const* diff = sDMConfig->GetDifficulty(session->DifficultyId);
    if (!diff)
        return 0;

    uint64 baseGold = sDMConfig->GetBaseGold();
    uint64 mobGold = static_cast<uint64>(session->MobsKilled) * sDMConfig->GetGoldPerMob();
    uint64 bossGold = static_cast<uint64>(session->BossesKilled) * sDMConfig->GetGoldPerBoss();
    double scaledTotal = static_cast<double>(baseGold + mobGold + bossGold) * diff->RewardMultiplier;

    return static_cast<uint32>(std::min<uint64>(
        static_cast<uint64>(std::llround(scaledTotal)),
        std::numeric_limits<uint32>::max()));
}

uint32 DungeonMasterMgr::CalculateRoguelikeGoldReward(uint32 tier) const
{
    uint64 scaledGold = static_cast<uint64>(sDMConfig->GetBaseGold()) * std::max<uint32>(1u, tier);
    return static_cast<uint32>(std::min<uint64>(scaledGold, std::numeric_limits<uint32>::max()));
}

uint8 DungeonMasterMgr::RollCompletionRewardQuality() const
{
    uint32 epicChance = std::min<uint32>(sDMConfig->GetEpicChance(), 100u);
    uint32 rareChance = std::min<uint32>(sDMConfig->GetRareChance(), 100u);

    if (RandInt<uint32>(1, 100) <= epicChance)
        return 4;
    if (RandInt<uint32>(1, 100) <= rareChance)
        return 3;
    return 2;
}

void DungeonMasterMgr::DistributeRewards(Session* session)
{
    if (!session) return;
    const DifficultyTier* diff = sDMConfig->GetDifficulty(session->DifficultyId);
    if (!diff) return;

    GiveCompletionXP(session);

    uint32 lvl       = session->EffectiveLevel;
    uint64 baseGold  = sDMConfig->GetBaseGold();
    uint64 mobGold   = static_cast<uint64>(session->MobsKilled) * sDMConfig->GetGoldPerMob();
    uint64 bossGold  = static_cast<uint64>(session->BossesKilled) * sDMConfig->GetGoldPerBoss();
    uint32 total     = CalculateCompletionGold(session);
    uint32 perPlayer = total / std::max<uint32>(1, session->Players.size());

    uint8 rewardLevel = static_cast<uint8>(std::min<uint32>(lvl, 80));
    uint32 itemChance = std::min<uint32>(sDMConfig->GetItemChance(), 100u);

    LOG_INFO(DM_LOG_CATEGORY,
        "DungeonMaster: Session {} reward summary -> map={}, difficulty='{}', level={}, mobs={}, bosses={}, players={}, baseGold={}, mobGold={}, bossGold={}, rewardMult={:.2f}, totalGold={}, perPlayerGold={}, itemChance={}%, rareChance={}%, epicChance={}%, rewardPool={}",
        session->SessionId, session->MapId, diff->Name, lvl, session->MobsKilled, session->BossesKilled,
        session->Players.size(), baseGold, mobGold, bossGold, diff->RewardMultiplier, total, perPlayer,
        itemChance, sDMConfig->GetRareChance(), sDMConfig->GetEpicChance(), _rewardItems.size());

    for (const auto& pd : session->Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!p || !p->IsInWorld())
        {
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: Player {} not found/not in world for rewards", pd.PlayerGuid.GetCounter());
            continue;
        }

        // Gold goes directly to wallet
        GiveGoldReward(p, perPlayer);

        bool grantItem = RandInt<uint32>(1, 100) <= itemChance;
        if (!grantItem)
        {
            LOG_INFO(DM_LOG_CATEGORY,
                "DungeonMaster: Session {} reward -> player {} received {} copper and no completion item (item roll failed, chance={}%)",
                session->SessionId, p->GetName(), perPlayer, itemChance);
            continue;
        }

        uint8 quality = RollCompletionRewardQuality();
        LOG_INFO(DM_LOG_CATEGORY,
            "DungeonMaster: Session {} reward -> player {} received {} copper and completion item quality {}",
            session->SessionId, p->GetName(), perPlayer, quality);
        GiveItemReward(p, rewardLevel, quality);
    }
}


void DungeonMasterMgr::GiveCompletionXP(Session* session)
{
    if (!session)
        return;

    uint32 normalKills = 0;
    uint32 eliteKills = 0;
    uint32 bossKills = 0;

    for (SpawnedCreature const& creature : session->SpawnedCreatures)
    {
        if (!creature.IsDead)
            continue;

        if (creature.IsBoss)
            ++bossKills;
        else if (creature.IsElite)
            ++eliteKills;
        else
            ++normalKills;
    }

    uint32 xpWeight = normalKills + eliteKills * 2 + bossKills * 10;
    float completionMult = std::max(0.0f, sDMConfig->GetXPMultiplier());
    if (xpWeight == 0 || completionMult <= 0.0f)
        return;

    for (PlayerSessionData const& pd : session->Players)
    {
        Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (!player || !player->IsAlive())
            continue;
        if (player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            continue;

        uint8 level = player->GetLevel();
        sScriptMgr->OnPlayerBeforeGetLevelForXPGain(player, level);

        uint32 baseXP = (level * 5) + 45;
        uint32 xp = static_cast<uint32>(baseXP * completionMult * xpWeight * player->GetQuestRate(false));
        xp = static_cast<uint32>(xp * player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_QUEST_PCT));

        if (xp == 0)
            continue;

        sScriptMgr->OnPlayerGiveXP(player, xp, nullptr, PlayerXPSource::XPSOURCE_QUEST);
        player->GiveXP(xp, nullptr);

        if (Pet* pet = player->GetPet())
            pet->GivePetXP(player->GetGroup() ? xp / 2 : xp);
    }
}


void DungeonMasterMgr::GiveKillXP(Session* session, bool isBoss, bool isElite)
{
    (void)session;
    (void)isBoss;
    (void)isElite;
}

void DungeonMasterMgr::DespawnTrackedSessionCreatures(Session const& session, std::vector<ObjectGuid> const& trackedGuids)
{
    if (trackedGuids.empty())
        return;

    Player* referencePlayer = GetReferencePlayer(session);
    if (!referencePlayer)
        return;

    Map* map = referencePlayer->GetMap();
    if (!map || !map->IsDungeon())
        return;

    InstanceMap* instance = map->ToInstanceMap();
    if (!instance)
        return;

    if (session.InstanceId != 0 && instance->GetInstanceId() != session.InstanceId)
        return;

    uint32 despawned = 0;
    for (ObjectGuid const& guid : trackedGuids)
    {
        Creature* creature = instance->GetCreature(guid);
        if (!creature || !creature->IsInWorld())
            continue;

        creature->DespawnOrUnsummon();
        ++despawned;
    }

    if (despawned > 0)
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} — pre-teardown despawned {} tracked creatures from map {} (inst {})",
            session.SessionId, despawned, instance->GetId(), instance->GetInstanceId());
}

void DungeonMasterMgr::GiveGoldReward(Player* player, uint32 amount)
{
    if (!player || !amount) return;
    player->ModifyMoney(amount);

    if (player->GetSession())
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "|cFFFFD700[Dungeon Master]|r You received: |cFFFFD700%u|rg |cFFC0C0C0%u|rs |cFFB87333%u|rc",
            amount / 10000, (amount % 10000) / 100, amount % 100);
        ChatHandler(player->GetSession()).SendSysMessage(buf);
    }
}

void DungeonMasterMgr::GiveItemReward(Player* player, uint8 level, uint8 quality)
{
    uint32 playerClass = player->getClass();
    uint32 itemEntry = SelectRewardItem(level, quality, playerClass);

    // Quality fallback: if requested quality isn't found, try lower qualities
    // but still maintain level appropriateness
    if (!itemEntry && quality > 2)
    {
        LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: No quality {} items for level {}, class {}. Trying lower quality...",
            quality, level, playerClass);
        for (uint8 q = quality - 1; q >= 2 && !itemEntry; --q)
            itemEntry = SelectRewardItem(level, q, playerClass);
    }

    if (!itemEntry)
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: No suitable reward item for player {} (level {}, class {}, quality {}). "
            "Reward pool has {} items total. Gold only.",
            player->GetName(), level, playerClass, quality, _rewardItems.size());
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cFFFF0000[Dungeon Master]|r No suitable gear found for your level and class. Gold only.");
        return;
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Giving item {} to {} (level {}, quality {}, class {})",
        itemEntry, player->GetName(), level, quality, playerClass);

    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) == EQUIP_ERR_OK)
    {
        if (Item* item = player->StoreNewItem(dest, itemEntry, true))
        {
            player->SendNewItem(item, 1, true, false);
            if (const ItemTemplate* t = sObjectMgr->GetItemTemplate(itemEntry))
            {
                if (player->GetSession())
                {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "|cFFFFD700[Dungeon Master]|r You received: |cFFFFFFFF%s|r", t->Name1.c_str());
                    ChatHandler(player->GetSession()).SendSysMessage(buf);
                }
            }
        }
    }
    else
    {
        // Bags full — mail the item instead.
        Item* mailItem = Item::CreateItem(itemEntry, 1, player);
        if (mailItem)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            MailDraft("Dungeon Master Reward", "Your bags were full. Here is your reward!")
                .AddItem(mailItem)
                .SendMailTo(trans,
                    MailReceiver(player, player->GetGUID().GetCounter()),
                    MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
            CharacterDatabase.CommitTransaction(trans);
            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cFFFFD700[Dungeon Master]|r Bags full! Reward mailed to you.");
        }
        else
        {
            LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: Failed to create mail item {} for {}", itemEntry, player->GetName());
        }
    }
}

    // Mail a reward item to player
void DungeonMasterMgr::MailItemReward(Player* player, uint8 level, uint8 quality,
                                       const std::string& subject, const std::string& body)
{
    if (!player || !player->IsInWorld()) return;

    uint32 playerClass = player->getClass();
    uint32 itemEntry = SelectRewardItem(level, quality, playerClass);

    // Quality fallback
    if (!itemEntry && quality > 2)
    {
        for (uint8 q = quality - 1; q >= 2 && !itemEntry; --q)
            itemEntry = SelectRewardItem(level, q, playerClass);
    }

    // Level window fallback
    if (!itemEntry)
    {
        uint8 windows[] = { 15, 25, 80 };
        for (uint8 w : windows)
        {
            uint8 lo = (level > w) ? level - w : 1;
            uint8 hi = std::min<uint8>(level + w, 80);
            for (const auto& ri : _rewardItems)
            {
                if (ri.Quality < 2 || ri.Quality > 4) continue;
                if (ri.MinLevel < lo || ri.MinLevel > hi) continue;
                if (ri.AllowableClass != -1 && !(ri.AllowableClass & (1 << (playerClass - 1))))
                    continue;
                itemEntry = ri.Entry;
                break;
            }
            if (itemEntry) break;
        }
    }

    if (!itemEntry)
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: No reward item found for mail to {} (level {}, class {})",
            player->GetName(), level, playerClass);
        return;
    }

    Item* mailItem = Item::CreateItem(itemEntry, 1, player);
    if (mailItem)
    {
        // Must use a real transaction (nullptr crashes SendMailTo)
        // inside SendMailTo when it calls trans->Append() on the null ptr.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        MailDraft(subject, body)
            .AddItem(mailItem)
            .SendMailTo(trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));

        CharacterDatabase.CommitTransaction(trans);

        if (const ItemTemplate* t = sObjectMgr->GetItemTemplate(itemEntry))
        {
            if (player->GetSession())
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cFFFFD700[Dungeon Master]|r Reward mailed: |cFFFFFFFF%s|r", t->Name1.c_str());
                ChatHandler(player->GetSession()).SendSysMessage(buf);
            }
        }
    }
    else
    {
        LOG_ERROR(DM_LOG_CATEGORY, "DungeonMaster: Failed to create mail item {} for {}", itemEntry, player->GetName());
    }
}


void DungeonMasterMgr::DistributeRoguelikeRewards(uint32 tier, uint8 effectiveLevel,
                                                    const std::vector<ObjectGuid>& playerGuids)
{
    uint8 rewardLevel = static_cast<uint8>(std::min<uint32>(effectiveLevel, 80));

    uint32 tierGold = CalculateRoguelikeGoldReward(tier);

    // Epic chance scales with tier
    uint32 epicChance = std::min<uint32>(5 + (tier * 5), 80);


    uint32 blueItems  = 1;
    uint32 greenItems = 0;
    uint32 epicItems  = 0;

    if (tier >= 9)      { blueItems = 3; epicItems = 1; }
    else if (tier >= 7) { blueItems = 2; epicItems = 1; }
    else if (tier >= 5) { blueItems = 2; }
    else if (tier >= 3) { blueItems = 1; greenItems = 1; }

    LOG_INFO(DM_LOG_CATEGORY,
        "DungeonMaster: Roguelike reward summary -> tier={}, level={}, players={}, configBaseGold={}, perPlayerGold={}, blue={}, green={}, guaranteedEpic={}, bonusEpicChance={}%",
        tier, rewardLevel, playerGuids.size(), sDMConfig->GetBaseGold(), tierGold,
        blueItems, greenItems, epicItems, epicChance);

    for (const auto& guid : playerGuids)
    {
        Player* p = ObjectAccessor::FindPlayer(guid);
        if (!p || !p->IsInWorld()) continue;

        // Gold
        GiveGoldReward(p, tierGold);

        LOG_INFO(DM_LOG_CATEGORY,
            "DungeonMaster: Roguelike reward -> player {} received {} copper at tier {}",
            p->GetName(), tierGold, tier);

        // Items go directly to inventory (mail fallback if bags full)

        // Guaranteed epic items
        for (uint32 i = 0; i < epicItems; ++i)
            GiveItemReward(p, rewardLevel, 4);

        // Roll for bonus epics
        if (epicItems == 0 && RandInt<uint32>(1, 100) <= epicChance)
            GiveItemReward(p, rewardLevel, 4);
        else if (epicItems > 0 && tier >= 9 && RandInt<uint32>(1, 100) <= 25)
            GiveItemReward(p, rewardLevel, 4);

        // Blue items
        for (uint32 i = 0; i < blueItems; ++i)
            GiveItemReward(p, rewardLevel, 3);

        // Green items
        for (uint32 i = 0; i < greenItems; ++i)
            GiveItemReward(p, rewardLevel, 2);

        if (p->GetSession())
            ChatHandler(p->GetSession()).SendSysMessage(
                "|cFF00FFFF[Roguelike]|r Rewards added to your inventory!");
    }
}

// Armor type by class
static uint8 GetMaxArmorSubclass(uint32 playerClass)
{
    switch (playerClass)
    {
        case 5: case 8: case 9:              return 1;  // cloth: Priest, Mage, Warlock
        case 4: case 11:                     return 2;  // leather: Rogue, Druid
        case 3: case 7:                      return 3;  // mail: Hunter, Shaman
        case 1: case 2: case 6:              return 4;  // plate: Warrior, Paladin, DK
        default:                             return 4;
    }
}

static uint32 GetClassBitmask(uint32 playerClass)
{
    if (playerClass == 0 || playerClass > 11) return 0x7FF;  // all classes
    return 1 << (playerClass - 1);
}

// Primary stat for class-based reward weighting
// Returns: ITEM_MOD_AGILITY(3), ITEM_MOD_STRENGTH(4), ITEM_MOD_INTELLECT(5)
static uint32 GetPrimaryStatForClass(uint32 playerClass)
{
    switch (playerClass)
    {
        case 1:  return 4;  // Warrior  -> STR
        case 2:  return 4;  // Paladin  -> STR
        case 3:  return 3;  // Hunter   -> AGI
        case 4:  return 3;  // Rogue    -> AGI
        case 5:  return 5;  // Priest   -> INT
        case 6:  return 4;  // DK       -> STR
        case 7:  return 5;  // Shaman   -> INT
        case 8:  return 5;  // Mage     -> INT
        case 9:  return 5;  // Warlock  -> INT
        case 11: return 3;  // Druid    -> AGI
        default: return 4;
    }
}

// Score item stat alignment with player class (0.0 = bad, 1.0 = perfect match)
static float ScoreItemForClass(uint32 itemEntry, uint32 playerClass)
{
    const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto) return 0.0f;

    uint32 primaryStat = GetPrimaryStatForClass(playerClass);
    float totalStats = 0.0f;
    float primaryTotal = 0.0f;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        int32 val = proto->ItemStat[i].ItemStatValue;
        uint32 type = proto->ItemStat[i].ItemStatType;
        if (val <= 0) continue;

        totalStats += static_cast<float>(val);
        if (type == primaryStat)
            primaryTotal += static_cast<float>(val);
    }

    if (totalStats <= 0.0f) return 0.5f;  // No stats (trinket, etc.) = neutral
    return primaryTotal / totalStats;
}

uint32 DungeonMasterMgr::SelectRewardItem(uint8 level, uint8 quality, uint32 playerClass)
{
    uint8  maxArmor   = GetMaxArmorSubclass(playerClass);
    uint32 classMask  = GetClassBitmask(playerClass);

    // Try progressively wider level windows, but always prefer closer to player level
    struct { uint8 below; uint8 above; } windows[] = {
        { 3, 0 },    // strict: [level-3, level]
        { 8, 0 },    // medium: [level-8, level]
        { 15, 0 },   // wide: [level-15, level]
        { 25, 0 },   // very wide: [level-25, level]
        { 80, 0 },   // last resort: [1, level] (never items above player level)
    };

    for (const auto& win : windows)
    {
        std::vector<uint32> cands;
        uint8 lo = (level > win.below) ? (level - win.below) : 1;
        uint8 hi = level;  // Never give items above player level

        for (const auto& ri : _rewardItems)
        {
            // Quality filter
            if (ri.Quality != quality) continue;

            // Level filter: item RequiredLevel must be within window
            if (ri.MinLevel < lo || ri.MinLevel > hi) continue;

            // Class restriction: AllowableClass bitmask check
            if (ri.AllowableClass != -1 && !(ri.AllowableClass & classMask))
                continue;

            // Armor subclass: player can only wear their class's max armor or lower
            if (ri.Class == 4 && ri.SubClass > 0 && ri.SubClass <= 4)
            {
                if (ri.SubClass > maxArmor) continue;
            }

            cands.push_back(ri.Entry);
        }

        if (!cands.empty())
        {
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: SelectRewardItem(level={}, quality={}, class={}) "
                "-> {} candidates in window [{}, {}]",
                level, quality, playerClass, cands.size(), lo, hi);

            // 75% chance: bias toward items with matching primary stat
            if (cands.size() > 3 && playerClass > 0 && RandInt<uint32>(1, 100) <= 75)
            {
                std::vector<std::pair<uint32, float>> scored;
                scored.reserve(cands.size());
                for (uint32 entry : cands)
                    scored.push_back({entry, ScoreItemForClass(entry, playerClass)});

                std::sort(scored.begin(), scored.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                // Pick from the top third (at least 3 items)
                size_t topN = std::max<size_t>(3, scored.size() / 3);
                return scored[RandInt<size_t>(0, topN - 1)].first;
            }

            // 25% chance: purely random from valid pool
            return cands[RandInt<size_t>(0, cands.size() - 1)];
        }
    }

    LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: SelectRewardItem(level={}, quality={}, class={}) "
        "-> NO candidates found in reward pool ({} items total)",
        level, quality, playerClass, _rewardItems.size());

    return 0;
}

uint32 DungeonMasterMgr::SelectLootItem(uint8 level, uint8 minQuality, uint8 maxQuality,
                                        bool equipmentOnly, uint32 playerClass)
{
    // Expected ItemLevel range for this level
    uint16 expectedMaxIlvl = static_cast<uint16>(level) * 2 + 10;

    uint8  maxArmor  = playerClass ? GetMaxArmorSubclass(playerClass) : 4;
    uint32 classMask = playerClass ? GetClassBitmask(playerClass) : 0x7FF;

    // Progressively widen level windows, always preferring items closer to player level
    struct { uint8 below; uint8 above; } windows[] = {
        { 3, 1 },   // strict: RequiredLevel in [level-3, level+1]
        { 5, 2 },   // medium
        { 8, 3 },   // wide
        { 15, 5 },  // very wide
        { 25, 8 },  // extremely wide (last resort)
    };

    for (const auto& win : windows)
    {
        uint8 lo = (level > win.below) ? (level - win.below) : 0;
        uint8 hi = std::min<uint16>(level + win.above, 83);

        std::vector<uint32> cands;
        for (const auto& li : _lootPool)
        {
            if (li.Quality < minQuality || li.Quality > maxQuality) continue;
            if (equipmentOnly && li.ItemClass != 2 && li.ItemClass != 4) continue;

            // Level filter for items with RequiredLevel > 0
            if (li.MinLevel > 0)
            {
                if (li.MinLevel < lo || li.MinLevel > hi) continue;
            }
            else
            {
                // RequiredLevel = 0: use ItemLevel as a sanity check
                if (li.ItemLevel > expectedMaxIlvl) continue;
            }

            // Class restriction for equipment items
            if (equipmentOnly || (li.ItemClass == 2 || li.ItemClass == 4))
            {
                // AllowableClass bitmask check
                if (li.AllowableClass != -1 && !(li.AllowableClass & classMask))
                    continue;

                // Armor subclass check (only for armor, not weapons)
                if (li.ItemClass == 4 && li.SubClass > 0 && li.SubClass <= 4)
                    if (li.SubClass > maxArmor) continue;
            }

            cands.push_back(li.Entry);
        }

        if (!cands.empty())
        {
            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: SelectLootItem(level={}, quality={}-{}, eqOnly={}, class={}) "
                "-> {} candidates in window [{}, {}]",
                level, minQuality, maxQuality, equipmentOnly, playerClass, cands.size(), lo, hi);

            // Bias equipment loot toward matching primary stat (75% chance)
            if (equipmentOnly && playerClass > 0 && cands.size() > 3
                && RandInt<uint32>(1, 100) <= 75)
            {
                std::vector<std::pair<uint32, float>> scored;
                scored.reserve(cands.size());
                for (uint32 entry : cands)
                    scored.push_back({entry, ScoreItemForClass(entry, playerClass)});

                std::sort(scored.begin(), scored.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                size_t topN = std::max<size_t>(3, scored.size() / 3);
                return scored[RandInt<size_t>(0, topN - 1)].first;
            }

            return cands[RandInt<size_t>(0, cands.size() - 1)];
        }
    }

    LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: SelectLootItem(level={}, quality={}-{}, eqOnly={}, class={}) "
        "-> NO candidates found in loot pool ({} items total)",
        level, minQuality, maxQuality, equipmentOnly, playerClass, _lootPool.size());

    return 0;
}


void DungeonMasterMgr::FillCreatureLoot(Creature* creature, Session* session, bool isBoss)
{
    if (!creature || !session) return;

    Loot& loot = creature->loot;
    loot.clear();
    loot.sourceWorldObjectGUID = creature->GetGUID();

    uint8 level = session->EffectiveLevel;

    Player* lootOwner = creature->GetLootRecipient();
    Group* lootGroup = creature->GetLootRecipientGroup();
    if ((!lootOwner || !lootOwner->IsInWorld()) && !session->Players.empty())
    {
        for (auto const& pd : session->Players)
        {
            Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (player && player->IsInWorld() && player->GetMapId() == session->MapId)
            {
                lootOwner = player;
                break;
            }
        }

        if (!lootOwner)
        {
            for (auto const& pd : session->Players)
            {
                Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                if (player && player->IsInWorld())
                {
                    lootOwner = player;
                    break;
                }
            }
        }

        if (lootOwner)
            lootGroup = lootOwner->GetGroup();
    }

    if (lootGroup && lootGroup->GetLootMethod() != FREE_FOR_ALL)
    {
        lootGroup->UpdateLooterGuid(creature, true);
        if (ObjectGuid groupLooterGuid = lootGroup->GetLooterGuid())
            if (Player* groupLooter = ObjectAccessor::FindPlayer(groupLooterGuid))
                lootOwner = groupLooter;
    }

    if (lootOwner)
        loot.lootOwnerGUID = lootOwner->GetGUID();
    if (lootGroup && lootOwner && lootGroup->GetLootMethod() != FREE_FOR_ALL)
        loot.roundRobinPlayer = lootOwner->GetGUID();

    // Pick a random party member's class for loot filtering
    uint32 lootClass = 0;
    if (!session->Players.empty())
    {
        // Try to pick a random alive player's class
        std::vector<uint32> classes;
        for (const auto& pd : session->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (p && p->IsAlive())
                classes.push_back(p->getClass());
        }
        if (classes.empty())
        {
            // All dead? Just pick from any player
            for (const auto& pd : session->Players)
            {
                Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                if (p) { classes.push_back(p->getClass()); break; }
            }
        }
        if (!classes.empty())
            lootClass = classes[RandInt<size_t>(0, classes.size() - 1)];
    }

    // Gold drop
    uint32 baseGold = isBoss ? std::max<uint32>(500u, sDMConfig->GetGoldPerBoss())
                             : std::max<uint32>(25u, sDMConfig->GetGoldPerMob());
    loot.gold = std::max(500u, baseGold + RandInt<uint32>(0, baseGold / 3));

    // Item drops
    uint32 itemsAdded = 0;
    auto addItem = [&](uint8 minQ, uint8 maxQ, bool eqOnly) -> bool
    {
        uint32 entry = SelectLootItem(level, minQ, maxQ, eqOnly, eqOnly ? lootClass : 0);
        if (!entry)
        {
            LOG_WARN(DM_LOG_CATEGORY, "DungeonMaster: FillCreatureLoot failed to find item (level={}, quality={}-{}, eqOnly={}, class={})",
                level, minQ, maxQ, eqOnly, lootClass);
            return false;
        }

        LootStoreItem storeItem(entry, 0, 100.0f, false, 1, 0, 1, 1);
        loot.AddItem(storeItem);
        ++itemsAdded;
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Added loot item {} (quality {}-{}) to {} (boss={})",
            entry, minQ, maxQ, creature->GetName(), isBoss);
        return true;
    };

    if (isBoss)
    {
        // Boss: 2 guaranteed rare (blue) equipment pieces
        if (!addItem(3, 3, true))
            addItem(2, 3, true);   // fallback to green/blue if no rare at this level
        if (!addItem(3, 3, true))
            addItem(2, 3, true);
    }
    else
    {
        bool isElite = false;
        bool isRare  = false;
        for (const auto& sc : session->SpawnedCreatures)
        {
            if (sc.Guid == creature->GetGUID())
            {
                isElite = sc.IsElite;
                isRare  = sc.IsRare;
                break;
            }
        }

        if (isRare)
        {
            // Rare spawn: guaranteed blue equipment piece
            if (!addItem(3, 3, true))
                addItem(2, 3, true);
        }
        else if (isElite)
        {
            // Elite: 40% chance of green equipment
            if (RandInt<uint32>(1, 100) <= 40)
            {
                if (!addItem(2, 2, true))
                    addItem(2, 2, false);
            }
        }
        else
        {
            // Trash: 15% grey/white junk, 3% green equipment
            if (RandInt<uint32>(1, 100) <= 15)
                addItem(0, 1, false);
            if (RandInt<uint32>(1, 100) <= 3)
                addItem(2, 2, true);
        }
    }

    // Ensure lootable flag is set (critical for boss loot)
    creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);

    // --- Group Loot Support (Need/Greed) ---
    // If the killing player is in a group with Group Loot or Need Before Greed,
    // trigger the group's loot distribution system for qualifying items.
    loot.loot_type = LOOT_CORPSE;
    Player* looter = lootOwner;
    Group*  group  = lootGroup;

    if (group && looter)
    {
        // Set both individual and group loot recipient so the group's loot
        // distribution system (Need/Greed/Pass) is properly triggered.
        creature->SetLootRecipient(looter, true);

        // Mark items above the group's loot threshold for rolling;
        // items below threshold become free-for-all (direct loot)
        uint8 threshold = group->GetLootThreshold();
        for (auto& item : loot.items)
        {
            const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item.itemid);
            if (!proto || proto->Quality < threshold)
                item.is_underthreshold = true;
        }

        // Trigger the active group loot distribution mode so the corpse's
        // internal blocked / threshold state matches the group's permissions.
        switch (group->GetLootMethod())
        {
            case GROUP_LOOT:
                group->GroupLoot(&loot, creature);
                break;
            case NEED_BEFORE_GREED:
                group->NeedBeforeGreed(&loot, creature);
                break;
            case MASTER_LOOT:
                group->MasterLoot(&loot, creature);
                break;
            default:
                break;
        }

        // Force dynamic flag update to all session players so they see the lootable corpse
        for (const auto& pd : session->Players)
        {
            Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (p && p->IsInWorld() && p->GetMapId() == session->MapId)
                creature->SendUpdateToPlayer(p);
        }

        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Group loot triggered for {} — {} items, "
            "lootMethod={}, threshold={}, groupSize={}",
            creature->GetName(), loot.items.size(),
            static_cast<int>(group->GetLootMethod()), threshold, group->GetMembersCount());
    }
    else if (looter)
    {
        creature->SetLootRecipient(looter);
    }
    
    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: FillCreatureLoot complete for {} (GUID: {}, Boss: {}, Level: {}, Gold: {}, Items: {})",
        creature->GetName(), creature->GetGUID().GetCounter(), isBoss, level, loot.gold, itemsAdded);
}

// Session end / cleanup
void DungeonMasterMgr::EndSession(uint32 sessionId, bool success)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);

    Session session;
    uint32 instanceId = 0;
    std::vector<ObjectGuid> trackedGuids;
    {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto it = _activeSessions.find(sessionId);
        if (it == _activeSessions.end())
            return;

        session = it->second;
        instanceId = session.InstanceId;

        if (instanceId != 0)
        {
            if (auto guidIt = _instanceCreatureGuids.find(instanceId); guidIt != _instanceCreatureGuids.end())
                trackedGuids = guidIt->second;

            _instanceToSession.erase(instanceId);
            _instanceCreatureGuids.erase(instanceId);
        }

        for (const auto& pd : session.Players)
            _playerToSession.erase(pd.PlayerGuid);

        _activeSessions.erase(it);
    } // lock released before any teleports or hook-triggering operations

    if (session.RoguelikeRunId != 0)
    {
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: EndSession {} — roguelike run {}, delegating to RoguelikeMgr.",
            sessionId, session.RoguelikeRunId);

        UpdatePlayerStatsFromSession(session, success);
        sRoguelikeMgr->EndRun(session.RoguelikeRunId, false);
        return;
    }

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: EndSession {} — success={}, state={}, players={}",
        sessionId, success, static_cast<int>(session.State), session.Players.size());

    for (const auto& pd : session.Players)
        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
            if (p->GetSession())
                ChatHandler(p->GetSession()).SendSysMessage(
                    success ? "|cFF00FF00[Dungeon Master]|r Challenge complete! Distributing rewards..."
                            : "|cFFFF0000[Dungeon Master]|r Challenge ended. No rewards given.");

    if (success && session.State == SessionState::Completed)
        DistributeRewards(&session);

    UpdatePlayerStatsFromSession(session, success);
    if (success && session.State == SessionState::Completed)
        SaveLeaderboardEntry(session);

    DespawnTrackedSessionCreatures(session, trackedGuids);
    TeleportPartyOut(&session);
    CleanupSession(session);

    for (const auto& pd : session.Players)
        SetCooldown(pd.PlayerGuid);
}

void DungeonMasterMgr::AbandonSession(uint32 id) { EndSession(id, false); }


void DungeonMasterMgr::CleanupRoguelikeSession(uint32 sessionId, bool success)
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _activeSessions.find(sessionId);
    if (it == _activeSessions.end()) return;

    Session& s = it->second;


    UpdatePlayerStatsFromSession(s, success);
    if (success && s.State == SessionState::Completed)
        SaveLeaderboardEntry(s);


    uint32 savedInstanceId = s.InstanceId;

    // Clean up mappings (no teleport/cooldowns for roguelike)
    if (savedInstanceId != 0)
    {
        _instanceToSession.erase(savedInstanceId);
        _instanceCreatureGuids.erase(savedInstanceId);
    }
    for (const auto& pd : s.Players)
        _playerToSession.erase(pd.PlayerGuid);

    _activeSessions.erase(it);

    LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Roguelike session {} cleaned up (success={}).",
        sessionId, success);
}

void DungeonMasterMgr::CleanupSession(Session& s) { s.InstanceId = 0; }

// Cooldowns
bool DungeonMasterMgr::IsOnCooldown(ObjectGuid g) const
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    auto it = _cooldowns.find(g);
    return it != _cooldowns.end()
        && GameTime::GetGameTime().count() < static_cast<time_t>(it->second);
}

void DungeonMasterMgr::SetCooldown(ObjectGuid g)
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    _cooldowns[g] = GameTime::GetGameTime().count() + sDMConfig->GetCooldownMinutes() * 60;
}

void DungeonMasterMgr::ClearCooldown(ObjectGuid g)
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    _cooldowns.erase(g);
}

uint32 DungeonMasterMgr::GetRemainingCooldown(ObjectGuid g) const
{
    std::lock_guard<std::mutex> lock(_cooldownMutex);
    auto it = _cooldowns.find(g);
    if (it == _cooldowns.end()) return 0;
    time_t now = GameTime::GetGameTime().count();
    return (now < static_cast<time_t>(it->second))
        ? static_cast<uint32>(it->second - now) : 0;
}

bool DungeonMasterMgr::CanCreateNewSession() const
{
    return _activeSessions.size() < sDMConfig->GetMaxConcurrentRuns();
}

// Player Statistics & Leaderboard

void DungeonMasterMgr::LoadAllPlayerStats()
{
    std::lock_guard<std::mutex> lock(_statsMutex);
    _playerStats.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, total_runs, completed_runs, failed_runs, "
        "total_mobs_killed, total_bosses_killed, total_deaths, fastest_clear "
        "FROM dm_player_stats");

    if (!result)
    {
        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: No player stats found (table may be empty or missing).");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 guidLow = f[0].Get<uint32>();

        PlayerStats ps;
        ps.TotalRuns        = f[1].Get<uint32>();
        ps.CompletedRuns    = f[2].Get<uint32>();
        ps.FailedRuns       = f[3].Get<uint32>();
        ps.TotalMobsKilled  = f[4].Get<uint32>();
        ps.TotalBossesKilled = f[5].Get<uint32>();
        ps.TotalDeaths      = f[6].Get<uint32>();
        ps.FastestClear     = f[7].Get<uint32>();

        _playerStats[guidLow] = ps;
        ++count;
    } while (result->NextRow());

    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Loaded stats for {} players.", count);
}

PlayerStats DungeonMasterMgr::GetPlayerStats(ObjectGuid guid) const
{
    std::lock_guard<std::mutex> lock(_statsMutex);
    uint32 guidLow = guid.GetCounter();
    auto it = _playerStats.find(guidLow);
    if (it != _playerStats.end())
        return it->second;
    return {};
}

void DungeonMasterMgr::SavePlayerStats(uint32 guidLow)
{
    PlayerStats ps;
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        auto it = _playerStats.find(guidLow);
        if (it == _playerStats.end()) return;
        ps = it->second;
    }

    char query[512];
    snprintf(query, sizeof(query),
        "REPLACE INTO dm_player_stats "
        "(guid, total_runs, completed_runs, failed_runs, "
        "total_mobs_killed, total_bosses_killed, total_deaths, fastest_clear) "
        "VALUES (%u, %u, %u, %u, %u, %u, %u, %u)",
        guidLow, ps.TotalRuns, ps.CompletedRuns, ps.FailedRuns,
        ps.TotalMobsKilled, ps.TotalBossesKilled, ps.TotalDeaths, ps.FastestClear);
    CharacterDatabase.Execute(query);
}

void DungeonMasterMgr::UpdatePlayerStatsFromSession(const Session& session, bool success)
{
    uint32 clearTime = 0;
    if (session.EndTime > session.StartTime)
        clearTime = static_cast<uint32>(session.EndTime - session.StartTime);
    else
        clearTime = static_cast<uint32>(GameTime::GetGameTime().count() - session.StartTime);

    for (const auto& pd : session.Players)
    {
        uint32 guidLow = pd.PlayerGuid.GetCounter();

        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            auto& ps = _playerStats[guidLow];
            ps.TotalRuns++;
            if (success)
            {
                ps.CompletedRuns++;
                if (ps.FastestClear == 0 || clearTime < ps.FastestClear)
                    ps.FastestClear = clearTime;
            }
            else
                ps.FailedRuns++;

            ps.TotalMobsKilled   += pd.MobsKilled;
            ps.TotalBossesKilled += pd.BossesKilled;
            ps.TotalDeaths       += pd.Deaths;
        }

        SavePlayerStats(guidLow);
    }
}

void DungeonMasterMgr::SaveLeaderboardEntry(const Session& session)
{
    uint32 clearTime = 0;
    if (session.EndTime > session.StartTime)
        clearTime = static_cast<uint32>(session.EndTime - session.StartTime);
    else
        clearTime = static_cast<uint32>(GameTime::GetGameTime().count() - session.StartTime);

    if (clearTime == 0) return;

    std::string leaderName = "Unknown";
    if (Player* leader = ObjectAccessor::FindPlayer(session.LeaderGuid))
        leaderName = leader->GetName();

    uint8 partySize = static_cast<uint8>(session.Players.size());

    // Aggregate kills/deaths across all participants
    uint32 totalMobs = 0, totalBosses = 0, totalDeaths = 0;
    for (const auto& pd : session.Players)
    {
        totalMobs   += pd.MobsKilled;
        totalBosses += pd.BossesKilled;
        totalDeaths += pd.Deaths;
    }

    std::string safeName = leaderName;
    size_t pos = 0;
    while ((pos = safeName.find('\'', pos)) != std::string::npos)
    {
        safeName.replace(pos, 1, "''");
        pos += 2;
    }

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO dm_leaderboard "
        "(guid, char_name, map_id, difficulty_id, clear_time, party_size, scaled, "
        "effective_level, mobs_killed, bosses_killed, deaths) "
        "VALUES (%u, '%s', %u, %u, %u, %u, %u, %u, %u, %u, %u)",
        session.LeaderGuid.GetCounter(), safeName.c_str(),
        session.MapId, session.DifficultyId, clearTime,
        partySize, session.ScaleToParty ? 1u : 0u,
        static_cast<uint32>(session.EffectiveLevel),
        totalMobs, totalBosses, totalDeaths);
    CharacterDatabase.Execute(query);
}

std::vector<LeaderboardEntry> DungeonMasterMgr::GetLeaderboard(
    uint32 mapId, uint32 difficultyId, uint32 limit) const
{
    std::vector<LeaderboardEntry> entries;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, guid, char_name, map_id, difficulty_id, clear_time, party_size, "
        "scaled, effective_level, mobs_killed, bosses_killed, deaths "
        "FROM dm_leaderboard "
        "WHERE map_id = %u AND difficulty_id = %u "
        "ORDER BY clear_time ASC LIMIT %u",
        mapId, difficultyId, limit);

    QueryResult result = CharacterDatabase.Query(query);

    if (!result) return entries;

    do
    {
        Field* f = result->Fetch();
        LeaderboardEntry e;
        e.Id             = f[0].Get<uint32>();
        e.Guid           = f[1].Get<uint32>();
        e.CharName       = f[2].Get<std::string>();
        e.MapId          = f[3].Get<uint32>();
        e.DifficultyId   = f[4].Get<uint32>();
        e.ClearTime      = f[5].Get<uint32>();
        e.PartySize      = f[6].Get<uint8>();
        e.Scaled         = f[7].Get<uint8>() != 0;
        e.EffectiveLevel = f[8].Get<uint8>();
        e.MobsKilled     = f[9].Get<uint32>();
        e.BossesKilled   = f[10].Get<uint32>();
        e.Deaths         = f[11].Get<uint32>();
        entries.push_back(e);
    } while (result->NextRow());

    return entries;
}

std::vector<LeaderboardEntry> DungeonMasterMgr::GetOverallLeaderboard(uint32 limit) const
{
    std::vector<LeaderboardEntry> entries;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, guid, char_name, map_id, difficulty_id, clear_time, party_size, "
        "scaled, effective_level, mobs_killed, bosses_killed, deaths "
        "FROM dm_leaderboard "
        "ORDER BY clear_time ASC LIMIT %u",
        limit);

    QueryResult result = CharacterDatabase.Query(query);

    if (!result) return entries;

    do
    {
        Field* f = result->Fetch();
        LeaderboardEntry e;
        e.Id             = f[0].Get<uint32>();
        e.Guid           = f[1].Get<uint32>();
        e.CharName       = f[2].Get<std::string>();
        e.MapId          = f[3].Get<uint32>();
        e.DifficultyId   = f[4].Get<uint32>();
        e.ClearTime      = f[5].Get<uint32>();
        e.PartySize      = f[6].Get<uint8>();
        e.Scaled         = f[7].Get<uint8>() != 0;
        e.EffectiveLevel = f[8].Get<uint8>();
        e.MobsKilled     = f[9].Get<uint32>();
        e.BossesKilled   = f[10].Get<uint32>();
        e.Deaths         = f[11].Get<uint32>();
        entries.push_back(e);
    } while (result->NextRow());

    return entries;
}

// Scaling multipliers
float DungeonMasterMgr::CalculateHealthMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->HealthMultiplier;
    uint32 n   = s->Players.size();
    float mult;
    if (n <= 1) mult = base * sDMConfig->GetSoloMultiplier();
    else        mult = base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerHealthMult());

    // Roguelike tier scaling
    if (s->RoguelikeRunId != 0)
        mult *= sRoguelikeMgr->GetTierHealthMultiplier(s->RoguelikeRunId);

    return mult;
}

float DungeonMasterMgr::CalculateDamageMultiplier(const Session* s) const
{
    if (!s) return 1.0f;
    const DifficultyTier* d = sDMConfig->GetDifficulty(s->DifficultyId);
    if (!d) return 1.0f;

    float base = d->DamageMultiplier;
    uint32 n   = s->Players.size();
    float mult;
    if (n <= 1) mult = base * sDMConfig->GetSoloMultiplier();
    else        mult = base * (1.0f + (n - 1) * sDMConfig->GetPerPlayerDamageMult());

    // Roguelike tier scaling
    if (s->RoguelikeRunId != 0)
        mult *= sRoguelikeMgr->GetTierDamageMultiplier(s->RoguelikeRunId);

    return mult;
}

// Check if creature belongs to an active session
bool DungeonMasterMgr::IsSessionCreature(ObjectGuid playerGuid, ObjectGuid creatureGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return false;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return false;

    return sit->second.IsSessionCreature(creatureGuid);
}

// Check if creature is a session boss
bool DungeonMasterMgr::IsSessionBoss(ObjectGuid playerGuid, ObjectGuid creatureGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return false;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return false;

    for (const auto& sc : sit->second.SpawnedCreatures)
        if (sc.Guid == creatureGuid)
            return sc.IsBoss;

    return false;
}

// Compute damage scale for a session creature attacking a session player.
// Spell/ability damage is hard-coded in DBC at the boss's original design level.
// This returns a multiplier to bring that damage in line with the session's level.
float DungeonMasterMgr::GetSessionCreatureDamageScale(
    ObjectGuid playerGuid, ObjectGuid creatureGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return 1.0f;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return 1.0f;

    const Session& session = sit->second;

    // Verify this creature belongs to the session
    bool isBoss = false;
    bool found  = false;
    for (const auto& sc : session.SpawnedCreatures)
    {
        if (sc.Guid == creatureGuid)
        {
            found  = true;
            isBoss = sc.IsBoss;
            break;
        }
    }
    if (!found)
        return 1.0f;

    // Trash mobs use our custom AI — melee is already scaled, no spells.
    if (!isBoss)
        return 1.0f;

    // For bosses: compare session target level to the boss's original template level.
    // Use creature_classlevelstats base damage ratio for accurate scaling.
    uint8 targetLevel = session.EffectiveLevel;

    // We need the creature's original template maxlevel.  Find a player reference
    // to resolve the creature GUID.
    Creature* creature = nullptr;
    for (const auto& pd : session.Players)
    {
        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
        if (p && p->IsInWorld())
        {
            creature = ObjectAccessor::GetCreature(*p, creatureGuid);
            if (creature) break;
        }
    }

    if (!creature)
        return 1.0f;

    uint8 templateLevel = creature->GetCreatureTemplate()->maxlevel;

    // If session level >= template level, boss is upscaled — no reduction needed.
    if (targetLevel >= templateLevel)
        return 1.0f;

    // Use classlevelstats to get the proper damage ratio between levels.
    uint8 unitClass = creature->GetCreatureTemplate()->unit_class;
    const ClassLevelStatEntry* targetStats   = GetBaseStatsForLevel(unitClass, targetLevel);
    const ClassLevelStatEntry* templateStats = GetBaseStatsForLevel(unitClass, templateLevel);

    float scale;
    if (targetStats && templateStats && templateStats->BaseDamage > 1.0f)
    {
        scale = targetStats->BaseDamage / templateStats->BaseDamage;
    }
    else
    {
        // Fallback: level ratio squared (spell damage scales ~quadratically)
        float lvlRatio = static_cast<float>(targetLevel) / static_cast<float>(templateLevel);
        scale = lvlRatio * lvlRatio;
    }

    // Apply solo/party multiplier so boss spells feel consistent with melee
    uint32 n = session.Players.size();
    if (n <= 1)
        scale *= sDMConfig->GetSoloMultiplier();

    // Clamp: never fully negate, never amplify
    scale = std::max(0.03f, std::min(1.0f, scale));

    LOG_DEBUG(DM_LOG_CATEGORY, "DungeonMaster: Boss spell damage scale for session {} — "
        "targetLvl={}, templateLvl={}, scale={:.3f}",
        session.SessionId, targetLevel, templateLevel, scale);

    return scale;
}

// Scale environmental damage to party level
float DungeonMasterMgr::GetEnvironmentalDamageScale(ObjectGuid playerGuid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto pit = _playerToSession.find(playerGuid);
    if (pit == _playerToSession.end())
        return 1.0f;

    auto sit = _activeSessions.find(pit->second);
    if (sit == _activeSessions.end())
        return 1.0f;

    const Session& session = sit->second;

    if (!session.ScaleToParty)
        return 1.0f;

    const DungeonInfo* dg = sDMConfig->GetDungeon(session.MapId);
    if (!dg)
        return 1.0f;

    uint8 dungeonLevel = dg->MaxLevel;
    uint8 partyLevel   = session.EffectiveLevel;

    if (partyLevel >= dungeonLevel)
        return 1.0f;

    float ratio = static_cast<float>(partyLevel) / static_cast<float>(dungeonLevel);
    float scale = std::pow(ratio, 1.5f);
    if (scale < 0.05f)
        scale = 0.05f;

    return scale;
}

// Main update tick (1s interval)
void DungeonMasterMgr::Update(uint32 diff)
{
    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL)
        return;
    _updateTimer = 0;

    struct PendingStrayCleanup
    {
        ObjectGuid ReferencePlayerGuid;
        uint32 MapId = 0;
        std::vector<ObjectGuid> CreatureGuids;
    };

    struct PendingRevive
    {
        uint32 MapId = 0;
        Position EntrancePos;
        std::vector<ObjectGuid> PlayerGuids;
    };

    std::vector<std::pair<uint32, bool>> toEnd;
    std::vector<std::pair<uint32, uint32>> roguelikeCompleted; // {runId, sessionId}
    std::vector<uint32> toPopulate;
    std::vector<PendingStrayCleanup> strayCleanup;
    std::vector<PendingRevive> pendingRevives;

    {
        std::lock_guard<std::mutex> lock(_sessionMutex);

        for (auto& [sid, session] : _activeSessions)
        {
            // ---- Poll creature deaths ----
            if (session.IsActive())
            {
                Player* ref = GetReferencePlayer(session);

                if (ref)
                {
                    // ---- Ensure instance mapping is registered ----
                    if (session.InstanceId == 0)
                    {
                        Map* m2 = ref->GetMap();
                        if (m2 && m2->IsDungeon())
                        {
                            session.InstanceId = m2->ToInstanceMap()->GetInstanceId();
                        }
                    }
                    if (session.InstanceId != 0 &&
                        _instanceToSession.find(session.InstanceId) == _instanceToSession.end())
                    {
                        _instanceToSession[session.InstanceId] = session.SessionId;
                    }

                    // ---- Populate if not yet done ----
                    if (session.TotalMobs == 0 && session.TotalBosses == 0 && !session.PopulationPending)
                    {
                        Map* m = ref->GetMap();
                        if (m && m->IsDungeon())
                        {
                            InstanceMap* inst = m->ToInstanceMap();
                            if (inst)
                            {
                                session.InstanceId = inst->GetInstanceId();
                                _instanceToSession[session.InstanceId] = session.SessionId;
                                session.PopulationPending = true;
                                toPopulate.push_back(session.SessionId);
                            }
                        }
                    }

                    // Build set of our known GUIDs for stray detection
                    std::set<ObjectGuid> ourGuids;
                    for (const auto& sc : session.SpawnedCreatures)
                        ourGuids.insert(sc.Guid);

                    for (auto& sc : session.SpawnedCreatures)
                    {
                        if (sc.IsDead && sc.LootFilled && sc.KillCredited)
                            continue;   // fully processed
                        Creature* c = ObjectAccessor::GetCreature(*ref, sc.Guid);
                        if (!c || !c->IsAlive())
                        {
                            sc.IsDead = true;

                            if (!sc.LootFilled && c)
                            {
                                sc.LootFilled = true;
                                FillCreatureLoot(c, &session, sc.IsBoss);
                            }

                            if (!sc.KillCredited)
                            {
                                sc.KillCredited = true;
                                GiveKillXP(&session, sc.IsBoss, sc.IsElite);

                                if (sc.IsBoss)
                                {
                                    session.PendingPhaseChecks.push_back(
                                        CreatePendingPhaseCheck(session, c, ref, sc.Entry));
                                }
                                else
                                {
                                    ++session.MobsKilled;
                                    for (auto& pd : session.Players)
                                        ++pd.MobsKilled;
                                }
                            }
                        }
                    }

                    // ---- Multi-phase boss resolution ----
                    // After 5 seconds, check if new creatures spawned near the boss death location.
                    // If found, promote them to boss status. If not, confirm the boss kill.
                    uint64 nowTime = GameTime::GetGameTime().count();
                    for (auto& ppc : session.PendingPhaseChecks)
                    {
                        if (ppc.Resolved) continue;
                        if (nowTime - ppc.DeathTime < 5) continue;  // Wait 5 seconds for phase transitions

                        ppc.Resolved = true;

                        // Scan for new non-tracked creatures near the boss death position
                        bool phaseCreatureFound = false;
                        Map* scanMap = ref->GetMap();
                        if (scanMap && scanMap->IsDungeon() && ppc.DeathPos.GetPositionX() != 0.0f)
                        {
                            std::list<Creature*> nearby;
                            ref->GetCreatureListWithEntryInGrid(nearby, 0, 5000.0f);

                            for (Creature* nc : nearby)
                            {
                                if (!nc || !nc->IsAlive() || nc->IsPet() || nc->IsGuardian())
                                    continue;
                                if (nc->GetEntry() == sDMConfig->GetNpcEntry())
                                    continue;
                                if (ourGuids.count(nc->GetGUID()) > 0)
                                    continue;  // Already tracked

                                // Check distance from boss death position (within 40 yards)
                                float dx = nc->GetPositionX() - ppc.DeathPos.GetPositionX();
                                float dy = nc->GetPositionY() - ppc.DeathPos.GetPositionY();
                                float dz = nc->GetPositionZ() - ppc.DeathPos.GetPositionZ();
                                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

                                if (dist > PHASE_DETECTION_RADIUS) continue;

                                if (std::find(ppc.NearbyEliteGuids.begin(), ppc.NearbyEliteGuids.end(), nc->GetGUID())
                                    != ppc.NearbyEliteGuids.end())
                                    continue;

                                // Check if it's an elite/boss creature (likely phase 2)
                                const CreatureTemplate* tmpl = nc->GetCreatureTemplate();
                                if (!tmpl || (tmpl->rank != 1 && tmpl->rank != 2 && tmpl->rank != 4))
                                    continue;

                                // Promote to boss creature
                                LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Phase creature detected! '{}' (entry {}) "
                                    "spawned {:.1f} yds from boss death location — promoting to boss",
                                    nc->GetName(), nc->GetEntry(), dist);

                                nc->SetFaction(14);
                                nc->SetReactState(REACT_AGGRESSIVE);
                                nc->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC
                                                                | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_PACIFIED);
                                nc->SetImmuneToPC(false);
                                nc->SetImmuneToNPC(false);

                                SpawnedCreature nsc;
                                nsc.Guid = nc->GetGUID();
                                nsc.Entry = nc->GetEntry();
                                nsc.IsElite = true;
                                nsc.IsBoss = true;
                                session.SpawnedCreatures.push_back(nsc);
                                ourGuids.insert(nc->GetGUID());

                                // Track the GUID for cleanup
                                auto& gl = _instanceCreatureGuids[session.InstanceId];
                                gl.push_back(nc->GetGUID());

                                phaseCreatureFound = true;

                                for (const auto& pd3 : session.Players)
                                    if (Player* p3 = ObjectAccessor::FindPlayer(pd3.PlayerGuid))
                                        if (p3->GetSession())
                                            ChatHandler(p3->GetSession()).SendSysMessage(
                                                "|cFFFF8000[Dungeon Master]|r The boss enters a new phase!");
                                break;  // Only promote one phase creature per check
                            }
                        }

                        if (!phaseCreatureFound)
                        {
                            // No phase creature found — confirm the boss kill
                            ++session.BossesKilled;
                            for (auto& pd : session.Players)
                                ++pd.BossesKilled;

                            LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Boss kill confirmed (entry {}) — progress: {}/{}",
                                ppc.OrigEntry, session.BossesKilled, session.TotalBosses);
                            HandleBossDeath(&session);

                            // Check completion
                            if (session.IsActive() && session.TotalBosses > 0
                                && session.BossesKilled >= session.TotalBosses)
                            {
                                session.State   = SessionState::Completed;
                                session.EndTime = GameTime::GetGameTime().count();

                                uint32 delay = (session.RoguelikeRunId != 0)
                                    ? sDMConfig->GetRoguelikeTransitionDelay()
                                    : sDMConfig->GetCompletionTeleportDelay();

                                for (const auto& pd2 : session.Players)
                                    if (Player* p = ObjectAccessor::FindPlayer(pd2.PlayerGuid))
                                        if (p->GetSession())
                                        {
                                            char buf[256];
                                            snprintf(buf, sizeof(buf),
                                                "|cFF00FF00[Dungeon Master]|r %s "
                                                "Rewards in |cFFFFFFFF%u|r seconds...",
                                                session.RoguelikeRunId != 0
                                                    ? "Floor cleared!" : "Dungeon complete!",
                                                delay);
                                            ChatHandler(p->GetSession()).SendSysMessage(buf);
                                        }
                                break;
                            }
                        }
                    }

                    // Clean up resolved phase checks
                    session.PendingPhaseChecks.erase(
                        std::remove_if(session.PendingPhaseChecks.begin(), session.PendingPhaseChecks.end(),
                            [](const PendingPhaseCheck& p) { return p.Resolved; }),
                        session.PendingPhaseChecks.end());

                    // ---- Sweep for stray creatures (script-spawned, respawned) ----
                    Map* m = ref->GetMap();
                    if (m && m->IsDungeon())
                    {
                        uint32 npcEntry = sDMConfig->GetNpcEntry();
                        auto const& dbStore = static_cast<InstanceMap*>(m)->GetCreatureBySpawnIdStore();
                        std::vector<ObjectGuid> strayGuids;
                        strayGuids.reserve(dbStore.size());

                        for (auto const& pair : dbStore)
                        {
                            Creature* stray = pair.second;
                            if (stray && stray->IsInWorld() && stray->IsAlive()
                                && stray->GetEntry() != npcEntry
                                && !stray->IsPet() && !stray->IsGuardian() && !stray->IsTotem()
                                && ourGuids.count(stray->GetGUID()) == 0)
                            {
                                strayGuids.push_back(stray->GetGUID());
                            }
                        }

                        if (!strayGuids.empty())
                        {
                            PendingStrayCleanup cleanup;
                            cleanup.ReferencePlayerGuid = ref->GetGUID();
                            cleanup.MapId = session.MapId;
                            cleanup.CreatureGuids = std::move(strayGuids);
                            strayCleanup.push_back(std::move(cleanup));
                        }
                    }
                }

                // ---- Auto-rez when out of combat ----
                if (session.IsActive() && !session.IsGroupInCombat())
                {
                    PendingRevive revive;
                    revive.MapId = session.MapId;
                    revive.EntrancePos = session.EntrancePos;

                    for (const auto& pd : session.Players)
                    {
                        Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                        if (p && !p->IsAlive() && p->GetMapId() == session.MapId)
                            revive.PlayerGuids.push_back(pd.PlayerGuid);
                    }

                    if (!revive.PlayerGuids.empty())
                        pendingRevives.push_back(std::move(revive));
                }
            }

            // ---- Time limit ----
            if (session.TimeLimit > 0 && session.State == SessionState::InProgress)
            {
                uint64 elapsed = GameTime::GetGameTime().count() - session.StartTime;
                if (elapsed >= session.TimeLimit)
                {
                    session.State = SessionState::Failed;
                    toEnd.emplace_back(sid, false);
                    for (const auto& pd : session.Players)
                        if (Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                            ChatHandler(p->GetSession()).SendSysMessage(
                                "|cFFFF0000[Dungeon Master]|r Time's up! Challenge failed.");
                    continue;
                }
            }

            // ---- Completed → teleport delay ----
            if (session.State == SessionState::Completed)
            {
                uint32 delay = (session.RoguelikeRunId != 0)
                    ? sDMConfig->GetRoguelikeTransitionDelay()
                    : sDMConfig->GetCompletionTeleportDelay();
                uint64 elapsed = GameTime::GetGameTime().count() - session.EndTime;

                // ---- Roguelike countdown announcements ----
                if (session.RoguelikeRunId != 0 && elapsed < delay)
                {
                    uint32 remaining = static_cast<uint32>(delay - elapsed);
                    static const uint32 announceAt[] = {25, 20, 15, 10, 5, 4, 3, 2, 1};
                    for (uint32 sec : announceAt)
                    {
                        if (remaining == sec)
                        {
                            char cbuf[128];
                            snprintf(cbuf, sizeof(cbuf),
                                "|cFF00FFFF[Roguelike]|r Next dungeon in |cFFFFFFFF%u|r second%s...",
                                remaining, remaining != 1 ? "s" : "");
                            for (const auto& pd3 : session.Players)
                                if (Player* p3 = ObjectAccessor::FindPlayer(pd3.PlayerGuid))
                                    if (p3->GetSession())
                                        ChatHandler(p3->GetSession()).SendSysMessage(cbuf);
                            break;
                        }
                    }
                }

                if (elapsed >= delay)
                {
                    if (session.RoguelikeRunId != 0)
                        roguelikeCompleted.push_back({session.RoguelikeRunId, sid});
                    else
                        toEnd.emplace_back(sid, true);
                    continue;
                }
            }

            // ---- Failed cleanup ----
            if (session.State == SessionState::Failed)
            {
                // Roguelike sessions: wipe is handled by RoguelikeMgr::OnPartyWipe
                if (session.RoguelikeRunId != 0)
                    continue;

                if (session.EndTime == 0)
                    session.EndTime = GameTime::GetGameTime().count();
                else if (GameTime::GetGameTime().count() - session.EndTime >= 2)
                {
                    toEnd.emplace_back(sid, false);
                    continue;
                }
            }

            // ---- Abandoned detection ----
            if (session.IsActive()
                && (GameTime::GetGameTime().count() - session.StartTime) >= 15)
            {
                bool anyone = false;
                for (const auto& pd : session.Players)
                {
                    Player* p = ObjectAccessor::FindPlayer(pd.PlayerGuid);
                    if (p && p->GetMapId() == session.MapId) { anyone = true; break; }
                }
                if (!anyone)
                {
                    LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} abandoned — no players on map {} after grace period",
                        sid, session.MapId);
                    session.State = SessionState::Abandoned;
                    toEnd.emplace_back(sid, false);
                }
            }
        }
    } // release lock

    for (PendingStrayCleanup const& cleanup : strayCleanup)
    {
        Player* ref = ObjectAccessor::FindPlayer(cleanup.ReferencePlayerGuid);
        if (!ref || !ref->IsInWorld() || ref->GetMapId() != cleanup.MapId)
            continue;

        for (ObjectGuid const& guid : cleanup.CreatureGuids)
        {
            Creature* stray = ObjectAccessor::GetCreature(*ref, guid);
            if (!stray || !stray->IsInWorld() || !stray->IsAlive())
                continue;
            if (stray->GetEntry() == sDMConfig->GetNpcEntry())
                continue;
            if (stray->IsPet() || stray->IsGuardian() || stray->IsTotem())
                continue;

            stray->SetRespawnTime(7 * DAY);
            stray->DespawnOrUnsummon();
        }
    }

    for (PendingRevive const& revive : pendingRevives)
    {
        for (ObjectGuid const& guid : revive.PlayerGuids)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || player->IsAlive() || player->GetMapId() != revive.MapId)
                continue;

            player->RemoveFlag(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTE_NO_RELEASE_WINDOW);
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
            player->TeleportTo(revive.MapId,
                revive.EntrancePos.GetPositionX(),
                revive.EntrancePos.GetPositionY(),
                revive.EntrancePos.GetPositionZ(),
                revive.EntrancePos.GetOrientation());

            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cFF00FF00[Dungeon Master]|r Revived at entrance. Get back in there!");
        }
    }

    for (uint32 sessionId : toPopulate)
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);

        Session sessionSnapshot;
        if (!GetSessionSnapshot(sessionId, sessionSnapshot))
            continue;

        Player* ref = nullptr;
        for (const auto& pd : sessionSnapshot.Players)
        {
            Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid);
            if (player && player->GetMapId() == sessionSnapshot.MapId)
            {
                ref = player;
                break;
            }
        }

        if (ref)
        {
            Map* map = ref->GetMap();
            InstanceMap* inst = (map && map->IsDungeon()) ? map->ToInstanceMap() : nullptr;
            if (inst && sessionSnapshot.IsActive() && sessionSnapshot.TotalMobs == 0 && sessionSnapshot.TotalBosses == 0)
            {
                Session* session = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    auto it = _activeSessions.find(sessionId);
                    if (it == _activeSessions.end() || !it->second.IsActive())
                    {
                        LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} disappeared before deferred population could begin.",
                            sessionId);
                        continue;
                    }

                    session = &it->second;
                }

                RegisterSessionInstance(session->SessionId, inst->GetInstanceId());

                for (const auto& pd : session->Players)
                    if (Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                        if (player->GetSession())
                            ChatHandler(player->GetSession()).SendSysMessage(
                                "|cFF00FF00[Dungeon Master]|r Preparing the challenge...");

                PopulateDungeon(session, inst);

                LOG_INFO(DM_LOG_CATEGORY, "DungeonMaster: Session {} — populated (map {}, mobs={}, bosses={})",
                    session->SessionId, session->MapId,
                    session->TotalMobs, session->TotalBosses);

                if (!session->IsActive())
                    continue;

                char buf[256];
                snprintf(buf, sizeof(buf),
                    "|cFF00FF00[Dungeon Master]|r |cFFFFFFFF%u|r enemies and "
                    "|cFFFFFFFF%u|r boss(es) spawned. Enemy level: |cFFFFFFFF%u|r "
                    "(tier band |cFFFFFFFF%u-%u|r). Good luck!",
                    session->TotalMobs, session->TotalBosses,
                    session->EffectiveLevel, session->LevelBandMin, session->LevelBandMax);
                for (const auto& pd : session->Players)
                    if (Player* player = ObjectAccessor::FindPlayer(pd.PlayerGuid))
                        if (player->GetSession())
                            ChatHandler(player->GetSession()).SendSysMessage(buf);
            }
        }

        {
            std::lock_guard<std::mutex> lock(_sessionMutex);
            auto it = _activeSessions.find(sessionId);
            if (it != _activeSessions.end())
                it->second.PopulationPending = false;
        }
    }

    for (const auto& [id, ok] : toEnd)
        EndSession(id, ok);

    // Process roguelike completions outside session lock
    for (const auto& [runId, sessId] : roguelikeCompleted)
        sRoguelikeMgr->OnDungeonCompleted(runId, sessId);


    {
        std::lock_guard<std::mutex> lock(_cooldownMutex);
        time_t now = GameTime::GetGameTime().count();
        for (auto it = _cooldowns.begin(); it != _cooldowns.end(); )
            (now >= static_cast<time_t>(it->second)) ? it = _cooldowns.erase(it) : ++it;
    }
}

std::string DungeonMasterMgr::GetSessionStatusString(const Session* s) const
{
    if (!s) return "No session";
    static const char* names[] = { "None","Preparing","InProgress","BossPhase","Completed","Failed","Abandoned" };
    char buf[256];
    snprintf(buf, sizeof(buf), "Session %u — %s, Mobs %u/%u, Bosses %u/%u, Lvl %u, Band %u-%u",
        s->SessionId, names[static_cast<int>(s->State)],
        s->MobsKilled, s->TotalMobs, s->BossesKilled, s->TotalBosses,
        s->EffectiveLevel, s->LevelBandMin, s->LevelBandMax);
    return buf;
}

} // namespace DungeonMaster

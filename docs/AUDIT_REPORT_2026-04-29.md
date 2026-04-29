# mod-dungeon-master audit report

Date: 2026-04-29

Status: audit complete, no implementation changes applied yet

## Scope reviewed

This audit was performed for a full review and planned overhaul of `mod-dungeon-master` with special attention to:

- spawns
- elites
- bosses
- map usage
- weighting / selection logic
- difficulty and affixes
- rewards
- UI robustness
- compatibility requirements

User-confirmed priorities:

- review all supported 5-man content
- make difficulty more punishing and less predictable
- tone themes down to better respect dungeon identity
- use a hybrid boss model depending on dungeon
- support only owner-led Playerbot groups
- hard block Challenge Modes usage
- redesign the gossip UI and explore an AIO client UI
- improve logging
- stop after audit and wait for approval before implementation

## Evidence reviewed

### Source files

- `src/DMConfig.cpp`
- `src/DMConfig.h`
- `src/DMTypes.h`
- `src/DungeonMasterMgr.cpp`
- `src/DungeonMasterMgr.h`
- `src/RoguelikeMgr.cpp`
- `src/RoguelikeTypes.h`
- `src/scripts/npc_dungeon_master.cpp`
- `src/scripts/dm_allmap_script.cpp`
- `src/scripts/dm_player_script.cpp`
- `src/scripts/dm_unit_script.cpp`
- `src/scripts/dm_world_script.cpp`
- `src/scripts/dm_command_script.cpp`
- `src/DungeonMaster_loader.cpp`
- `conf/mod_dungeon_master.conf.dist`
- `data/sql/db-world/base/dm_setup.sql`
- `data/sql/db-characters/base/dm_characters_setup.sql`
- `README.md`

### Database validation

Validated against live AzerothCore databases:

- entrance counts from `areatrigger_teleport`
- spawn density from `creature`
- boss pool candidates from `creature_template`, `creature`, `creature_template_movement`
- characters tables for DM persistence

## Executive summary

The module is functional in broad strokes, but it currently behaves more like a procedural sandbox than a reliable dungeon system. The biggest issues are not cosmetic — they are architectural and explain the bugs already reported by the user.

### Highest-priority findings

1. **Boss generation can legitimately fail**, especially for low-tier themed runs, which can leave sessions with `TotalBosses == 0` and therefore no completion path.
2. **Spawn budgeting does not exist.** The module uses almost every non-boss creature spawn point in a map, so huge dungeons massively overpopulate while smaller dungeons stay comparatively sane.
3. **Elite logic is partially invisible and partially inconsistent.** Elite stat boosts exist, but elite presentation and config usage are incomplete, which makes elites easy to perceive as “not spawning.”
4. **Most weighting is not weighting.** Dungeon, creature, boss, and affix selection are mostly uniform random choices with very little tuning or contextual logic.
5. **Several reward config settings are not actually honored.** The module exposes knobs that the gameplay path does not use.
6. **Compatibility gates are missing.** There is no Challenge Modes protection, no Playerbot ownership validation, and no AIO integration at all.
7. **The UI is functional but fragile.** It is chat-heavy, gossip-only, and has no rich preview, validation, or error surfaces.

## Module inventory

### Main runtime components

- `DMConfig`: hard-coded dungeon catalog + config loading
- `DungeonMasterMgr`: sessions, spawning, rewards, cleanup, leaderboard writes
- `RoguelikeMgr`: multi-floor progression, affixes, tier scaling, roguelike leaderboard writes
- `npc_dungeon_master`: player-facing gossip flow
- map / unit / player / world / command scripts: lifecycle hooks and GM controls

### SQL content

- `dm_setup.sql`: world NPC + vendor setup
- `dm_characters_setup.sql`: normal and roguelike stats / leaderboard tables

## Map audit

### Supported dungeon pool

The code currently hard-codes **45** dungeons in `DMConfig::LoadDungeons()`, not 37.

#### Classic

- Ragefire Chasm (`389`)
- Deadmines (`36`)
- Shadowfang Keep (`33`)
- The Stockade (`34`)
- Wailing Caverns (`43`)
- Blackfathom Deeps (`48`)
- Razorfen Kraul (`47`)
- Gnomeregan (`90`)
- Razorfen Downs (`129`)
- Scarlet Monastery (`189`)
- Uldaman (`70`)
- Zul'Farrak (`209`)
- Maraudon (`349`)
- Sunken Temple (`109`)
- Blackrock Depths (`230`)
- Blackrock Spire (`229`)
- Scholomance (`289`)
- Stratholme (`329`)

#### TBC

- Hellfire Ramparts (`543`)
- Blood Furnace (`542`)
- Slave Pens (`547`)
- Underbog (`546`)
- Mana-Tombs (`557`)
- Auchenai Crypts (`558`)
- Sethekk Halls (`556`)
- Shadow Labyrinth (`555`)
- Shattered Halls (`540`)
- Botanica (`553`)
- Mechanar (`554`)
- Arcatraz (`552`)

#### WotLK

- Utgarde Keep (`574`)
- The Nexus (`576`)
- Azjol-Nerub (`601`)
- Ahn'kahet (`619`)
- Drak'Tharon Keep (`600`)
- Violet Hold (`608`)
- Gundrak (`604`)
- Halls of Stone (`599`)
- Halls of Lightning (`602`)
- The Oculus (`578`)
- Utgarde Pinnacle (`575`)
- Culling of Stratholme (`595`)
- Forge of Souls (`632`)
- Pit of Saron (`658`)
- Halls of Reflection (`668`)

### Documentation mismatch

`README.md` advertises **37 dungeons**. The code ships **45**. This needs correction before any gameplay redesign so operators and players are not misled.

### Multiple-entrance maps

DB validation shows these configured maps have multiple entrances and therefore need entrance clustering logic:

- `70` Uldaman — 2 entrances
- `90` Gnomeregan — 2 entrances
- `189` Scarlet Monastery — 4 entrances
- `229` Blackrock Spire — 2 entrances
- `329` Stratholme — 3 entrances
- `349` Maraudon — 2 entrances
- `604` Gundrak — 2 entrances

This is one of the stronger parts of the current module: `GetSpawnPointsForMap()` already attempts entrance-cluster filtering, which is especially important for Scarlet Monastery.

### Spawn density by map

The current module builds trash spawn points from **all creature spawns in the map** and only excludes boss-designated points later. There is no budget cap, no route selection, and no per-map normalization.

Selected DB spawn counts:

- Blackrock Depths (`230`) — 1098 spawns
- Maraudon (`349`) — 630
- Blackrock Spire (`229`) — 607
- Stratholme (`329`) — 476
- Gnomeregan (`90`) — 404
- Scholomance (`289`) — 400
- Scarlet Monastery (`189`) — 348
- Uldaman (`70`) — 317
- Wailing Caverns (`43`) — 315
- Sunken Temple (`109`) — 312
- Violet Hold (`608`) — 39
- Halls of Reflection (`668`) — 57

#### Finding

Map complexity and spawn volume vary wildly, but the module currently treats spawn points as near-equivalent. That makes some dungeons overwhelmingly long or dense, while others are sparse or trivial.

#### Impact

- large dungeons become bloated slogs
- completion pacing is inconsistent between maps
- rewards and difficulty feel detached from dungeon size
- roguelike floor-to-floor fairness is poor

## Spawn audit

### Current spawn pipeline

`PopulateDungeon()` does the following:

1. clears the instance aggressively
2. opens all doors by deleting door / button gameobjects
3. marks instance encounters `DONE`
4. gathers all spawn points from the DB
5. summons trash at all non-boss positions
6. optionally adds one rare spawn
7. summons boss(es) at boss-designated positions
8. resets encounters to `NOT_STARTED`
9. optionally spawns the roguelike vendor

### Strengths

- existing deferred population in `Update()` is safer than populating during `OnPlayerEnterAll`
- instance registration for compatibility with modules like autobalance is already present
- entrance clustering exists for shared-map dungeons

### Weaknesses

#### 1. Trash spawn count ignores difficulty mob scaling

`DifficultyTier` contains `MobCountMultiplier`, but `PopulateDungeon()` never uses it.

This means:

- difficulty tiers do not meaningfully affect number of enemies
- map size dominates encounter count more than difficulty choice
- config suggests a tuning dimension that the runtime ignores

#### 2. Spawn selection ignores dungeon identity

Trash comes from `SelectCreatureForTheme()` using global creature pools filtered by type and level, not by dungeon, expansion, biome, or map tone.

That produces highly synthetic runs such as:

- mechanical trash in places with no mechanical fantasy
- undead or demons in dungeons where they feel nonsensical
- generic “type match” replacing dungeon identity entirely

This conflicts with the user’s desired direction to tone themes down and preserve dungeon identity.

#### 3. No route shaping or encounter composition

The module does not distinguish between:

- patrol lanes
- room anchors
- boss antechambers
- side branches
- event spaces

It simply uses DB creature positions as procedural sockets.

## Elite audit

### What the code does

For each trash summon, elite chance is rolled using:

- base chance: `DungeonMaster.Dungeon.EliteChance`
- Savage affix: multiplies the chance

Elite mobs then get:

- extra health
- extra damage
- elite XP / loot behavior

### Critical findings

#### 1. Elite damage multiplier config is ignored

`mod_dungeon_master.conf.dist` exposes:

- `DungeonMaster.Scaling.EliteDamageMult`

But `PopulateDungeon()` uses a hard-coded `1.5f` for elite damage instead of `sDMConfig->GetEliteDamageMult()`.

#### 2. Elites are not visually promoted

Trash elites do not get an elite rank frame or stronger visual treatment. Only bosses and rares get explicit unit-byte rank changes.

This is a likely root cause of the user report that **“elites do not spawn in Roguelike.”**

What is probably happening in practice:

- elite stat buffs are applied
- elite XP / loot classification is applied
- but players do not get a clear elite presentation, so elites look like normal trash

#### 3. Elite behavior does not get unique mechanics

Elites are still just stronger trash using `DungeonMasterCreatureAI`. There is no elite-specific behavior, pack logic, aura package, or affix interaction beyond flat stat multipliers.

### Audit conclusion for elites

The elite system exists, but it is not convincing enough to feel deliberate. It needs both correctness fixes and stronger identity.

## Rare spawn audit

### Current implementation

Rares are spawned once per run based on `RareSpawnChance`.

### Finding

Rare selection calls `SelectCreatureForTheme(theme, true, ...)`, which uses boss-oriented candidate logic rather than a dedicated rare pool.

That means rares are currently closer to “random promoted boss/elite-capable entries” than true curated rares.

### Impact

- inconsistent rare quality
- weak separation between rare and boss identity
- difficult to reason about rarity balance

## Boss audit

### Current model

Boss placement and boss identity are separate systems:

- **boss positions** are taken from the target map
- **boss entries** are selected from a **global dungeon boss pool** filtered only by theme and level band

This means a boss from one dungeon can be spawned inside another dungeon if theme and level match.

### Good news

DB validation shows every configured map currently has at least one rank/immunity-derived boss position candidate, so the boss-position lookup itself is not the main failure point.

### Critical findings

#### 1. Low-tier themed boss coverage is badly incomplete

Global scripted boss pool coverage by creature type is heavily skewed toward high-level content.

DB validation summary:

- type 1 Beast bosses: min level 59
- type 2 Dragonkin bosses: min level 60
- type 3 Demon bosses: min level 62
- type 4 Elemental bosses: min level 55
- type 5 Giant bosses: min level 57
- type 6 Undead bosses: min level 56
- type 7 Humanoid bosses: min level 20
- type 9 Mechanical bosses: min level 72
- type 10 NotSpecified bosses: min level 69

Tier overlap counts show the problem clearly:

- **Novice (10-19): zero boss coverage for every type**
- **Apprentice (20-29): only humanoid has any boss coverage**
- **Journeyman (30-44): only humanoid has any boss coverage**
- most themed boss types only become available in Expert / Master / Grandmaster bands

#### 2. Missing boss -> no dungeon completion

If `SelectDungeonBoss()` returns `0`, `PopulateDungeon()` logs a warning and skips boss spawn.

Later, completion logic requires:

- `session.TotalBosses > 0`
- `session.BossesKilled >= session.TotalBosses`

If no boss is spawned, `TotalBosses` stays `0` and the session never enters the success path.

This is a direct explanation for:

- **“bosses do not always spawn”**
- **“dungeons do not end due to no end boss”**

#### 3. Bosses are not map-native

The current system is fully global. There is no:

- per-map boss pool
- per-map fallback chain
- dungeon-identity-first selection
- boss blacklist for poor fits

This conflicts with the user requirement for a **hybrid boss model depending on dungeon**.

#### 4. Boss weighting is uniform random

Within the candidate pool, the final boss is selected uniformly. There is no weighting by:

- map appropriateness
- lore fit
- mechanic complexity
- movement / phase safety
- failure history

### Boss audit conclusion

The boss pipeline is currently the most important overhaul target.

## Weighting audit

### What is actually weighted today

Very little.

### Standard mode

- random dungeon selection is uniform random across the difficulty-filtered dungeon list
- creature selection is uniform random across candidate entries
- boss selection is uniform random across candidate entries

### Roguelike mode

- map selection is uniform random across the base-difficulty-filtered dungeon list
- immediate repeat avoidance exists via `PreviousMapId`
- affixes are selected by shuffling the pool and taking the first N entries

### What is missing

There is no weighting for:

- map size
- dungeon completion time
- route complexity
- player count
- player roles / party composition
- prior floor history beyond immediate repeat avoidance
- theme-map synergy
- affix-map synergy
- affix-affix synergy
- boss-map fit
- reward normalization by runtime effort

### Result

The module currently feels random, not curated.

## Difficulty audit

### Current formulas

Difficulty combines:

- tier health / damage multipliers
- party-size scaling
- solo multiplier
- elite / boss multipliers
- roguelike tier multipliers
- affix multipliers

### Critical findings

#### 1. Difficulty is more stat inflation than encounter design

Most difficulty comes from:

- HP multiplication
- damage multiplication
- occasional elite chance multiplication

There is very little mechanical escalation beyond that.

#### 2. Difficulty is inconsistent across maps because spawn count dominates

Since large maps use far more spawn points and there is no budget normalization, map size can outweigh formal difficulty settings.

#### 3. `MobCountMultiplier` is unused

This is one of the clearest correctness problems in the system. The config exposes mob-count scaling but runtime never applies it.

## Affix audit

### Current affixes

- Fortified
- Tyrannical
- Raging
- Bolstering
- Savage

### What they really do today

These are not mechanic-rich affixes; they are mostly aggregate multipliers:

- Fortified: trash HP / damage stats
- Tyrannical: boss HP / damage stats
- Raging: all damage stats
- Bolstering: all HP stats
- Savage: doubles elite chance and slightly buffs trash damage

### Findings

#### 1. Affixes are global and map-agnostic

There are no per-map, per-boss, or per-theme restrictions.

#### 2. Affix selection is fully uniform

Affixes are shuffled and sliced. No weights, no exclusions, no curated pairings.

#### 3. "Bolstering" is not bolstering in the Mythic+ sense

Current implementation is a static HP multiplier, not an in-combat reactive buff mechanic.

#### 4. "Raging" is not raging in the Mythic+ sense

Current implementation is a flat damage multiplier, not an execute-threshold state change.

### Affix audit conclusion

Affixes exist, but they are closer to stat mutators than gameplay modifiers.

## Reward audit

### Critical findings

Several exposed reward settings are not actually honored by runtime reward code.

#### Configs currently bypassed or effectively unused

- `DungeonMaster.Rewards.BaseGold`
- `DungeonMaster.Rewards.GoldPerMob`
- `DungeonMaster.Rewards.GoldPerBoss`
- `DungeonMaster.Rewards.XPMultiplier`
- `DungeonMaster.Rewards.ItemChance`

### Current behavior instead

Normal completion rewards are derived from:

- effective level
- mobs killed
- bosses killed
- difficulty reward multiplier

Kill XP is hard-coded from player level and boss/elite status.

Completion items are always attempted; `ItemChance` is not used as a gate.

### Roguelike reward issue

Roguelike floors currently receive:

1. per-floor standard DM rewards during `OnDungeonCompleted()`
2. end-of-run roguelike rewards in `EndRun()`

This may be intended, but it creates a very generous double-reward economy and should be explicitly redesigned rather than inherited accidentally.

## UI audit

### Current state

The UI is entirely gossip-driven with chat output used for:

- summaries
- instructions
- stats
- leaderboards
- run announcements

### Strengths

- works with stock client
- simple to reason about
- relatively easy to extend

### Weaknesses

#### 1. No AIO UI exists yet

There is currently no AIO / Lua integration in the module.

#### 2. Validation is minimal

The gossip flow does not validate:

- Challenge Modes conflicts
- Playerbot ownership rules
- raid / oversize party restrictions
- unsupported compositions
- risky boss/theme/dungeon combinations

#### 3. UX is chat-heavy and state-light

Important info is pushed to chat rather than represented in the interaction surface.

Missing UI features include:

- stronger back-navigation model
- one-screen run summary
- per-choice difficulty preview
- affix preview
- map identity summary
- risk indicators
- compatibility warnings
- richer leaderboard navigation

#### 4. Leaderboards are broad, not surgical

Normal leaderboard UI shows global fastest clears, not per-map or per-difficulty boards by default.

### UI conclusion

The current gossip UI is a good fallback, but not a good primary experience for a major progression system.

## Compatibility audit

### Challenge Modes

There is currently **no Challenge Modes protection** in the module.

No code path in the audited source checks Challenge Mode state before:

- showing the menu
- creating a session
- starting a roguelike run
- teleporting players

This directly conflicts with the user requirement to hard block Challenge Modes.

### Playerbots

There is currently **no owner-led Playerbot validation**.

The session builder simply includes all in-world group members. There is no inspection for:

- bots vs real players
- who owns each bot
- whether the leader owns every participating bot

This directly conflicts with the user requirement to allow only owner-led bot groups.

### Other modules

There is some evidence of prior compatibility work:

- session instance registration exists early enough for systems like autobalance
- XP hook integration uses `OnPlayerGiveXP` before `GiveXP`

However, there is no explicit compatibility layer for:

- Challenge Modes
- Playerbots policy
- Mythic Enhanced
- AIO UI

## Logging and observability audit

### Strengths

- dedicated logger category exists: `module.DungeonMaster`
- info logging is already fairly rich during spawn and session lifecycle
- debug logging can be enabled via config

### Weaknesses

#### Missing or weak observability areas

- no structured summary of why boss selection failed for a specific theme / band
- no counters for per-tier / per-theme failure rates
- no map-budget logs because there is no map budget system
- no compatibility rejection logs because there are no compatibility gates
- no UI analytics / flow tracing

### Logging conclusion

Logging exists, but it is still closer to manual debugging than operational telemetry.

## SQL and data audit

### Characters DB

The required characters tables exist:

- `dm_player_stats`
- `dm_leaderboard`
- `dm_roguelike_player_stats`
- `dm_roguelike_leaderboard`

### World DB

The Dungeon Master NPC currently has **11** city spawns in the world DB, which matches the setup SQL.

### Vendor

The roguelike vendor is configured in SQL and spawned programmatically. This is serviceable, though vendor UX is still primitive compared to an AIO panel.

## Root-cause analysis for reported bugs

### 1. “Elites do not spawn in Roguelike”

Most likely root causes:

- elites are not visually promoted, so they do not read as elites in play
- elite damage tuning is hard-coded rather than fully config-driven
- elite identity is only statistical, not mechanical or visual

### 2. “Bosses do not always spawn”

Root cause:

- boss pool level/type coverage is sparse, especially at low tiers
- `SelectDungeonBoss()` can legitimately return `0`
- no guaranteed per-map fallback boss exists

### 3. “Dungeons do not end due to no end boss”

Root cause:

- if no boss spawns, `TotalBosses` remains `0`
- completion logic only triggers when `TotalBosses > 0` and kill count reaches it
- therefore the run can become unwinnable and never complete

## Recommended overhaul direction

## Phase 1 — correctness and safeguards

Must be done first.

1. guarantee at least one valid completion boss for every run
2. add hard Challenge Modes block
3. add Playerbot ownership validation
4. make elite config fully respected
5. make elites visibly elite
6. add a fail-safe completion path if boss spawn fails
7. make spawn budget independent from raw DB spawn count

## Phase 2 — map and boss redesign

1. replace global boss-only logic with a hybrid model:
	- map-native bosses first
	- curated cross-map substitutes only where intentional
2. add per-map spawn budgets
3. add per-map theme relaxation rules
4. add map exclusions / warnings for poor procedural fit

## Phase 3 — weighting overhaul

1. weighted dungeon selection by size, difficulty, and recent history
2. weighted creature pools by map identity and theme compatibility
3. weighted boss pools by map, theme, and tier
4. weighted affix pools with exclusion rules and curated pairings

## Phase 4 — rewards and progression

1. make reward config actually authoritative
2. redesign normal vs roguelike reward economy explicitly
3. normalize rewards by runtime effort, not just level and kill count

## Phase 5 — UI overhaul

1. redesign gossip into a stronger fallback UX
2. add AIO client menu as preferred path
3. surface previews for:
	- map
	- difficulty
	- affixes
	- rewards
	- warnings / incompatibilities
4. improve leaderboard drill-down and player status visibility

## Immediate implementation priority order

If implementation is approved, the recommended order is:

1. boss guarantee + completion safety
2. Challenge Modes block
3. Playerbot ownership gating
4. elite visibility + config correctness
5. spawn budgeting by map
6. boss model redesign
7. weighting redesign
8. reward redesign
9. UI/AIO redesign
10. logging improvements

## Approval checkpoint

This audit intentionally stops before code changes.

Recommended next step after approval:

- produce a phased implementation plan
- execute Phase 1 first
- validate after each phase with build + targeted in-game checks


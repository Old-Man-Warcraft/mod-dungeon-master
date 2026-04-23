# mod-dungeon-master — Leaderboard database reference

This document describes the **characters database** tables used for Dungeon Master leaderboards so you can build a companion website (API, caching, rankings, filters).

**Scope:** Leaderboard tables only. World-database content (NPC spawns, `creature_template`) lives in `data/sql/db-world/base/dm_setup.sql` and is not needed for a web leaderboard.

---

## Which database?

All leaderboard and DM player statistics tables are created on the **characters** database (commonly named `acore_characters` on AzerothCore), **not** on `acore_world`.

Install / migrate script in the repo:

- `data/sql/db-characters/base/dm_characters_setup.sql`

---

## Mental model

1. **Rows are run records, not one row per player.** Every successful save inserts a **new** row. A single character (`guid`) can appear many times. Ranks are computed in application code or SQL with `ORDER BY` and optional window functions — there is **no** `rank` column in the database.

2. **Two parallel systems:** **Normal** procedural dungeon runs (`dm_leaderboard`, `dm_player_stats`) and **Roguelike** runs (`dm_roguelike_leaderboard`, `dm_roguelike_player_stats`). They do not share rows.

3. **In-game NPC vs. raw data:** The Dungeon Master NPC “Normal — Fastest Clears” view loads the **top N rows globally** ordered by `clear_time` ascending, **across all maps and difficulty tiers** (see *Query patterns* below). Your site can match that behavior or offer per-dungeon / per-difficulty boards using the same columns.

---

## Table: `dm_leaderboard` (normal runs)

Stores one row per recorded clear (fastest-time oriented). Columns match what the module inserts and reads in `DungeonMasterMgr::SaveLeaderboardEntry` / `GetLeaderboard` / `GetOverallLeaderboard`.

| Column | Type | Meaning |
|--------|------|---------|
| `id` | `INT UNSIGNED` | Auto-increment primary key (insert order). Not used for ranking. |
| `guid` | `INT UNSIGNED` | **Party leader’s** character low GUID (`ObjectGuid::GetCounter()`), same key space as `characters.guid`. |
| `char_name` | `VARCHAR(48)` | Leader’s name **at save time**. Can differ from `characters.name` after rename; useful for display without joining. |
| `map_id` | `INT UNSIGNED` | Instance map ID of the run (WotLK 5-man IDs are hard-coded in `DMConfig::LoadDungeons` — e.g. `574` Utgarde Keep). |
| `difficulty_id` | `INT UNSIGNED` | **Dungeon Master difficulty tier ID** from `DungeonMaster.Difficulty.N` in `mod_dungeon_master.conf` (default `1`–`6`: Novice … Grandmaster). **This is not** WoW’s `Difficulty` / `DUNGEON_DIFFICULTY` enum (normal/heroic). |
| `clear_time` | `INT UNSIGNED` | Clear duration in **whole seconds** (end − start; see server code if `EndTime` unset). **Lower is better** for ranking. |
| `party_size` | `TINYINT UNSIGNED` | Number of players in the session when saved. |
| `scaled` | `TINYINT UNSIGNED` | `1` if party scaling was enabled (`ScaleToParty`), else `0`. |
| `effective_level` | `TINYINT UNSIGNED` | Effective level used for mob level banding (solo level or party average, clamped to tier — see module docs / config). |
| `mobs_killed` | `INT UNSIGNED` | Sum of trash kills **across all party members** for that run. |
| `bosses_killed` | `INT UNSIGNED` | Sum of boss kills **across all party members** for that run. |
| `deaths` | `INT UNSIGNED` | Sum of deaths **across all party members** for that run. |
| `completed_at` | `DATETIME` | Server timestamp when the row was inserted (default `CURRENT_TIMESTAMP`). **Present in SQL**; the in-game C++ leaderboard readers **do not** select it, but it is ideal for a website (“last 7 days”, “recent PBs”). |

**Indexes (from setup SQL):**

- `(map_id, difficulty_id, clear_time)` — filter by dungeon + tier, sort by time.
- `(clear_time)` — global fastest lists.
- `(guid)` — “all my runs” or highlight by player.

---

## Table: `dm_roguelike_leaderboard` (roguelike runs)

One row per roguelike run when the module persists a result (`RoguelikeMgr::SaveRoguelikeLeaderboard`).

| Column | Type | Meaning |
|--------|------|---------|
| `id` | `INT UNSIGNED` | Auto-increment PK. |
| `guid` | `INT UNSIGNED` | Run leader’s character low GUID. |
| `char_name` | `VARCHAR(64)` | Leader name at save time. |
| `tier_reached` | `INT UNSIGNED` | Highest **roguelike tier** reached on that run (`CurrentTier` in code). Higher is better for “Highest Tier” boards. |
| `dungeons_cleared` | `INT UNSIGNED` | Number of **floors** / dungeons cleared in that run (shown as “floors” in gossip). |
| `total_kills` | `INT UNSIGNED` | **Denormalized:** `TotalMobsKilled + TotalBossesKilled` at save time. |
| `total_bosses` | `INT UNSIGNED` | Boss kill count for the run. |
| `total_deaths` | `INT UNSIGNED` | Death count for the run. |
| `run_duration` | `INT UNSIGNED` | Run length in **seconds** (wall clock from run start to save). Used as tie-breaker: **shorter is better** when tier/floors are equal. |
| `party_size` | `TINYINT UNSIGNED` | Players in the run. |
| `completed_at` | `DATETIME` | When the row was inserted. |

**Sort orders used in-game** (`GetRoguelikeLeaderboard`):

- **Highest Tier:** `ORDER BY tier_reached DESC, dungeons_cleared DESC, run_duration ASC`
- **Most Floors:** `ORDER BY dungeons_cleared DESC, tier_reached DESC, run_duration ASC`

---

## Related aggregate tables (optional for a richer site)

These are **not** leaderboards but pair well on a profile page.

| Table | Purpose |
|-------|---------|
| `dm_player_stats` | Per-character normal-mode totals: `total_runs`, `completed_runs`, `failed_runs`, kill/death totals, `fastest_clear` (best seconds across all dungeons). Keyed by `guid`. |
| `dm_roguelike_player_stats` | Per-character roguelike totals: `highest_tier`, `most_floors_cleared`, cumulative floors/kills/deaths, `longest_run_time`, etc. Keyed by `guid`. |

---

## Resolving `map_id` and `difficulty_id` for display

- **`map_id`:** Map IDs are standard WoW instance map IDs. The module’s built-in display names come from the static list in `DMConfig::LoadDungeons()` (Classic / TBC / WotLK 5-mans). For a website you can copy that map → name table, query `acore_world`.`instance_template` if your core uses it for names, or maintain your own JSON.

- **`difficulty_id`:** Matches **config line number** `DungeonMaster.Difficulty.<id>` (see `mod_dungeon_master.conf.dist`). Default names: Novice, Apprentice, Journeyman, Expert, Master, Grandmaster for IDs 1–6. **If the server operator changes config,** labels and IDs on the site should follow the same file or a synced copy — the DB only stores the numeric id.

---

## Query patterns (copy-paste starting points)

**Important:** Always use parameterized queries in production; literals below are for clarity.

### Normal — match in-game “top 10 fastest clears” (global, any map/tier)

```sql
SELECT id, guid, char_name, map_id, difficulty_id, clear_time,
       party_size, scaled, effective_level, mobs_killed, bosses_killed, deaths,
       completed_at
FROM dm_leaderboard
ORDER BY clear_time ASC
LIMIT 10;
```

### Normal — best row per player for a specific dungeon and tier (window function)

Picks each player’s fastest clear only (one row per `guid` when ties are broken by `id`).

```sql
SELECT * FROM (
  SELECT lb.*,
         ROW_NUMBER() OVER (
           PARTITION BY guid
           ORDER BY clear_time ASC, id ASC
         ) AS rn
  FROM dm_leaderboard lb
  WHERE map_id = 574 AND difficulty_id = 6
) t
WHERE rn = 1
ORDER BY clear_time ASC;
```

### Normal — true rank for one run row (window function)

```sql
SELECT lb.*,
       RANK() OVER (PARTITION BY map_id, difficulty_id ORDER BY clear_time ASC) AS rank_in_map_tier
FROM dm_leaderboard lb;
```

### Roguelike — match in-game “Highest Tier” top 10

```sql
SELECT id, guid, char_name, tier_reached, dungeons_cleared,
       total_kills, total_bosses, total_deaths, run_duration, party_size, completed_at
FROM dm_roguelike_leaderboard
ORDER BY tier_reached DESC, dungeons_cleared DESC, run_duration ASC
LIMIT 10;
```

### Roguelike — match in-game “Most Floors” top 10

```sql
SELECT id, guid, char_name, tier_reached, dungeons_cleared,
       total_kills, total_bosses, total_deaths, run_duration, party_size, completed_at
FROM dm_roguelike_leaderboard
ORDER BY dungeons_cleared DESC, tier_reached DESC, run_duration ASC
LIMIT 10;
```

### Optional join to live character name

```sql
SELECT lb.*, c.name AS current_name
FROM dm_leaderboard lb
LEFT JOIN characters c ON c.guid = lb.guid
ORDER BY lb.clear_time ASC
LIMIT 50;
```

---

## Implementation notes for a web companion

1. **Read replica / caching:** The worldserver writes on completion; read-heavy leaderboard pages should use a DB replica or periodic cache refresh to avoid hammering the primary.

2. **Roguelike `total_kills`:** Remember it is **mobs + bosses** at save. For breakdown, use `total_bosses` and derive mob kills as `total_kills - total_bosses` only if that invariant always holds for your server version.

3. **Ties:** Normal ranking is strictly by `clear_time`; roguelike uses tier, then floors, then duration. Document tie-breakers on the UI.

4. **Security:** If you expose SQL-backed APIs, never trust `char_name` alone for authorization; use your auth layer and treat `guid` as the stable key alongside realm id in multi-realm setups.

5. **Code references:** Inserts/selects live in `DungeonMasterMgr.cpp` (`dm_leaderboard`) and `RoguelikeMgr.cpp` (`dm_roguelike_leaderboard`); gossip formatting in `npc_dungeon_master.cpp`.

---

## File map in this repository

| Artifact | Path |
|----------|------|
| Characters schema + indexes | `data/sql/db-characters/base/dm_characters_setup.sql` |
| C++ structs (field names) | `src/DMTypes.h` (`LeaderboardEntry`), `src/RoguelikeTypes.h` (`RoguelikeLeaderboardEntry`) |
| SQL read/write | `src/DungeonMasterMgr.cpp`, `src/RoguelikeMgr.cpp` |

---

*Generated from mod-dungeon-master sources in the OMW AzerothCore tree; verify against your deployed SQL if you have local migrations beyond `dm_characters_setup.sql`.*

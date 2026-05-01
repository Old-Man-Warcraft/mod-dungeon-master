--[[
  mod-dungeon-master — server-side AIO bridge for Dungeon Master UI.
  Requires: mod-ale (Eluna), Rochet2 AIO in lua_scripts/AIO_Server/AIO.lua

  This first-pass bridge powers a richer client window for:
    - run planning / previews
    - normal + roguelike stats
    - normal + roguelike leaderboards

  Final run creation still happens through the Dungeon Master NPC / C++ module,
  so the preview UI stays honest about server-side validation and launch flow.
]]

local function load_aio()
    if AIO then
        return true
    end

    local paths = {
        "lua_scripts/AIO_Server/AIO.lua",
        "AIO_Server/AIO.lua",
    }

    for _, path in ipairs(paths) do
        local chunk = loadfile(path)
        if chunk then
            chunk()
            return true
        end
    end

    print("[DungeonMasterAIO] FATAL: could not load AIO")
    return false
end

if not load_aio() or not AIO or not AIO.IsServer() or not AIO.IsMainState() then
    return
end

local CHANNEL = "DMUI"
local MAX_ROWS = 50
local CLIENT_ADDON_NAME = "dungeon_master_aio_client_dmui_v20260429_1"

local DEFAULT_REWARDS = {
    baseGold = 50000,
    goldPerMob = 50,
    goldPerBoss = 10000,
    xpMultiplier = 1.0,
    itemChance = 80,
    rareChance = 40,
    epicChance = 15,
}

local DEFAULT_DIFFICULTIES = {
    { id = 1, name = "Novice", minLevel = 10, maxLevel = 19, healthMult = 0.6, damageMult = 0.6, rewardMult = 1.0, mobMult = 0.5 },
    { id = 2, name = "Apprentice", minLevel = 20, maxLevel = 29, healthMult = 0.8, damageMult = 0.8, rewardMult = 1.5, mobMult = 0.7 },
    { id = 3, name = "Journeyman", minLevel = 30, maxLevel = 44, healthMult = 1.0, damageMult = 1.0, rewardMult = 2.0, mobMult = 0.85 },
    { id = 4, name = "Expert", minLevel = 45, maxLevel = 59, healthMult = 1.3, damageMult = 1.2, rewardMult = 3.0, mobMult = 1.0 },
    { id = 5, name = "Master", minLevel = 60, maxLevel = 69, healthMult = 1.6, damageMult = 1.4, rewardMult = 4.0, mobMult = 1.0 },
    { id = 6, name = "Grandmaster", minLevel = 70, maxLevel = 80, healthMult = 2.0, damageMult = 1.6, rewardMult = 6.0, mobMult = 1.2 },
}

local DEFAULT_THEMES = {
    { id = 1, name = "Beast Hunt", types = { 1 } },
    { id = 2, name = "Dragon's Lair", types = { 2 } },
    { id = 3, name = "Demonic Invasion", types = { 3 } },
    { id = 4, name = "Elemental Chaos", types = { 4 } },
    { id = 5, name = "Giant's Keep", types = { 5 } },
    { id = 6, name = "Undead Rising", types = { 6 } },
    { id = 7, name = "Humanoid Stronghold", types = { 7 } },
    { id = 8, name = "Mechanical Mayhem", types = { 9 } },
    { id = 9, name = "Random Chaos", types = { -1 } },
}

local DUNGEONS = {
    { mapId = 389, name = "Ragefire Chasm", minLevel = 13, maxLevel = 20, expansion = "Classic" },
    { mapId = 36, name = "Deadmines", minLevel = 15, maxLevel = 25, expansion = "Classic" },
    { mapId = 33, name = "Shadowfang Keep", minLevel = 18, maxLevel = 28, expansion = "Classic" },
    { mapId = 34, name = "The Stockade", minLevel = 20, maxLevel = 30, expansion = "Classic" },
    { mapId = 43, name = "Wailing Caverns", minLevel = 15, maxLevel = 28, expansion = "Classic" },
    { mapId = 48, name = "Blackfathom Deeps", minLevel = 20, maxLevel = 32, expansion = "Classic" },
    { mapId = 47, name = "Razorfen Kraul", minLevel = 25, maxLevel = 35, expansion = "Classic" },
    { mapId = 90, name = "Gnomeregan", minLevel = 25, maxLevel = 38, expansion = "Classic" },
    { mapId = 129, name = "Razorfen Downs", minLevel = 35, maxLevel = 45, expansion = "Classic" },
    { mapId = 189, name = "Scarlet Monastery", minLevel = 30, maxLevel = 45, expansion = "Classic" },
    { mapId = 70, name = "Uldaman", minLevel = 38, maxLevel = 50, expansion = "Classic" },
    { mapId = 209, name = "Zul'Farrak", minLevel = 42, maxLevel = 52, expansion = "Classic" },
    { mapId = 349, name = "Maraudon", minLevel = 40, maxLevel = 52, expansion = "Classic" },
    { mapId = 109, name = "Sunken Temple", minLevel = 45, maxLevel = 55, expansion = "Classic" },
    { mapId = 230, name = "Blackrock Depths", minLevel = 48, maxLevel = 60, expansion = "Classic" },
    { mapId = 229, name = "Blackrock Spire", minLevel = 52, maxLevel = 60, expansion = "Classic" },
    { mapId = 289, name = "Scholomance", minLevel = 55, maxLevel = 60, expansion = "Classic" },
    { mapId = 329, name = "Stratholme", minLevel = 55, maxLevel = 60, expansion = "Classic" },
    { mapId = 543, name = "Hellfire Ramparts", minLevel = 58, maxLevel = 70, expansion = "TBC" },
    { mapId = 542, name = "Blood Furnace", minLevel = 59, maxLevel = 70, expansion = "TBC" },
    { mapId = 547, name = "Slave Pens", minLevel = 60, maxLevel = 70, expansion = "TBC" },
    { mapId = 546, name = "Underbog", minLevel = 61, maxLevel = 70, expansion = "TBC" },
    { mapId = 557, name = "Mana-Tombs", minLevel = 62, maxLevel = 70, expansion = "TBC" },
    { mapId = 558, name = "Auchenai Crypts", minLevel = 63, maxLevel = 70, expansion = "TBC" },
    { mapId = 556, name = "Sethekk Halls", minLevel = 65, maxLevel = 70, expansion = "TBC" },
    { mapId = 555, name = "Shadow Labyrinth", minLevel = 68, maxLevel = 70, expansion = "TBC" },
    { mapId = 540, name = "Shattered Halls", minLevel = 68, maxLevel = 70, expansion = "TBC" },
    { mapId = 553, name = "Botanica", minLevel = 68, maxLevel = 70, expansion = "TBC" },
    { mapId = 554, name = "Mechanar", minLevel = 68, maxLevel = 70, expansion = "TBC" },
    { mapId = 552, name = "Arcatraz", minLevel = 68, maxLevel = 70, expansion = "TBC" },
    { mapId = 574, name = "Utgarde Keep", minLevel = 68, maxLevel = 80, expansion = "WotLK" },
    { mapId = 576, name = "The Nexus", minLevel = 69, maxLevel = 80, expansion = "WotLK" },
    { mapId = 601, name = "Azjol-Nerub", minLevel = 70, maxLevel = 80, expansion = "WotLK" },
    { mapId = 619, name = "Ahn'kahet", minLevel = 71, maxLevel = 80, expansion = "WotLK" },
    { mapId = 600, name = "Drak'Tharon Keep", minLevel = 72, maxLevel = 80, expansion = "WotLK" },
    { mapId = 608, name = "Violet Hold", minLevel = 73, maxLevel = 80, expansion = "WotLK" },
    { mapId = 604, name = "Gundrak", minLevel = 74, maxLevel = 80, expansion = "WotLK" },
    { mapId = 599, name = "Halls of Stone", minLevel = 75, maxLevel = 80, expansion = "WotLK" },
    { mapId = 602, name = "Halls of Lightning", minLevel = 77, maxLevel = 80, expansion = "WotLK" },
    { mapId = 578, name = "The Oculus", minLevel = 77, maxLevel = 80, expansion = "WotLK" },
    { mapId = 575, name = "Utgarde Pinnacle", minLevel = 78, maxLevel = 80, expansion = "WotLK" },
    { mapId = 595, name = "Culling of Stratholme", minLevel = 78, maxLevel = 80, expansion = "WotLK" },
    { mapId = 632, name = "Forge of Souls", minLevel = 79, maxLevel = 80, expansion = "WotLK" },
    { mapId = 658, name = "Pit of Saron", minLevel = 79, maxLevel = 80, expansion = "WotLK" },
    { mapId = 668, name = "Halls of Reflection", minLevel = 79, maxLevel = 80, expansion = "WotLK" },
}

local function clamp_u32(n, default)
    n = tonumber(n)
    if not n or n ~= n then
        return default
    end
    n = math.floor(n)
    if n < 0 then
        return default
    end
    if n > 0xFFFFFFFF then
        return 0xFFFFFFFF
    end
    return n
end

local function clamp_limit(limit)
    limit = clamp_u32(limit, 10)
    if limit < 1 then
        return 1
    end
    if limit > MAX_ROWS then
        return MAX_ROWS
    end
    return limit
end

local function trim(s)
    return (tostring(s or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function strip_quotes(s)
    s = trim(s)
    if s:sub(1, 1) == '"' and s:sub(-1, -1) == '"' then
        return s:sub(2, -2)
    end
    return s
end

local function split_csv(s)
    local out = {}
    for token in string.gmatch(tostring(s or ""), "([^,]+)") do
        out[#out + 1] = trim(token)
    end
    return out
end

local function shallow_copy_list(src)
    local out = {}
    for i = 1, #src do
        local row = {}
        for k, v in pairs(src[i]) do
            if type(v) == "table" then
                local copy = {}
                for j = 1, #v do
                    copy[j] = v[j]
                end
                row[k] = copy
            else
                row[k] = v
            end
        end
        out[i] = row
    end
    return out
end

local function load_config_map()
    local paths = {
        "../etc/modules/mod_dungeon_master.conf",
        "../etc/modules/mod_dungeon_master.conf.dist",
        "env/dist/etc/modules/mod_dungeon_master.conf",
        "env/dist/etc/modules/mod_dungeon_master.conf.dist",
        "modules/mod-dungeon-master/conf/mod_dungeon_master.conf.dist",
        "mod_dungeon_master.conf",
        "mod_dungeon_master.conf.dist",
    }

    for _, path in ipairs(paths) do
        local f = io.open(path, "r")
        if f then
            local map = {}
            for line in f:lines() do
                if not line:match("^%s*#") and line:find("=") then
                    local key, value = line:match("^%s*([^=]+)%s*=%s*(.-)%s*$")
                    if key and value and key ~= "" then
                        map[trim(key)] = trim(value)
                    end
                end
            end
            f:close()
            return map
        end
    end

    return {}
end

local function load_difficulties(cfg)
    local rows = {}
    for i = 1, 10 do
        local raw = cfg["DungeonMaster.Difficulty." .. i]
        if raw and raw ~= "\"\"" and raw ~= "" then
            local parts = split_csv(strip_quotes(raw))
            if #parts >= 7 then
                rows[#rows + 1] = {
                    id = i,
                    name = parts[1],
                    minLevel = tonumber(parts[2]) or 1,
                    maxLevel = tonumber(parts[3]) or 80,
                    healthMult = tonumber(parts[4]) or 1.0,
                    damageMult = tonumber(parts[5]) or 1.0,
                    rewardMult = tonumber(parts[6]) or 1.0,
                    mobMult = tonumber(parts[7]) or 1.0,
                }
            end
        end
    end

    if #rows == 0 then
        rows = shallow_copy_list(DEFAULT_DIFFICULTIES)
    end

    return rows
end

local function load_themes(cfg)
    local rows = {}
    for i = 1, 20 do
        local raw = cfg["DungeonMaster.Theme." .. i]
        if raw and raw ~= "\"\"" and raw ~= "" then
            local parts = split_csv(strip_quotes(raw))
            if #parts >= 2 then
                local theme = { id = i, name = parts[1], types = {} }
                for idx = 2, #parts do
                    theme.types[#theme.types + 1] = tonumber(parts[idx]) or -1
                end
                rows[#rows + 1] = theme
            end
        end
    end

    if #rows == 0 then
        rows = shallow_copy_list(DEFAULT_THEMES)
    end

    return rows
end

local function load_rewards(cfg)
    return {
        baseGold = tonumber(cfg["DungeonMaster.Rewards.BaseGold"]) or DEFAULT_REWARDS.baseGold,
        goldPerMob = tonumber(cfg["DungeonMaster.Rewards.GoldPerMob"]) or DEFAULT_REWARDS.goldPerMob,
        goldPerBoss = tonumber(cfg["DungeonMaster.Rewards.GoldPerBoss"]) or DEFAULT_REWARDS.goldPerBoss,
        xpMultiplier = tonumber(cfg["DungeonMaster.Rewards.XPMultiplier"]) or DEFAULT_REWARDS.xpMultiplier,
        itemChance = tonumber(cfg["DungeonMaster.Rewards.ItemChance"]) or DEFAULT_REWARDS.itemChance,
        rareChance = tonumber(cfg["DungeonMaster.Rewards.RareChance"]) or DEFAULT_REWARDS.rareChance,
        epicChance = tonumber(cfg["DungeonMaster.Rewards.EpicChance"]) or DEFAULT_REWARDS.epicChance,
    }
end

local function get_runtime_catalog()
    local cfg = load_config_map()
    return {
        difficulties = load_difficulties(cfg),
        themes = load_themes(cfg),
        dungeons = shallow_copy_list(DUNGEONS),
        rewards = load_rewards(cfg),
    }
end

local function list_find_by_id(rows, id, key)
    key = key or "id"
    id = tonumber(id) or 0
    for i = 1, #rows do
        if tonumber(rows[i][key]) == id then
            return rows[i]
        end
    end
    return nil
end

local function query_player_stats(guidLow)
    local q = CharDBQuery(string.format(
        "SELECT total_runs, completed_runs, failed_runs, total_mobs_killed, total_bosses_killed, total_deaths, fastest_clear FROM dm_player_stats WHERE guid = %u LIMIT 1",
        guidLow
    ))

    if not q or q:GetRowCount() == 0 then
        return {
            totalRuns = 0,
            completedRuns = 0,
            failedRuns = 0,
            totalMobsKilled = 0,
            totalBossesKilled = 0,
            totalDeaths = 0,
            fastestClear = 0,
        }
    end

    return {
        totalRuns = q:GetUInt32(0),
        completedRuns = q:GetUInt32(1),
        failedRuns = q:GetUInt32(2),
        totalMobsKilled = q:GetUInt32(3),
        totalBossesKilled = q:GetUInt32(4),
        totalDeaths = q:GetUInt32(5),
        fastestClear = q:GetUInt32(6),
    }
end

local function query_roguelike_stats(guidLow)
    local q = CharDBQuery(string.format(
        "SELECT total_runs, highest_tier, most_floors_cleared, total_floors_cleared, total_mobs_killed, total_bosses_killed, total_deaths, longest_run_time FROM dm_roguelike_player_stats WHERE guid = %u LIMIT 1",
        guidLow
    ))

    if not q or q:GetRowCount() == 0 then
        return {
            totalRuns = 0,
            highestTier = 0,
            mostFloorsCleared = 0,
            totalFloorsCleared = 0,
            totalMobsKilled = 0,
            totalBossesKilled = 0,
            totalDeaths = 0,
            longestRunTime = 0,
        }
    end

    return {
        totalRuns = q:GetUInt32(0),
        highestTier = q:GetUInt32(1),
        mostFloorsCleared = q:GetUInt32(2),
        totalFloorsCleared = q:GetUInt32(3),
        totalMobsKilled = q:GetUInt32(4),
        totalBossesKilled = q:GetUInt32(5),
        totalDeaths = q:GetUInt32(6),
        longestRunTime = q:GetUInt32(7),
    }
end

local function safe_group_size(player)
    local group = player and player:GetGroup() or nil
    if not group then
        return 1
    end

    local ok, size = pcall(function()
        return group:GetMembersCount()
    end)
    if ok and tonumber(size) then
        return tonumber(size)
    end

    return 5
end

local function build_preview(player, mode, difficultyId, themeId, mapId, scaleMode)
    local catalog = get_runtime_catalog()
    local difficulty = list_find_by_id(catalog.difficulties, difficultyId, "id") or catalog.difficulties[1]
    local theme = list_find_by_id(catalog.themes, themeId, "id") or catalog.themes[#catalog.themes]
    local dungeon = list_find_by_id(catalog.dungeons, mapId, "mapId")
    local isRoguelike = tostring(mode or "normal") == "roguelike"
    local scaleToParty = tostring(scaleMode or "party") ~= "tier"
    local playerLevel = player and player:GetLevel() or 1
    local partySize = safe_group_size(player)
    local warnings = {}
    local notes = {}

    if difficulty and playerLevel < difficulty.minLevel then
        warnings[#warnings + 1] = string.format("Your level (%u) is below %s's minimum requirement (%u).",
            playerLevel, difficulty.name or "this difficulty", difficulty.minLevel or 1)
    end

    if partySize > 5 then
        warnings[#warnings + 1] = string.format("Party size appears to be %u. Dungeon Master supports up to 5 players.", partySize)
    end

    if mapId == 0 then
        notes[#notes + 1] = "Weighted random dungeon selection favors healthier map sizes and stronger native boss coverage."
    elseif dungeon then
        notes[#notes + 1] = string.format("Selected map: %s (%s, level %u-%u).",
            dungeon.name, dungeon.expansion, dungeon.minLevel, dungeon.maxLevel)
    end

    notes[#notes + 1] = string.format("Scaling mode: %s.", scaleToParty and "Party Level" or "Dungeon Difficulty")
    notes[#notes + 1] = string.format("Final eligibility still runs through server-side validation (Challenge Modes, Playerbots, cooldowns, and active session checks).")

    if isRoguelike then
        notes[#notes + 1] = "Roguelike floors escalate with tier scaling, affixes, and end-run rewards."
    else
        notes[#notes + 1] = "Final launch still happens through the Dungeon Master NPC while this AIO layer handles planning, previews, and stats."
    end

    local rewardMult = difficulty and difficulty.rewardMult or 1.0
    local expectedBaseGold = math.floor((catalog.rewards.baseGold or DEFAULT_REWARDS.baseGold) * rewardMult + 0.5)

    return {
        mode = isRoguelike and "roguelike" or "normal",
        modeLabel = isRoguelike and "Roguelike" or "Standard Challenge",
        scaleLabel = scaleToParty and "Party Level" or "Dungeon Difficulty",
        playerLevel = playerLevel,
        partySize = partySize,
        difficultyId = difficulty and difficulty.id or 0,
        difficultyName = difficulty and difficulty.name or "Unknown",
        difficultyMinLevel = difficulty and difficulty.minLevel or 1,
        difficultyMaxLevel = difficulty and difficulty.maxLevel or 80,
        healthMult = difficulty and difficulty.healthMult or 1.0,
        damageMult = difficulty and difficulty.damageMult or 1.0,
        rewardMult = rewardMult,
        mobMult = difficulty and difficulty.mobMult or 1.0,
        themeId = theme and theme.id or 0,
        themeName = theme and theme.name or "Unknown",
        mapId = mapId or 0,
        dungeonName = dungeon and dungeon.name or "Weighted Random Dungeon",
        dungeonExpansion = dungeon and dungeon.expansion or "Mixed",
        dungeonMinLevel = dungeon and dungeon.minLevel or 0,
        dungeonMaxLevel = dungeon and dungeon.maxLevel or 0,
        expectedBaseGold = expectedBaseGold,
        itemChance = catalog.rewards.itemChance,
        rareChance = catalog.rewards.rareChance,
        epicChance = catalog.rewards.epicChance,
        warnings = warnings,
        notes = notes,
    }
end

local function query_normal_board(limit, mapId, difficultyId)
    limit = clamp_limit(limit)
    mapId = clamp_u32(mapId, 0)
    difficultyId = clamp_u32(difficultyId, 0)

    local where = {}
    if mapId > 0 then
        where[#where + 1] = string.format("map_id = %u", mapId)
    end
    if difficultyId > 0 then
        where[#where + 1] = string.format("difficulty_id = %u", difficultyId)
    end

    local sql = "SELECT char_name, map_id, difficulty_id, clear_time, party_size, scaled, effective_level, mobs_killed, bosses_killed, deaths, completed_at FROM dm_leaderboard"
    if #where > 0 then
        sql = sql .. " WHERE " .. table.concat(where, " AND ")
    end
    sql = sql .. string.format(" ORDER BY clear_time ASC LIMIT %u", limit)

    local rows = {}
    local q = CharDBQuery(sql)
    if not q or q:GetRowCount() == 0 then
        return rows
    end

    repeat
        rows[#rows + 1] = {
            charName = q:GetString(0),
            mapId = q:GetUInt32(1),
            difficultyId = q:GetUInt32(2),
            clearTime = q:GetUInt32(3),
            partySize = q:GetUInt8(4),
            scaled = q:GetUInt8(5) ~= 0,
            effectiveLevel = q:GetUInt8(6),
            mobsKilled = q:GetUInt32(7),
            bossesKilled = q:GetUInt32(8),
            deaths = q:GetUInt32(9),
            completedAt = q:GetString(10),
        }
    until not q:NextRow()

    return rows
end

local function query_roguelike_board(limit, sortByFloors)
    limit = clamp_limit(limit)
    local sql
    if sortByFloors then
        sql = string.format(
            "SELECT char_name, tier_reached, dungeons_cleared, total_kills, total_bosses, total_deaths, run_duration, party_size, completed_at FROM dm_roguelike_leaderboard ORDER BY dungeons_cleared DESC, tier_reached DESC, run_duration ASC LIMIT %u",
            limit
        )
    else
        sql = string.format(
            "SELECT char_name, tier_reached, dungeons_cleared, total_kills, total_bosses, total_deaths, run_duration, party_size, completed_at FROM dm_roguelike_leaderboard ORDER BY tier_reached DESC, dungeons_cleared DESC, run_duration ASC LIMIT %u",
            limit
        )
    end

    local rows = {}
    local q = CharDBQuery(sql)
    if not q or q:GetRowCount() == 0 then
        return rows
    end

    repeat
        rows[#rows + 1] = {
            charName = q:GetString(0),
            tierReached = q:GetUInt32(1),
            dungeonsCleared = q:GetUInt32(2),
            totalKills = q:GetUInt32(3),
            totalBosses = q:GetUInt32(4),
            totalDeaths = q:GetUInt32(5),
            runDuration = q:GetUInt32(6),
            partySize = q:GetUInt8(7),
            completedAt = q:GetString(8),
        }
    until not q:NextRow()

    return rows
end

AIO.AddHandlers(CHANNEL, {
    ReqBootstrap = function(player)
        local catalog = get_runtime_catalog()
        local guidLow = player:GetGUIDLow()
        AIO.Handle(player, CHANNEL, "PushBootstrap",
            player:GetName(),
            player:GetLevel(),
            safe_group_size(player),
            catalog.difficulties,
            catalog.themes,
            catalog.dungeons,
            catalog.rewards,
            query_player_stats(guidLow),
            query_roguelike_stats(guidLow),
            "npc")
    end,

    ReqPreview = function(player, mode, difficultyId, themeId, mapId, scaleMode)
        local preview = build_preview(player, mode, difficultyId, themeId, mapId, scaleMode)
        AIO.Handle(player, CHANNEL, "PushPreview",
            preview.mode,
            preview.modeLabel,
            preview.scaleLabel,
            preview.playerLevel,
            preview.partySize,
            preview.difficultyId,
            preview.difficultyName,
            preview.difficultyMinLevel,
            preview.difficultyMaxLevel,
            preview.healthMult,
            preview.damageMult,
            preview.rewardMult,
            preview.mobMult,
            preview.themeId,
            preview.themeName,
            preview.mapId,
            preview.dungeonName,
            preview.dungeonExpansion,
            preview.dungeonMinLevel,
            preview.dungeonMaxLevel,
            preview.expectedBaseGold,
            preview.itemChance,
            preview.rareChance,
            preview.epicChance,
            preview.warnings,
            preview.notes)
    end,

    ReqNormalBoard = function(player, limit, mapId, difficultyId)
        AIO.Handle(player, CHANNEL, "PushNormalBoard", query_normal_board(limit, mapId, difficultyId), clamp_u32(mapId, 0), clamp_u32(difficultyId, 0), clamp_limit(limit))
    end,

    ReqRoguelikeBoard = function(player, limit, sortByFloors)
        AIO.Handle(player, CHANNEL, "PushRoguelikeBoard", query_roguelike_board(limit, sortByFloors and true or false), sortByFloors and true or false, clamp_limit(limit))
    end,
})

local client_paths = {
    "lua_scripts/dungeon_master_aio_client.lua",
    "dungeon_master_aio_client.lua",
}

local added = false
for _, rel in ipairs(client_paths) do
    if AIO.AddAddon(rel, CLIENT_ADDON_NAME) then
        added = true
        print("[DungeonMasterAIO] client addon registered: " .. rel .. " as " .. CLIENT_ADDON_NAME)
        break
    end
end

if not added then
    print("[DungeonMasterAIO] WARNING: client file not found")
end

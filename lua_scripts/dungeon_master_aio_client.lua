--[[
  mod-dungeon-master — AIO client UI for Dungeon Master planning, stats, and leaderboards.
  3.3.5a — preferred planning UI, with final run launch still handled by the Dungeon Master NPC.
]]

local AIO = AIO or require("AIO")
if AIO.AddAddon() then
    return
end

local CHANNEL = "DMUI"
local PLAYER_NAME = UnitName("player") or "Player"

local SAVED_MODE_KEY = "DungeonMasterAIO_Mode"
local SAVED_DIFF_KEY = "DungeonMasterAIO_Difficulty"
local SAVED_THEME_KEY = "DungeonMasterAIO_Theme"
local SAVED_MAP_KEY = "DungeonMasterAIO_Map"
local SAVED_SCALE_KEY = "DungeonMasterAIO_Scale"
local SAVED_LIMIT_KEY = "DungeonMasterAIO_Limit"
local SAVED_ROGUE_SORT_KEY = "DungeonMasterAIO_RogueSort"

if AIO.AddSavedVarChar then
    AIO.AddSavedVarChar(SAVED_MODE_KEY)
    AIO.AddSavedVarChar(SAVED_DIFF_KEY)
    AIO.AddSavedVarChar(SAVED_THEME_KEY)
    AIO.AddSavedVarChar(SAVED_MAP_KEY)
    AIO.AddSavedVarChar(SAVED_SCALE_KEY)
    AIO.AddSavedVarChar(SAVED_LIMIT_KEY)
    AIO.AddSavedVarChar(SAVED_ROGUE_SORT_KEY)
end

_G[SAVED_MODE_KEY] = _G[SAVED_MODE_KEY] or "normal"
_G[SAVED_DIFF_KEY] = tonumber(_G[SAVED_DIFF_KEY]) or 1
_G[SAVED_THEME_KEY] = tonumber(_G[SAVED_THEME_KEY]) or 9
_G[SAVED_MAP_KEY] = tonumber(_G[SAVED_MAP_KEY]) or 0
_G[SAVED_SCALE_KEY] = _G[SAVED_SCALE_KEY] or "party"
_G[SAVED_LIMIT_KEY] = tonumber(_G[SAVED_LIMIT_KEY]) or 10
_G[SAVED_ROGUE_SORT_KEY] = _G[SAVED_ROGUE_SORT_KEY] or "tier"

local state = {
    bootstrap = nil,
    preview = nil,
    selectedMode = _G[SAVED_MODE_KEY],
    selectedDifficultyId = _G[SAVED_DIFF_KEY],
    selectedThemeId = _G[SAVED_THEME_KEY],
    selectedMapId = _G[SAVED_MAP_KEY],
    selectedScale = _G[SAVED_SCALE_KEY],
    boardLimit = _G[SAVED_LIMIT_KEY],
    roguelikeSort = _G[SAVED_ROGUE_SORT_KEY],
    normalBoardRows = {},
    roguelikeBoardRows = {},
    currentView = "overview",
}

local function fmtTime(sec)
    sec = math.floor(tonumber(sec) or 0)
    local h = math.floor(sec / 3600)
    local m = math.floor((sec % 3600) / 60)
    local s = sec % 60
    if h > 0 then
        return string.format("%uh %02um %02us", h, m, s)
    end
    return string.format("%um %02us", m, s)
end

local function safe_len(tbl)
    if type(tbl) ~= "table" then
        return 0
    end
    local count = 0
    for _, value in pairs(tbl) do
        if value ~= nil then
            count = count + 1
        end
    end
    return count
end

local function normalizeRows(rows, sortKey)
    if type(rows) ~= "table" then
        return {}
    end

    local out = {}
    for key, value in pairs(rows) do
        if type(value) == "table" then
            local numericKey = tonumber(key)
            out[#out + 1] = {
                __numericKey = numericKey,
                __value = value,
            }
        end
    end

    table.sort(out, function(a, b)
        local av = a.__value
        local bv = b.__value

        if sortKey then
            local aSort = tonumber(av[sortKey])
            local bSort = tonumber(bv[sortKey])
            if aSort and bSort and aSort ~= bSort then
                return aSort < bSort
            end
        end

        if a.__numericKey and b.__numericKey and a.__numericKey ~= b.__numericKey then
            return a.__numericKey < b.__numericKey
        end

        return tostring(av.name or av.charName or "") < tostring(bv.name or bv.charName or "")
    end)

    local normalized = {}
    for i = 1, #out do
        normalized[i] = out[i].__value
    end

    return normalized
end

local function findById(rows, id, key)
    rows = normalizeRows(rows, key)
    rows = rows or {}
    key = key or "id"
    id = tonumber(id) or 0
    for i = 1, #rows do
        local row = rows[i]
        if row and tonumber(row[key]) == id then
            return rows[i], i
        end
    end
    return nil, nil
end

local function dungeonChoices()
    local choices = {
        { mapId = 0, name = "Weighted Random Dungeon", expansion = "Mixed", minLevel = 0, maxLevel = 0 }
    }

    local bootstrap = state.bootstrap
    if bootstrap and bootstrap.dungeons then
        local dungeons = normalizeRows(bootstrap.dungeons, "mapId")
        for i = 1, #dungeons do
            choices[#choices + 1] = dungeons[i]
        end
    end
    return choices
end

local function cycleChoice(rows, currentId, key, dir, defaultId)
    key = key or "id"
    rows = normalizeRows(rows, key)
    if #rows == 0 then
        return defaultId or 0
    end

    local _, index = findById(rows, currentId, key)
    if not index then
        index = 1
    else
        index = index + dir
    end

    if index < 1 then
        index = #rows
    elseif index > #rows then
        index = 1
    end

    return tonumber(rows[index][key]) or defaultId or 0
end

local frame = CreateFrame("Frame", "DungeonMasterAIOFrame", UIParent)
frame:SetFrameStrata("DIALOG")
frame:SetClampedToScreen(true)
frame:SetSize(660, 510)
frame:SetPoint("CENTER")
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true,
    tileSize = 32,
    edgeSize = 32,
    insets = { left = 10, right = 10, top = 10, bottom = 10 },
})
frame:SetBackdropColor(0.04, 0.04, 0.06, 0.96)
frame:SetMovable(true)
frame:EnableMouse(true)
frame:Hide()

tinsert(UISpecialFrames, frame:GetName())

local titleBar = CreateFrame("Button", nil, frame)
titleBar:SetHeight(28)
titleBar:SetPoint("TOPLEFT", 12, -10)
titleBar:SetPoint("TOPRIGHT", -12, -10)
titleBar:RegisterForDrag("LeftButton")
titleBar:SetScript("OnDragStart", function() frame:StartMoving() end)
titleBar:SetScript("OnDragStop", function() frame:StopMovingOrSizing() end)

local titleBg = titleBar:CreateTexture(nil, "BACKGROUND")
titleBg:SetAllPoints()
titleBg:SetTexture("Interface\\Buttons\\WHITE8X8")
titleBg:SetVertexColor(0.19, 0.14, 0.08, 0.95)

local title = titleBar:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("LEFT", 8, 0)
title:SetText("Dungeon Master")

local closeBtn = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
closeBtn:SetPoint("TOPRIGHT", -6, -6)
closeBtn:SetScript("OnClick", function() frame:Hide() end)

local tabOverview = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
tabOverview:SetSize(120, 24)
tabOverview:SetPoint("TOPLEFT", frame, "TOPLEFT", 18, -46)
tabOverview:SetText("Overview")

local tabNormal = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
tabNormal:SetSize(120, 24)
tabNormal:SetPoint("LEFT", tabOverview, "RIGHT", 6, 0)
tabNormal:SetText("Normal Board")

local tabRogue = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
tabRogue:SetSize(132, 24)
tabRogue:SetPoint("LEFT", tabNormal, "RIGHT", 6, 0)
tabRogue:SetText("Roguelike Board")

local tabHelp = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
tabHelp:SetSize(90, 24)
tabHelp:SetPoint("LEFT", tabRogue, "RIGHT", 6, 0)
tabHelp:SetText("Help")

local refreshBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
refreshBtn:SetSize(82, 24)
refreshBtn:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -18, -46)
refreshBtn:SetText("Refresh")

local launchBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
launchBtn:SetSize(140, 24)
launchBtn:SetPoint("RIGHT", refreshBtn, "LEFT", -8, 0)
launchBtn:SetText("Start Near NPC")

local controlsHeader = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
controlsHeader:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -82)
controlsHeader:SetText("Planner")

local modeLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
modeLabel:SetPoint("TOPLEFT", controlsHeader, "BOTTOMLEFT", 0, -10)
modeLabel:SetText("Mode")

local modeBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
modeBtn:SetSize(174, 22)
modeBtn:SetPoint("LEFT", modeLabel, "RIGHT", 10, 0)

local diffPrev = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
diffPrev:SetSize(24, 20)
diffPrev:SetPoint("TOPLEFT", modeLabel, "BOTTOMLEFT", 0, -10)
diffPrev:SetText("<")

local diffBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
diffBtn:SetSize(180, 20)
diffBtn:SetPoint("LEFT", diffPrev, "RIGHT", 4, 0)

local diffNext = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
diffNext:SetSize(24, 20)
diffNext:SetPoint("LEFT", diffBtn, "RIGHT", 4, 0)
diffNext:SetText(">")

local themePrev = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
themePrev:SetSize(24, 20)
themePrev:SetPoint("TOPLEFT", diffPrev, "BOTTOMLEFT", 0, -8)
themePrev:SetText("<")

local themeBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
themeBtn:SetSize(180, 20)
themeBtn:SetPoint("LEFT", themePrev, "RIGHT", 4, 0)

local themeNext = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
themeNext:SetSize(24, 20)
themeNext:SetPoint("LEFT", themeBtn, "RIGHT", 4, 0)
themeNext:SetText(">")

local mapPrev = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
mapPrev:SetSize(24, 20)
mapPrev:SetPoint("TOPLEFT", themePrev, "BOTTOMLEFT", 0, -8)
mapPrev:SetText("<")

local mapBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
mapBtn:SetSize(180, 20)
mapBtn:SetPoint("LEFT", mapPrev, "RIGHT", 4, 0)

local mapNext = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
mapNext:SetSize(24, 20)
mapNext:SetPoint("LEFT", mapBtn, "RIGHT", 4, 0)
mapNext:SetText(">")

local scaleBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
scaleBtn:SetSize(232, 22)
scaleBtn:SetPoint("TOPLEFT", mapPrev, "BOTTOMLEFT", 0, -12)

local boardFilterBtn = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
boardFilterBtn:SetSize(142, 22)
boardFilterBtn:SetPoint("LEFT", scaleBtn, "RIGHT", 10, 0)

local npcHint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
npcHint:SetPoint("TOPLEFT", scaleBtn, "BOTTOMLEFT", 0, -8)
npcHint:SetWidth(300)
npcHint:SetJustifyH("LEFT")
npcHint:SetText("Final launch still uses the Dungeon Master NPC. This AIO layer is your planning, stats, and leaderboard cockpit.")

local scroll = CreateFrame("ScrollFrame", "DungeonMasterAIOFrameScroll", frame, "UIPanelScrollFrameTemplate")
scroll:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -226)
scroll:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -36, 42)

local content = CreateFrame("Frame", nil, scroll)
content:SetSize(580, 1)
scroll:SetScrollChild(content)

local body = content:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
body:SetPoint("TOPLEFT", 4, -4)
body:SetWidth(580)
body:SetJustifyH("LEFT")
body:SetJustifyV("TOP")
body:SetNonSpaceWrap(false)

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 22, 18)
status:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -18, 18)
status:SetJustifyH("LEFT")
status:SetText("Requesting Dungeon Master data…")

local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
hint:SetPoint("TOP", frame, "BOTTOM", 0, -2)
hint:SetWidth(520)
hint:SetJustifyH("CENTER")
hint:SetText("|cff00ccff/dmui|r |cff666666or|r |cff00ccff/dungeonmaster|r  |cff888888·|r  ESC closes")

local function setBody(text)
    body:SetText(text or "")
    local h = math.max(body:GetStringHeight() + 18, 80)
    content:SetHeight(h)
    scroll:SetVerticalScroll(0)
    scroll:UpdateScrollChildRect()
end

local function setTabVisual(active)
    local tabs = {
        overview = tabOverview,
        normal = tabNormal,
        rogue = tabRogue,
        help = tabHelp,
    }

    for key, btn in pairs(tabs) do
        local fs = btn:GetFontString()
        if fs then
            if key == active then
                fs:SetTextColor(1.0, 0.85, 0.25)
            else
                fs:SetTextColor(0.68, 0.68, 0.68)
            end
        end
    end
end

local function currentDifficulty()
    return findById(state.bootstrap and state.bootstrap.difficulties or {}, state.selectedDifficultyId, "id")
end

local function currentTheme()
    return findById(state.bootstrap and state.bootstrap.themes or {}, state.selectedThemeId, "id")
end

local function currentDungeon()
    return findById(dungeonChoices(), state.selectedMapId, "mapId")
end

local function updateControls()
    local diff = currentDifficulty()
    local theme = currentTheme()
    local dungeon = currentDungeon()

    modeBtn:SetText(state.selectedMode == "roguelike" and "Roguelike" or "Standard Challenge")
    diffBtn:SetText(diff and diff.name or "Difficulty")
    themeBtn:SetText(theme and theme.name or "Theme")
    mapBtn:SetText(dungeon and dungeon.name or "Dungeon")
    scaleBtn:SetText(state.selectedScale == "party" and "Scaling: Party Level" or "Scaling: Dungeon Difficulty")
    boardFilterBtn:SetText(state.roguelikeSort == "floors" and "Rogue sort: Most Floors" or "Rogue sort: Highest Tier")
    launchBtn:SetText(state.selectedMode == "roguelike" and "Start Rogue Near NPC" or "Start Near NPC")

    _G[SAVED_MODE_KEY] = state.selectedMode
    _G[SAVED_DIFF_KEY] = state.selectedDifficultyId
    _G[SAVED_THEME_KEY] = state.selectedThemeId
    _G[SAVED_MAP_KEY] = state.selectedMapId
    _G[SAVED_SCALE_KEY] = state.selectedScale
    _G[SAVED_LIMIT_KEY] = state.boardLimit
    _G[SAVED_ROGUE_SORT_KEY] = state.roguelikeSort
end

local function buildLaunchCommand()
    if state.selectedMode == "roguelike" then
        return string.format(".dm roguelike %u %u %s",
            tonumber(state.selectedDifficultyId) or 1,
            tonumber(state.selectedThemeId) or 9,
            state.selectedScale == "party" and "party" or "tier")
    end

    return string.format(".dm start %u %u %u %s",
        tonumber(state.selectedDifficultyId) or 1,
        tonumber(state.selectedThemeId) or 9,
        tonumber(state.selectedMapId) or 0,
        state.selectedScale == "party" and "party" or "tier")
end

local function requestPreview()
    if not state.bootstrap then
        return
    end
    status:SetText("Refreshing preview…")
    AIO.Handle(CHANNEL, "ReqPreview", state.selectedMode, state.selectedDifficultyId, state.selectedThemeId, state.selectedMapId, state.selectedScale)
end

local function requestBootstrap()
    status:SetText("Loading Dungeon Master data…")
    AIO.Handle(CHANNEL, "ReqBootstrap")
end

local function requestNormalBoard()
    status:SetText("Loading normal leaderboard…")
    AIO.Handle(CHANNEL, "ReqNormalBoard", state.boardLimit, state.selectedMapId, state.selectedDifficultyId)
end

local function requestRoguelikeBoard()
    status:SetText("Loading roguelike leaderboard…")
    AIO.Handle(CHANNEL, "ReqRoguelikeBoard", state.boardLimit, state.roguelikeSort == "floors")
end

local function renderOverview()
    setTabVisual("overview")
    state.currentView = "overview"

    if not state.bootstrap then
        setBody("|cffaaaaaaWaiting for Dungeon Master bootstrap data…|r")
        return
    end

    if not state.preview then
        setBody("|cffaaaaaaRequesting preview…|r")
        requestPreview()
        return
    end

    local p = state.preview
    local lines = {}
    lines[#lines + 1] = "|cffccaa77Dungeon Master Planner|r"
    lines[#lines + 1] = "|cff444444--------------------------------------------------------------|r"
    lines[#lines + 1] = string.format("|cffffffffMode|r: %s", p.modeLabel or "?")
    lines[#lines + 1] = string.format("|cffffffffDifficulty|r: %s |cff888888(Lv %u-%u)|r", p.difficultyName or "?", p.difficultyMinLevel or 1, p.difficultyMaxLevel or 80)
    lines[#lines + 1] = string.format("|cffffffffTheme|r: %s", p.themeName or "?")
    lines[#lines + 1] = string.format("|cffffffffDungeon|r: %s", p.dungeonName or "?")
    lines[#lines + 1] = string.format("|cffffffffScaling|r: %s", p.scaleLabel or "?")
    lines[#lines + 1] = string.format("|cffffffffPlayer level|r: %u  |cff666666·|r  |cffffffffParty size|r: %u", p.playerLevel or 1, p.partySize or 1)
    lines[#lines + 1] = " "
    lines[#lines + 1] = string.format("|cffffffffEnemy tuning|r: HP x%.2f  |cff666666·|r  Damage x%.2f  |cff666666·|r  Mob density x%.2f", p.healthMult or 1.0, p.damageMult or 1.0, p.mobMult or 1.0)
    lines[#lines + 1] = string.format("|cffffffffReward tuning|r: base completion gold ~ |cffffd700%u|r copper  |cff666666·|r  reward x%.2f", p.expectedBaseGold or 0, p.rewardMult or 1.0)
    lines[#lines + 1] = string.format("|cffffffffItem rolls|r: item %u%%  |cff666666·|r  rare %u%%  |cff666666·|r  epic %u%%", p.itemChance or 0, p.rareChance or 0, p.epicChance or 0)

    if safe_len(p.warnings) > 0 then
        lines[#lines + 1] = " "
        lines[#lines + 1] = "|cffff6666Warnings|r"
        for i = 1, #p.warnings do
            lines[#lines + 1] = string.format("  • %s", tostring(p.warnings[i]))
        end
    end

    if safe_len(p.notes) > 0 then
        lines[#lines + 1] = " "
        lines[#lines + 1] = "|cff88ccffNotes|r"
        for i = 1, #p.notes do
            lines[#lines + 1] = string.format("  • %s", tostring(p.notes[i]))
        end
    end

    lines[#lines + 1] = " "
    lines[#lines + 1] = "|cffaaaaaaLaunch path today: use this panel to plan, then talk to the Dungeon Master NPC to actually begin the run.|r"
    lines[#lines + 1] = "|cffaaaaaaThe Start button sends a validated `.dm` launch command, but it only works while you are standing near a Dungeon Master NPC.|r"
    lines[#lines + 1] = "|cffaaaaaaThe final server-side checks still happen there: cooldowns, active sessions, Challenge Modes, and owner-led Playerbot gating.|r"
    lines[#lines + 1] = string.format("|cff88ccffLaunch command|r: |cffffffff%s|r", buildLaunchCommand())

    setBody(table.concat(lines, "\n"))
    status:SetText("Planner ready.")
end

local function renderNormalBoard()
    setTabVisual("normal")
    state.currentView = "normal"

    local rows = state.normalBoardRows or {}
    local diff = currentDifficulty()
    local dungeon = currentDungeon()
    local lines = {}

    lines[#lines + 1] = "|cffccaa77Dungeon Master — Fastest Clears|r"
    lines[#lines + 1] = string.format("|cffaaaaaaFilter|r: %s / %s / top %u", dungeon and dungeon.name or "All maps", diff and diff.name or "All difficulties", state.boardLimit)
    lines[#lines + 1] = "|cff444444---------------------------------------------------------------------|r"
    lines[#lines + 1] = "|cffccaa77#|r  |cffccaa77Player|r        |cffccaa77Time|r      |cffccaa77Map|r                 |cffccaa77P|r |cffccaa77L|r |cffccaa77D|r"
    lines[#lines + 1] = "|cff444444---------------------------------------------------------------------|r"

    if #rows == 0 then
        lines[#lines + 1] = "|cffffcc66No normal-run leaderboard entries matched the current filter.|r"
    else
        local dungeons = state.bootstrap and state.bootstrap.dungeons or {}
        local diffs = state.bootstrap and state.bootstrap.difficulties or {}
        for i = 1, #rows do
            local row = rows[i]
            local map = findById(dungeons, row.mapId, "mapId")
            local rowDiff = findById(diffs, row.difficultyId, "id")
            local isSelf = tostring(row.charName) == PLAYER_NAME
            lines[#lines + 1] = string.format(
                "%s%2u.|r %-12s  %-9s %-20s %u %2u %u %s",
                isSelf and "|cff55ff55" or "|cffffffff",
                i,
                tostring(row.charName or "?"),
                fmtTime(row.clearTime),
                tostring(map and map.name or row.mapId or "?"),
                tonumber(row.partySize) or 0,
                tonumber(row.effectiveLevel) or 0,
                tonumber(row.deaths) or 0,
                rowDiff and ("|cff888888(" .. rowDiff.name .. ")|r") or ""
            )
        end
    end

    setBody(table.concat(lines, "\n"))
    status:SetText("Normal leaderboard ready.")
end

local function renderRoguelikeBoard()
    setTabVisual("rogue")
    state.currentView = "rogue"

    local rows = state.roguelikeBoardRows or {}
    local lines = {}
    lines[#lines + 1] = state.roguelikeSort == "floors" and "|cffccaa77Dungeon Master — Roguelike (Most Floors)|r" or "|cffccaa77Dungeon Master — Roguelike (Highest Tier)|r"
    lines[#lines + 1] = string.format("|cffaaaaaaTop %u entries|r", state.boardLimit)
    lines[#lines + 1] = "|cff444444---------------------------------------------------------------------|r"
    lines[#lines + 1] = "|cffccaa77#|r  |cffccaa77Player|r        |cffccaa77Tier|r |cffccaa77Floors|r |cffccaa77Kills|r |cffccaa77Deaths|r |cffccaa77Time|r"
    lines[#lines + 1] = "|cff444444---------------------------------------------------------------------|r"

    if #rows == 0 then
        lines[#lines + 1] = "|cffffcc66No roguelike leaderboard entries exist yet.|r"
    else
        for i = 1, #rows do
            local row = rows[i]
            local isSelf = tostring(row.charName) == PLAYER_NAME
            lines[#lines + 1] = string.format(
                "%s%2u.|r %-12s %4u %6u %6u %6u %-10s",
                isSelf and "|cff55ff55" or "|cffffffff",
                i,
                tostring(row.charName or "?"),
                tonumber(row.tierReached) or 0,
                tonumber(row.dungeonsCleared) or 0,
                tonumber(row.totalKills) or 0,
                tonumber(row.totalDeaths) or 0,
                fmtTime(row.runDuration)
            )
        end
    end

    setBody(table.concat(lines, "\n"))
    status:SetText("Roguelike leaderboard ready.")
end

local function renderHelp()
    setTabVisual("help")
    state.currentView = "help"

    local bootstrap = state.bootstrap
    local lines = {}
    lines[#lines + 1] = "|cffccaa77Dungeon Master AIO Help|r"
    lines[#lines + 1] = "|cff444444--------------------------------------------------------------|r"
    lines[#lines + 1] = "|cffffffffWhat this UI does today|r"
    lines[#lines + 1] = "  • Preview difficulty, scaling, theme, and dungeon choices"
    lines[#lines + 1] = "  • Show your normal and roguelike stat summaries"
    lines[#lines + 1] = "  • Browse fastest-clear and roguelike leaderboards"
    lines[#lines + 1] = " "
    lines[#lines + 1] = "|cffffffffWhat still happens through the NPC|r"
    lines[#lines + 1] = "  • Starting the actual run"
    lines[#lines + 1] = "  • Final validation for cooldowns, Challenge Modes, active sessions, and owner-led Playerbot rules"
    lines[#lines + 1] = " "
    lines[#lines + 1] = "|cffffffffControls|r"
    lines[#lines + 1] = "  • /dmui or /dungeonmaster — open this window"
    lines[#lines + 1] = "  • Refresh — reload bootstrap data and rerender the active tab"
    lines[#lines + 1] = "  • Rogue sort button — switch between Highest Tier and Most Floors"
    lines[#lines + 1] = "  • Start Near NPC — sends a validated `.dm` launch command while you are standing by a Dungeon Master NPC"
    lines[#lines + 1] = " "
    if bootstrap then
        local ns = bootstrap.normalStats or {}
        local rs = bootstrap.roguelikeStats or {}
        lines[#lines + 1] = "|cffffffffYour snapshot|r"
        lines[#lines + 1] = string.format("  • Normal runs: %u total / %u completed / %u failed", ns.totalRuns or 0, ns.completedRuns or 0, ns.failedRuns or 0)
        lines[#lines + 1] = string.format("  • Roguelike runs: %u total / highest tier %u / most floors %u", rs.totalRuns or 0, rs.highestTier or 0, rs.mostFloorsCleared or 0)
    end

    setBody(table.concat(lines, "\n"))
    status:SetText("Help ready.")
end

local function refreshActiveView(forceBootstrap)
    updateControls()
    if forceBootstrap then
        requestBootstrap()
        return
    end

    if state.currentView == "overview" then
        requestPreview()
    elseif state.currentView == "normal" then
        requestNormalBoard()
    elseif state.currentView == "rogue" then
        requestRoguelikeBoard()
    else
        renderHelp()
    end
end

modeBtn:SetScript("OnClick", function()
    state.selectedMode = state.selectedMode == "roguelike" and "normal" or "roguelike"
    updateControls()
    requestPreview()
end)

diffPrev:SetScript("OnClick", function()
    state.selectedDifficultyId = cycleChoice(state.bootstrap and state.bootstrap.difficulties or {}, state.selectedDifficultyId, "id", -1, 1)
    updateControls()
    requestPreview()
end)

diffNext:SetScript("OnClick", function()
    state.selectedDifficultyId = cycleChoice(state.bootstrap and state.bootstrap.difficulties or {}, state.selectedDifficultyId, "id", 1, 1)
    updateControls()
    requestPreview()
end)

themePrev:SetScript("OnClick", function()
    state.selectedThemeId = cycleChoice(state.bootstrap and state.bootstrap.themes or {}, state.selectedThemeId, "id", -1, 9)
    updateControls()
    requestPreview()
end)

themeNext:SetScript("OnClick", function()
    state.selectedThemeId = cycleChoice(state.bootstrap and state.bootstrap.themes or {}, state.selectedThemeId, "id", 1, 9)
    updateControls()
    requestPreview()
end)

mapPrev:SetScript("OnClick", function()
    state.selectedMapId = cycleChoice(dungeonChoices(), state.selectedMapId, "mapId", -1, 0)
    updateControls()
    requestPreview()
end)

mapNext:SetScript("OnClick", function()
    state.selectedMapId = cycleChoice(dungeonChoices(), state.selectedMapId, "mapId", 1, 0)
    updateControls()
    requestPreview()
end)

scaleBtn:SetScript("OnClick", function()
    state.selectedScale = state.selectedScale == "party" and "tier" or "party"
    updateControls()
    requestPreview()
end)

boardFilterBtn:SetScript("OnClick", function()
    state.roguelikeSort = state.roguelikeSort == "tier" and "floors" or "tier"
    updateControls()
    if state.currentView == "rogue" then
        requestRoguelikeBoard()
    else
        renderOverview()
    end
end)

refreshBtn:SetScript("OnClick", function()
    refreshActiveView(true)
end)

launchBtn:SetScript("OnClick", function()
    local command = buildLaunchCommand()
    status:SetText("Prepared Dungeon Master launch command. Press Enter while near the NPC.")

    if ChatFrame_OpenChat then
        ChatFrame_OpenChat(command)
    elseif DEFAULT_CHAT_FRAME and DEFAULT_CHAT_FRAME.editBox then
        DEFAULT_CHAT_FRAME.editBox:SetText(command)
        DEFAULT_CHAT_FRAME.editBox:Show()
        DEFAULT_CHAT_FRAME.editBox:SetFocus()
    end
end)

tabOverview:SetScript("OnClick", function()
    state.currentView = "overview"
    renderOverview()
end)

tabNormal:SetScript("OnClick", function()
    state.currentView = "normal"
    requestNormalBoard()
end)

tabRogue:SetScript("OnClick", function()
    state.currentView = "rogue"
    requestRoguelikeBoard()
end)

tabHelp:SetScript("OnClick", function()
    state.currentView = "help"
    renderHelp()
end)

AIO.AddHandlers(CHANNEL, {
    PushBootstrap = function(payload)
        state.bootstrap = payload or {}
        state.bootstrap.difficulties = normalizeRows(state.bootstrap.difficulties, "id")
        state.bootstrap.themes = normalizeRows(state.bootstrap.themes, "id")
        state.bootstrap.dungeons = normalizeRows(state.bootstrap.dungeons, "mapId")
        if state.selectedDifficultyId == 0 and safe_len(state.bootstrap.difficulties) > 0 then
            state.selectedDifficultyId = tonumber(state.bootstrap.difficulties[1].id) or 1
        end
        if state.selectedThemeId == 0 and safe_len(state.bootstrap.themes) > 0 then
            state.selectedThemeId = tonumber(state.bootstrap.themes[1].id) or 1
        end
        updateControls()
        title:SetText("Dungeon Master — " .. tostring(payload and payload.playerName or PLAYER_NAME))

        if state.currentView == "normal" then
            requestNormalBoard()
        elseif state.currentView == "rogue" then
            requestRoguelikeBoard()
        elseif state.currentView == "help" then
            renderHelp()
        else
            requestPreview()
        end
    end,

    PushPreview = function(preview)
        state.preview = preview or {}
        renderOverview()
    end,

    PushNormalBoard = function(rows)
        state.normalBoardRows = normalizeRows(rows, nil)
        renderNormalBoard()
    end,

    PushRoguelikeBoard = function(rows)
        state.roguelikeBoardRows = normalizeRows(rows, nil)
        renderRoguelikeBoard()
    end,
})

if AIO.SavePosition then
    AIO.SavePosition(frame, true)
end

SLASH_DUNGEONMASTERAIO1 = "/dmui"
SLASH_DUNGEONMASTERAIO2 = "/dungeonmaster"
SLASH_DUNGEONMASTERAIO3 = "/dmplanner"
SlashCmdList["DUNGEONMASTERAIO"] = function()
    if frame:IsShown() then
        frame:Hide()
        return
    end

    frame:Show()
    setTabVisual(state.currentView)
    updateControls()
    requestBootstrap()
end

print("|cff00ccff[Dungeon Master]|r AIO planner loaded — |cffffffff/dmui|r, |cffffffff/dungeonmaster|r  |cff888888(launch still uses the NPC)|r")

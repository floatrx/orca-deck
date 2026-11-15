--- Demo mode (2026-11-15, for the knob tour video): while ~/.config/orca-deck/demo-mode exists, /api/* serves
--- invented workspaces, agents, PRs and a day, so no real project name or prompt reaches a recording. Commands
--- touch nothing: no Orca pane switches, no prompt is sent, no Mac volume or mic changes. They answer ok and move
--- the demo's own state (a send shows as sent, the mic and volume change only here).
---   on:  touch ~/.config/orca-deck/demo-mode     off:  rm ~/.config/orca-deck/demo-mode
local demo = {}

local FLAG = os.getenv("HOME") .. "/.config/orca-deck/demo-mode"

function demo.active()
  local file = io.open(FLAG, "r")
  if not file then return false end
  file:close()
  return true
end

local function now() return math.floor(hs.timer.secondsSinceEpoch() * 1000) end

-- One table per worktree: repo, name (the branch, as Orca names a worktree), and its agents.
-- bucket: working · attention (needs you) · done · idle. ago = seconds since the agent's state began.
local WORKTREES = {
  { repo = "orca-deck", name = "main", agents = {
    { id = "demo-orca", bucket = "working", state = "working", tool = "Edit", ago = 95,
      prompt = "Add a Wi-Fi fallback to the relay",
      last = "Wiring the Wi-Fi fallback into the relay: BLE first, then the LAN.",
      message = "Wiring the Wi-Fi fallback into the relay. BLE stays the first route; when the knob walks out of range it reconnects over the LAN in about two seconds.",
      next = {} } } },
  { repo = "api-server", name = "feat/rate-limits", agents = {
    { id = "demo-api", bucket = "attention", state = "waiting", ago = 40,
      prompt = "Add per-key rate limits to the public API",
      last = "Two choices before I write the limiter.",
      message = "The limiter is sketched. Two choices before I write it.",
      question = {
        itemId = "demo-q", revision = 1, header = "Limits", text = "Where should the counters live?",
        options = { { id = "redis", label = "Redis" }, { id = "memory", label = "In memory" } }, answerable = true,
        groupAnswerable = true,
        questions = {
          { id = "store", header = "Store", text = "Where should the counters live?", multiSelect = false,
            options = { { id = "redis", label = "Redis (shared)" }, { id = "memory", label = "In memory" } } },
          { id = "scope", header = "Scope", text = "Which routes get a limit?", multiSelect = true,
            options = { { id = "auth", label = "/auth" }, { id = "search", label = "/search" }, { id = "upload", label = "/upload" } } },
        } } } } },
  { repo = "web-dashboard", name = "fix/chart-tooltips", agents = {
    { id = "demo-web", bucket = "done", state = "done", ago = 300,
      prompt = "Fix the chart tooltips on mobile",
      last = "Tooltips fixed: they flip above the finger and never leave the screen.",
      message = "Tooltips fixed. On touch they open above the finger and flip at the screen edge. 14 tests pass.",
      next = { "Run the e2e suite on mobile", "Commit and open a PR", "Add a screenshot test for the tooltip" } } } },
  { repo = "mobile-app", name = "dev", agents = {
    { id = "demo-mobile", bucket = "working", state = "working", tool = "Bash", ago = 210,
      prompt = "Upgrade to the new navigation API",
      last = "Running the iOS build after the navigation upgrade.",
      message = "Upgrading the navigation API. Twelve screens are done; the iOS build is running now.",
      next = {} } } },
  { repo = "docs-site", name = "main", agents = {
    { id = "demo-docs", bucket = "done", state = "done", ago = 1500,
      prompt = "Write the getting-started guide",
      last = "The getting-started guide is written: install, pair, first prompt.",
      message = "The getting-started guide is written: install, pair the knob, send your first prompt.",
      next = { "Proofread it and fix the links", "Add screenshots of the pairing" } } } },
  { repo = "infra", name = "chore/terraform", agents = {} },
}

local PRS = {
  { key = "acme/api-server#418", repo = "acme/api-server", number = 418, title = "feat: per-key rate limits", checks = "run", mine = true, draft = false, review = "", rerun = false },
  { key = "acme/web-dashboard#203", repo = "acme/web-dashboard", number = 203, title = "fix: chart tooltips on touch", checks = "ok", mine = true, draft = false, review = "approved", rerun = false },
  { key = "acme/mobile-app#97", repo = "acme/mobile-app", number = 97, title = "chore: new navigation API", checks = "fail", mine = true, draft = false, review = "", rerun = true },
  { key = "acme/docs-site#56", repo = "acme/docs-site", number = 56, title = "docs: getting started", checks = "ok", mine = false, draft = false, review = "", rerun = false },
}

demo.sends, demo.answers = {}, {}
demo.mac = { micMuted = false, muted = false, volume = 45, micVolume = 60 }

local function worktreeId(w) return "demo-" .. w.repo .. "::/demo/" .. w.repo .. "/" .. w.name end

local function agentOut(a, at)
  return { id = a.id, kind = "claude", chat = true, bucket = a.bucket, state = a.state, tool = a.tool,
    since = at - a.ago * 1000, last = a.last, prompt = a.prompt, interrupted = false }
end

function demo.worktrees()
  local at, out = now(), {}
  for i, w in ipairs(WORKTREES) do
    local agents = {}
    for _, a in ipairs(w.agents) do agents[#agents + 1] = agentOut(a, at) end
    local status = #agents > 0 and agents[1].bucket or "idle"
    out[#out + 1] = { id = worktreeId(w), repo = w.repo, name = w.name, branch = w.name, active = i == 1,
      status = status, agents = agents, lead = agents[1] }
  end
  return out
end

local function find(id)
  for _, w in ipairs(WORKTREES) do
    for _, a in ipairs(w.agents) do
      if a.id == id then return a, w end
    end
  end
end

-- The day: minutes since midnight, spans [from, to, "w" worked | "a" waited on you], up to the current minute.
local function day()
  local t = os.date("*t")
  local minute = t.hour * 60 + t.min
  local function clip(spans)
    local out = {}
    for _, s in ipairs(spans) do
      if s[1] <= minute then out[#out + 1] = { s[1], math.min(s[2], minute), s[3] } end
    end
    return out
  end
  local rows = {
    { name = "orca-deck", branch = "main", spans = clip({ { 540, 610, "w" }, { 611, 618, "a" }, { 619, 700, "w" }, { 780, 860, "w" }, { 900, 990, "w" }, { minute - 20, minute, "w" } }) },
    { name = "api-server", branch = "feat/rate-limits", spans = clip({ { 560, 640, "w" }, { 641, 660, "a" }, { 840, 930, "w" }, { minute - 3, minute, "a" } }) },
    { name = "web-dashboard", branch = "fix/chart-tooltips", spans = clip({ { 600, 655, "w" }, { 700, 745, "w" }, { 746, 752, "a" }, { 990, 1040, "w" } }) },
    { name = "mobile-app", branch = "dev", spans = clip({ { 620, 700, "w" }, { 960, 1010, "w" }, { minute - 40, minute, "w" } }) },
    { name = "docs-site", branch = "main", spans = clip({ { 680, 730, "w" }, { 880, 905, "w" } }) },
  }
  return { v = 1, date = os.date("%Y-%m-%d"), now = minute, worked = 312, waited = 27, rows = rows }
end

--- /api/* in demo mode: a body to answer with, or nil to fall through (ping, prompts, log stay real).
--- `realMac` is the bridge's macStatus(), for the parts that name nothing (CPU, battery, Claude limits).
function demo.api(method, route, query, realMac)
  if method ~= "GET" then return nil end
  if route == "/api/state" then
    return 200, { v = 1, seq = 1, worktrees = demo.worktrees(), events = {} }
  elseif route == "/api/day" then
    return 200, day()
  elseif route == "/api/prs" then
    return 200, { v = 1, at = now(), prs = PRS, loaded = true }
  elseif route == "/api/mac" then
    local mac = realMac or {}
    for k, v in pairs(demo.mac) do mac[k] = v end
    mac.output, mac.mic = "Speakers", "Microphone"
    return 200, mac
  elseif route == "/api/agent" then
    local a, w = find(query.id)
    if not a then return 404, { error = "unknown agent" } end
    local answered = demo.answers[a.id]
    return 200, { v = 1, id = a.id, worktreeId = worktreeId(w), worktree = w.name, kind = "claude", chat = true,
      state = a.state, bucket = a.bucket, since = now() - a.ago * 1000, message = a.message, full = true,
      next = a.question and {} or a.next, question = not answered and a.question or nil,
      send = demo.sends[a.id], answer = answered }
  end
  return nil
end

--- Every command in demo mode: an ok ack, and nothing outside this file moves.
function demo.cmd(message)
  local cmd, at = message.cmd, now()
  if cmd == "say" and message.id then
    demo.sends[message.id] = { at = at, ok = true }
  elseif cmd == "answer" and message.id then
    demo.answers[message.id] = { at = at, ok = true }
  elseif cmd == "mac" then
    local m, v = demo.mac, tonumber(message.value)
    if message.action == "inmute" then
      if message.value == nil then m.micMuted = not m.micMuted else m.micMuted = message.value == true end
    elseif message.action == "mute" then
      m.muted = not m.muted
    elseif message.action == "involume" and v then
      m.micVolume = math.max(0, math.min(100, m.micVolume + v))
    elseif message.action == "volume" and v then
      m.volume = math.max(0, math.min(100, m.volume + v))
    end
  end
  return { type = "ack", v = 1, cmd = tostring(cmd), ok = true, note = "demo" }
end

--- Back to a clean take: nothing sent, nothing answered, the mic live.
function demo.reset()
  demo.sends, demo.answers = {}, {}
  demo.mac = { micMuted = false, muted = false, volume = 45, micVolume = 60 }
end

return demo

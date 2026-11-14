--- orca-deck bridge core: the pure logic of protocol v1 (protocol.md) — buckets, the lead agent,
--- events, the event log, HTTP helpers and the ntfy message. No hs.* here, so bridge/test/ can test it.

local core = {}

core.PROTOCOL = 1

--- Orca's AGENT_STATUS_STATES (shared/agent-status-types.js:29) → protocol buckets.
local BUCKETS = { working = "working", done = "done", blocked = "attention", waiting = "attention" }

--- Higher wins the lead.
local URGENCY = { idle = 1, done = 2, working = 3, attention = 4 }

function core.bucket(state)
  return BUCKETS[state] or "idle"
end

--- Cuts text to `limit` characters on one line; non-strings become "".
function core.clip(text, limit)
  if type(text) ~= "string" then return "" end
  text = text:gsub("%s+", " "):match("^%s*(.-)%s*$")
  if utf8.len(text) and utf8.len(text) > limit then
    text = text:sub(1, utf8.offset(text, limit + 1) - 1) .. "…"
  end
  return text
end

--- The agent that most needs the user; ties keep Orca's order. nil when there are no agents.
local function leadOf(agents)
  local lead
  for _, agent in ipairs(agents) do
    if not lead or URGENCY[agent.bucket] > URGENCY[lead.bucket] then lead = agent end
  end
  return lead
end

--- `orca worktree ps --json` → protocol worktrees, in the CLI's order.
function core.worktreesOf(doc)
  local worktrees = {}
  for _, worktree in ipairs(((doc or {}).result or {}).worktrees or {}) do
    local agents = {}
    -- An agent without a paneKey has no id for clients to address; it is left out.
    for _, agent in ipairs(worktree.agents or {}) do
      if agent.paneKey then agents[#agents + 1] = {
        id = agent.paneKey,
        kind = agent.agentType,
        chat = (agent.paneKey or ""):find("^structured%-agent%-session") ~= nil,
        state = agent.state,
        bucket = core.bucket(agent.state),
        interrupted = agent.interrupted == true,
        tool = agent.toolName,
        prompt = core.clip(agent.prompt, 90),
        last = core.clip(agent.lastAssistantMessage, 140),
        since = agent.stateStartedAt,
      } end
    end
    worktrees[#worktrees + 1] = {
      id = worktree.worktreeId,
      name = worktree.displayName or worktree.repo or worktree.worktreeId,
      repo = worktree.repo,
      branch = (worktree.branch or ""):gsub("^refs/heads/", ""),
      active = worktree.isActive == true,
      status = worktree.status,
      lead = leadOf(agents),
      agents = agents,
    }
  end
  return worktrees
end

--- Compares each agent's bucket with `previous` (agentId → bucket from the last poll).
--- Returns the events (without seq/at — the event log adds them) and the new agentId → bucket map.
function core.transitions(previous, worktrees)
  local events, seen = {}, {}
  for _, worktree in ipairs(worktrees) do
    for _, agent in ipairs(worktree.agents) do
      local before, now = previous[agent.id], agent.bucket
      local kind
      if before and before ~= now then
        if now == "attention" then
          kind = "attention"
        elseif now == "done" and before == "working" and not agent.interrupted then
          kind = "done"
        end
      end
      if kind then
        events[#events + 1] = { kind = kind, worktreeId = worktree.id, worktree = worktree.name, agentId = agent.id }
      end
      seen[agent.id] = now
    end
  end
  return events, seen
end

--- The last `capacity` events. `seq` starts at `firstSeq` (the boot time in ms) and grows by 1.
function core.newEventLog(firstSeq, capacity)
  local log = { seq = firstSeq, events = {} }

  function log:add(event, at)
    self.seq = self.seq + 1
    event.type, event.v, event.seq, event.at = "event", core.PROTOCOL, self.seq, at
    self.events[#self.events + 1] = event
    if #self.events > capacity then table.remove(self.events, 1) end
    return event
  end

  --- Events with seq > since. No `since` → nothing: a fresh client replays no pulses.
  function log:after(since)
    local found = {}
    since = tonumber(since)
    if not since then return found end
    for _, event in ipairs(self.events) do
      if event.seq > since then found[#found + 1] = event end
    end
    return found
  end

  return log
end

local function unescape(text)
  return (text:gsub("+", " "):gsub("%%(%x%x)", function(hex) return string.char(tonumber(hex, 16)) end))
end

--- "/api/state?since=12" → "/api/state", { since = "12" }.
function core.parseTarget(target)
  local route, rest = (target or ""):match("^([^?]*)%??(.*)$")
  local query = {}
  for pair in rest:gmatch("[^&]+") do
    local key, value = pair:match("^([^=]*)=?(.*)$")
    query[unescape(key)] = unescape(value)
  end
  return route, query
end

--- True when `headers` carry `Authorization: Bearer <token>`. An empty token never matches.
function core.authorized(headers, token)
  if type(token) ~= "string" or token == "" then return false end
  for name, value in pairs(headers or {}) do
    if type(name) == "string" and name:lower() == "authorization" and type(value) == "string" then
      local scheme, given = value:match("^(%S+)%s+(%S+)$")
      return scheme ~= nil and scheme:lower() == "bearer" and given == token
    end
  end
  return false
end

--- The ntfy JSON publish body for `event`. Only the worktree name and the kind — never prompt text.
function core.ntfyMessage(event, repo, topic)
  local title = event.worktree
  if repo and repo ~= "" and repo ~= event.worktree then title = title .. " · " .. repo end
  local attention = event.kind == "attention"
  return {
    topic = topic,
    title = title,
    message = attention and "needs you" or "done",
    tags = { attention and "warning" or "white_check_mark" },
    priority = attention and 4 or 3,
  }
end

-- Agent screen (apex-v2) ------------------------------------------------------------------------

core.SAY_MAX = 2000
local VARIANTS_MAX = 5

--- "structured-agent-session-claude_82c20d91_…:<leaf>" → "claude_82c20d91_…". nil for terminal panes and
--- for anything that is not a plain id, because the id becomes a file name under next-pending/.
function core.sessionIdOf(agentId)
  if type(agentId) ~= "string" then return nil end
  local id = agentId:match("^structured%-agent%-session%-([^:]+):")
  if id and id:match("^[%w_%-]+$") then return id end
  return nil
end

--- The Next variants of a next-pending file: non-empty trimmed lines, in order, at most 5.
function core.parseVariants(text)
  local variants = {}
  for line in (text or ""):gmatch("[^\n]+") do
    local variant = line:match("^%s*(.-)%s*$")
    if variant ~= "" and #variants < VARIANTS_MAX then variants[#variants + 1] = variant end
  end
  return variants
end

--- Whether a sent text used the pending variants: it is one of them, or carries one (the watch sends several
--- ticked variants as a numbered list). Then the variants are spent, like after Orca's →.
function core.usesVariants(text, variants)
  if type(text) ~= "string" then return false end
  for _, variant in ipairs(variants or {}) do
    if text == variant or text:find(variant, 1, true) then return true end
  end
  return false
end

--- The message without its trailing run of "Next: `…`" lines (blank lines between them allowed) —
--- the same run the Stop hook (~/.claude/hooks/next-prefill.sh) turns into variants.
function core.stripNextLines(message)
  if type(message) ~= "string" then return "" end
  local lines = {}
  for line in (message .. "\n"):gmatch("(.-)\n") do lines[#lines + 1] = line end
  local last = #lines
  while last > 0 do
    local line = lines[last]
    if line:match("^%s*$") or line:match("^%s*Next:%s*`[^`]+`") then last = last - 1 else break end
  end
  return (table.concat(lines, "\n", 1, last):match("^%s*(.-)%s*$"))
end

--- Why a say command cannot run, or nil when it can.
function core.sayProblem(message)
  if type(message.id) ~= "string" or message.id == "" then return "say needs an agent id" end
  if type(message.text) ~= "string" or not message.text:match("%S") then return "say needs text" end
  if utf8.len(message.text) and utf8.len(message.text) > core.SAY_MAX then
    return "text is longer than " .. core.SAY_MAX .. " characters"
  end
  return nil
end

--- Why `cmd` may not run over the transport `via` ("ws" or "http <addr>"), or nil when it may.
--- `say` types into a Claude session that runs with bypassed permissions, and `answer` decides for it, so both
--- need the bearer token: any web page can open ws://localhost:7352/ws, and the WebSocket has no auth.
function core.commandRefusal(cmd, via)
  if (cmd == "say" or cmd == "answer" or cmd == "mac" or cmd == "jog" or cmd == "focus" or cmd == "pr" or cmd == "wifi_add") and via == "ws" then
    return cmd .. " needs the bearer token: use POST /api/cmd"
  end
  return nil
end

--- The knob's Mac remote (cmd "mac"): what `action` may be.
--- inmute / involume: the Mac's microphone (the knob's Mic Mute).
core.MAC_ACTIONS = { volume = true, mute = true, play = true, next = true, prev = true, inmute = true, involume = true }

--- Why a mac command cannot run, or nil when it can. `volume` / `involume` take `value`: a step (-100…100, percent
--- points); `inmute` takes an optional boolean `value` (set it; without one, toggle).
function core.macProblem(message)
  if not core.MAC_ACTIONS[message.action] then return "mac needs action volume|mute|play|next|prev|inmute|involume" end
  if message.action == "inmute" then
    if message.value ~= nil and type(message.value) ~= "boolean" then return "inmute takes a boolean value" end
    return nil
  end
  if message.action == "volume" or message.action == "involume" then
    local v = tonumber(message.value)
    if not v or v < -100 or v > 100 then return "volume needs a value step from -100 to 100" end
  end
  return nil
end

--- The knob's Jog Wheel (cmd "jog"): the crown as a precise wheel for the front app.
core.JOG_MODES = { scroll = true, undo = true, tab = true, zoom = true, frame = true }
core.JOG_MAX = 20  -- detents per command: the knob batches every 80 ms, a fast spin stays below this

--- Why a jog command cannot run, or nil. `value`: signed detents (+ = down / redo / next / in / forward);
--- `reset` (zoom only) instead of a value: back to 100 %.
function core.jogProblem(message)
  if not core.JOG_MODES[message.mode] then return "jog needs mode scroll|undo|tab|zoom|frame" end
  if message.reset then
    if message.mode ~= "zoom" then return "only zoom resets" end
    return nil
  end
  local v = tonumber(message.value)
  if not v or v == 0 or v ~= math.floor(v) or math.abs(v) > core.JOG_MAX then
    return "jog needs a whole value from -" .. core.JOG_MAX .. " to " .. core.JOG_MAX
  end
  return nil
end

--- Claude plan limits from GET api.anthropic.com/api/oauth/usage (the endpoint Orca's status bar uses):
--- { session = %, week = %, sessionResets = "iso", weekResets = "iso" }; nil when the answer has neither.
function core.claudeUsageOf(doc)
  if type(doc) ~= "table" then return nil end
  local function pct(window)
    local u = type(window) == "table" and tonumber(window.utilization) or nil
    return u and math.floor(u + 0.5) or nil
  end
  local out = { session = pct(doc.five_hour), week = pct(doc.seven_day) }
  if out.session == nil and out.week == nil then return nil end
  out.sessionResets = type(doc.five_hour) == "table" and doc.five_hour.resets_at or nil
  out.weekResets = type(doc.seven_day) == "table" and doc.seven_day.resets_at or nil
  -- The week's severity (normal / warning / critical) and the per-model weekly limits ("Fable 0 %").
  out.models = {}
  for _, limit in ipairs(type(doc.limits) == "table" and doc.limits or {}) do
    if type(limit) == "table" then
      if limit.kind == "weekly_all" then out.weekSeverity = limit.severity end
      local model = type(limit.scope) == "table" and type(limit.scope.model) == "table" and limit.scope.model.display_name
      if limit.kind == "weekly_scoped" and type(model) == "string" and tonumber(limit.percent) then
        out.models[#out.models + 1] = { name = model, percent = math.floor(tonumber(limit.percent) + 0.5) }
      end
    end
  end
  -- What the week went on: Claude Code / Cowork / Chats … (percent of the week's use), the non-zero ones.
  out.breakdown = {}
  local rows = type(doc.seven_day_breakdown) == "table" and doc.seven_day_breakdown.rows or {}
  for _, row in ipairs(type(rows) == "table" and rows or {}) do
    if type(row) == "table" and tonumber(row.percent) and tonumber(row.percent) > 0 then
      out.breakdown[#out.breakdown + 1] = { name = row.display_name or row.key, percent = math.floor(tonumber(row.percent) + 0.5) }
    end
  end
  return out
end

--- Keychain services where Claude Code may keep the OAuth credentials of `configDir` (its CLAUDE_CONFIG_DIR):
--- "Claude Code-credentials-<first 8 hex of sha256(configDir)>" for a custom dir, then the default one.
--- `sha256hex` is injected (hs.hash.SHA256 in the bridge) so this stays testable.
function core.credentialServices(configDir, sha256hex)
  local out = {}
  if type(configDir) == "string" and configDir ~= "" then
    out[#out + 1] = "Claude Code-credentials-" .. sha256hex(configDir):sub(1, 8)
  end
  out[#out + 1] = "Claude Code-credentials"
  return out
end

--- The knob's Focus Mode: macOS has no public Focus API, so the bridge runs two Shortcuts the user made once.
core.FOCUS_SHORTCUTS = { on = "Knob Focus On", off = "Knob Focus Off" }

--- The Shortcut name for a focus command, or nil and why not.
function core.focusShortcut(message)
  if type(message.on) ~= "boolean" then return nil, "focus needs on: true|false" end
  return core.FOCUS_SHORTCUTS[message.on and "on" or "off"], nil
end

--- The knob's Agent Day Timeline: per worktree, today's minutes of working and needs-you as spans
--- { from, to, "w" | "a" } (minutes of the local day). `done` and idle are not kept: a done lingers for hours
--- until the next prompt and would paint the whole day. Plus two unions over all worktrees: `waiting` (any
--- agent needs you — the minutes the user held the work up) and `working`.
--- A span grows while its state holds and the gap is at most 2 min (polls skip; the Mac sleeps).
local function grow(spans, minute, kind)
  local last = spans[#spans]
  if last and last[3] == kind and minute - last[2] <= 2 and minute >= last[2] then
    last[2] = minute
  else
    spans[#spans + 1] = { minute, minute, kind }
  end
end

function core.dayUpdate(day, worktrees, minute)
  day.rows = day.rows or {}
  day.waiting = day.waiting or {}
  day.working = day.working or {}
  local anyWaiting, anyWorking = false, false
  for _, worktree in ipairs(worktrees or {}) do
    local lead = "idle"
    for _, agent in ipairs(worktree.agents or {}) do
      if agent.bucket == "attention" then lead = "attention"
      elseif agent.bucket == "working" and lead ~= "attention" then lead = "working" end
    end
    if lead ~= "idle" then
      local row = day.rows[worktree.id]
      if not row then
        row = { name = worktree.repo ~= "" and worktree.repo or worktree.name, branch = worktree.name, spans = {} }
        day.rows[worktree.id] = row
      end
      grow(row.spans, minute, lead == "attention" and "a" or "w")
      anyWaiting = anyWaiting or lead == "attention"
      anyWorking = anyWorking or lead == "working"
    end
  end
  if anyWaiting then grow(day.waiting, minute, "a") end
  if anyWorking then grow(day.working, minute, "w") end
end

--- Minutes covered by a list of spans (each span counts its end minute).
function core.spanMinutes(spans)
  local total = 0
  for _, span in ipairs(spans or {}) do total = total + span[2] - span[1] + 1 end
  return total
end

--- The day for GET /api/day: the busiest `limit` rows first, and the two totals.
function core.dayView(day, minute, limit)
  local rows = {}
  for _, row in pairs(day.rows or {}) do rows[#rows + 1] = row end
  table.sort(rows, function(a, b) return core.spanMinutes(a.spans) > core.spanMinutes(b.spans) end)
  local out = {}
  for i = 1, math.min(limit, #rows) do out[i] = { name = rows[i].name, branch = rows[i].branch, spans = rows[i].spans } end
  return { date = day.date, now = minute, waited = core.spanMinutes(day.waiting), worked = core.spanMinutes(day.working),
    rows = out }
end

--- CPU busy percent between two hs.host.cpuUsageTicks() samples (their `overall` tables); nil without a previous.
function core.cpuPercent(previous, current)
  if not previous or not current then return nil end
  local active = (current.active or 0) - (previous.active or 0)
  local idle = (current.idle or 0) - (previous.idle or 0)
  if active + idle <= 0 then return nil end
  return math.floor(100 * active / (active + idle) + 0.5)
end

--- "YouTube\t(3) Song - YouTube" → { source = "YouTube", title = "Song" }; nil for an empty answer.
--- The browser tab title carries a notification count in front and the site's name behind.
function core.nowPlayingOf(raw)
  if type(raw) ~= "string" then return nil end
  local source, title = raw:match("^([^\t]+)\t(.-)%s*$")
  if not source or title == "" then return nil end
  title = title:gsub("^%(%d+%)%s*", ""):gsub("%s*%- YouTube$", "")
  return { source = source, title = core.clip(title, 120) }
end

--- Longest id Orca takes for an item or an option (AGENT_SESSION_RESPONSE_OPTION_ID_MAX_LENGTH).
local ID_MAX = 1024
core.QUESTION_OPTIONS_MAX = 6
core.QUESTIONS_MAX = 4
local QUESTION_TEXT_MAX = 1500
local OPTION_LABEL_MAX = 120

local function isId(value)
  return type(value) == "string" and value ~= "" and #value <= ID_MAX
end

--- Why an answer command cannot run, or nil when it can.
function core.answerProblem(message)
  if not isId(message.id) then return "answer needs an agent id" end
  if not isId(message.itemId) then return "answer needs the question's item id" end
  local revision = message.revision
  if type(revision) ~= "number" or revision < 1 or revision ~= math.floor(revision) then
    return "answer needs the question's revision"
  end
  if message.answers ~= nil then  -- several questions / multi-select: [{questionId, optionIds = {…}}]
    if type(message.answers) ~= "table" or #message.answers == 0 or #message.answers > core.QUESTIONS_MAX then
      return "answers needs one entry per question"
    end
    for _, a in ipairs(message.answers) do
      if type(a) ~= "table" or not isId(a.questionId) or type(a.optionIds) ~= "table" or #a.optionIds == 0 then
        return "each answer needs a question id and its option ids"
      end
      for _, id in ipairs(a.optionIds) do
        if not isId(id) then return "each answer needs a question id and its option ids" end
      end
    end
    return nil
  end
  if not isId(message.optionId) then return "answer needs an option id" end
  return nil
end

--- The pending question agent.cjs read, checked and clipped for GET /api/agent; nil when there is none or it
--- is malformed. A question without options cannot be answered with a tap.
function core.questionOf(raw)
  if type(raw) ~= "table" or not isId(raw.itemId) or type(raw.revision) ~= "number" then return nil end
  local options = {}
  for _, option in ipairs(type(raw.options) == "table" and raw.options or {}) do
    if #options >= core.QUESTION_OPTIONS_MAX then break end
    if type(option) == "table" and isId(option.id) then
      options[#options + 1] = { id = option.id, label = core.clip(option.label, OPTION_LABEL_MAX) }
    end
  end
  -- Every question of the item, for the knob to answer as a group (multi-select too).
  local questions = {}
  for _, q in ipairs(type(raw.questions) == "table" and raw.questions or {}) do
    if #questions >= core.QUESTIONS_MAX or type(q) ~= "table" or not isId(q.id) then break end
    local opts = {}
    for _, option in ipairs(type(q.options) == "table" and q.options or {}) do
      if #opts >= core.QUESTION_OPTIONS_MAX then break end
      if type(option) == "table" and isId(option.id) then
        opts[#opts + 1] = { id = option.id, label = core.clip(option.label, OPTION_LABEL_MAX) }
      end
    end
    questions[#questions + 1] = { id = q.id, text = core.clip(q.text, 400), header = type(q.header) == "string" and core.clip(q.header, 40) or nil,
      multiSelect = q.multiSelect == true, options = opts }
  end
  local groupAnswerable = raw.groupAnswerable == true and type(raw.questions) == "table" and #questions == #raw.questions
  for _, q in ipairs(questions) do
    if #q.options == 0 then groupAnswerable = false end
  end
  return {
    itemId = raw.itemId,
    revision = raw.revision,
    text = core.clip(raw.text, QUESTION_TEXT_MAX),
    header = type(raw.header) == "string" and core.clip(raw.header, 40) or nil,
    options = options,
    answerable = raw.answerable == true and #options > 0,
    questions = #questions > 0 and questions or nil,
    groupAnswerable = groupAnswerable,
  }
end

--- A private IPv4 address (10/8, 172.16/12, 192.168/16) — one a client on the same network can reach.
local function isPrivateIPv4(address)
  local a, b = address:match("^(%d+)%.(%d+)%.%d+%.%d+$")
  a, b = tonumber(a), tonumber(b)
  if not a then return false end
  return a == 10 or (a == 172 and b >= 16 and b <= 31) or (a == 192 and b == 168)
end

--- The URLs a LAN client can reach the remote API on (`port`): the Mac's private IPv4 addresses on Ethernet and
--- Wi-Fi (en*) interfaces, the `primary` interface first. `interfaces` maps a name to its addresses, as
--- hs.network reports them; VPN tunnels, loopback, link-local and IPv6 are skipped. The watch learns the first
--- one from GET /api/state, so a new network needs no reinstall (protocol.md § Remote access).
function core.lanURLs(interfaces, primary, port)
  local names = {}
  for name in pairs(interfaces or {}) do
    if name:match("^en%d+$") then names[#names + 1] = name end
  end
  table.sort(names, function(a, b)
    if a == primary then return true end
    if b == primary then return false end
    return a < b
  end)
  local urls = {}
  for _, name in ipairs(names) do
    for _, address in ipairs(interfaces[name]) do
      if isPrivateIPv4(address) then urls[#urls + 1] = string.format("http://%s:%d", address, port) end
    end
  end
  return urls
end

--- The knob's Prompt Palette: stock prompts in groups, typed into the agent on screen by `say`. The file
--- ~/.config/orca-deck/prompts.json replaces them: [{ "name": "…", "prompts": ["…", …] }, …].
core.PROMPT_GROUPS, core.PROMPTS_PER_GROUP, core.PROMPT_MAX = 6, 12, 200
core.DEFAULT_PROMPTS = {
  { name = "Робота", prompts = { "продовжуй", "що далі?", "поясни, що ти зробив", "покажи diff змін", "зупинись і підсумуй" } },
  { name = "Перевірка", prompts = { "запусти тести", "запусти lint і tsc -b", "перевір візуально", "зроби code review змін", "що лишилось у .work/todo.md?" } },
  { name = "Відповіді", prompts = { "так", "ні", "ок, роби", "зачекай, не роби цього", "давай простіше" } },
}

--- The palette from a decoded prompts.json, trimmed to the knob's limits; the defaults when it is not usable.
function core.promptsOf(doc)
  local out = {}
  for _, group in ipairs(type(doc) == "table" and doc or {}) do
    if #out >= core.PROMPT_GROUPS then break end
    if type(group) == "table" and type(group.name) == "string" and type(group.prompts) == "table" then
      local prompts = {}
      for _, p in ipairs(group.prompts) do
        if type(p) == "string" and p:match("%S") and #prompts < core.PROMPTS_PER_GROUP and (utf8.len(p) or #p) <= core.PROMPT_MAX then
          prompts[#prompts + 1] = p
        end
      end
      if #prompts > 0 then out[#out + 1] = { name = group.name, prompts = prompts } end
    end
  end
  if #out == 0 then return core.DEFAULT_PROMPTS end
  return out
end

--- PR Radar: one GitHub GraphQL query (via `gh api graphql`) for the user's open PRs and the ones waiting on their
--- review. Each PR: { key = "owner/repo#12", repo, number, title, url, checks = ok|fail|run|none, mine, draft,
--- review = approved|changes|"" , failedRuns = { run ids } }.
core.PR_QUERY = [[query {
  viewer { pullRequests(states: OPEN, first: 20, orderBy: {field: UPDATED_AT, direction: DESC}) { nodes { ...pr } } }
  search(type: ISSUE, query: "is:pr is:open review-requested:@me archived:false", first: 10) { nodes { ...pr } }
}
fragment pr on PullRequest {
  number title url isDraft reviewDecision repository { nameWithOwner }
  commits(last: 1) { nodes { commit { statusCheckRollup { state }
    checkSuites(first: 20) { nodes { conclusion workflowRun { databaseId } } } } } }
}]]
core.PR_MAX = 10

local CHECKS = { SUCCESS = "ok", FAILURE = "fail", ERROR = "fail", PENDING = "run", EXPECTED = "run" }

local function prOf(node, mine)
  if type(node) ~= "table" or not tonumber(node.number) or type(node.repository) ~= "table" then return nil end
  local commit = type(node.commits) == "table" and type(node.commits.nodes) == "table" and node.commits.nodes[1]
  commit = type(commit) == "table" and commit.commit or {}
  local rollup = type(commit.statusCheckRollup) == "table" and commit.statusCheckRollup.state or nil
  local failed = {}
  local suites = type(commit.checkSuites) == "table" and commit.checkSuites.nodes or {}
  for _, suite in ipairs(type(suites) == "table" and suites or {}) do
    local run = type(suite) == "table" and type(suite.workflowRun) == "table" and tonumber(suite.workflowRun.databaseId)
    if run and (suite.conclusion == "FAILURE" or suite.conclusion == "TIMED_OUT") then failed[#failed + 1] = run end
  end
  local review = node.reviewDecision == "APPROVED" and "approved" or node.reviewDecision == "CHANGES_REQUESTED" and "changes" or ""
  return { key = node.repository.nameWithOwner .. "#" .. node.number, repo = node.repository.nameWithOwner,
    number = tonumber(node.number), title = type(node.title) == "string" and node.title or "", url = node.url,
    checks = CHECKS[rollup] or "none", mine = mine, draft = node.isDraft == true, review = review, failedRuns = failed }
end

--- The radar's list from the GraphQL answer: failing first, then running, then the rest; at most PR_MAX.
function core.prsOf(doc)
  local data = type(doc) == "table" and doc.data
  if type(data) ~= "table" then return nil end
  local out, seen = {}, {}
  local function add(nodes, mine)
    for _, node in ipairs(type(nodes) == "table" and nodes or {}) do
      local pr = prOf(node, mine)
      if pr and not seen[pr.key] then
        seen[pr.key] = true
        out[#out + 1] = pr
      end
    end
  end
  add(type(data.search) == "table" and data.search.nodes, false)  -- waiting on you: before your own
  add(type(data.viewer) == "table" and type(data.viewer.pullRequests) == "table" and data.viewer.pullRequests.nodes, true)
  local rank = { fail = 1, run = 2, ok = 3, none = 4 }
  for i, pr in ipairs(out) do pr.order = i end
  table.sort(out, function(a, b)
    if a.mine ~= b.mine then return not a.mine end
    if rank[a.checks] ~= rank[b.checks] then return rank[a.checks] < rank[b.checks] end
    return a.order < b.order
  end)
  while #out > core.PR_MAX do table.remove(out) end
  for _, pr in ipairs(out) do pr.order = nil end
  return out
end

--- Why a pr command cannot run against the cached list, or nil and the PR. `n` names a PR the radar knows
--- (the knob never sends a URL); `action`: open | rerun (rerun needs a failed run).
function core.prProblem(message, prs)
  if message.action ~= "open" and message.action ~= "rerun" then return "pr needs action open|rerun" end
  if type(message.n) ~= "string" then return "pr needs n" end
  for _, pr in ipairs(prs or {}) do
    if pr.key == message.n then
      if message.action == "rerun" and #pr.failedRuns == 0 then return "no failed run to re-run" end
      if message.action == "open" and not (type(pr.url) == "string" and pr.url:match("^https://github%.com/")) then
        return "no GitHub URL"
      end
      return nil, pr
    end
  end
  return "unknown PR"
end

--- The knob's Wi-Fi networks from a decoded ~/.config/orca-deck/wifi.json: [{ssid, pass}], at most 8, WPA rules
--- (an SSID of 1–32 bytes; a password of 8–63, or none for an open network). Bad entries are dropped.
core.WIFI_MAX = 8
function core.wifiOf(doc)
  local out, seen = {}, {}
  for _, n in ipairs(type(doc) == "table" and doc or {}) do
    if #out >= core.WIFI_MAX then break end
    local ssid, pass = type(n) == "table" and n.ssid, type(n) == "table" and (n.pass or "")
    if type(ssid) == "string" and #ssid >= 1 and #ssid <= 32 and type(pass) == "string"
        and (#pass == 0 or (#pass >= 8 and #pass <= 63)) and not seen[ssid] then
      seen[ssid] = true
      out[#out + 1] = { ssid = ssid, pass = pass }
    end
  end
  return out
end

--- wifi.json with `ssid` added (or its password replaced), first in the list; nil and a problem when it breaks
--- the WPA rules. An older entry beyond WIFI_MAX falls off the end.
function core.wifiWith(list, ssid, pass)
  if type(ssid) ~= "string" or #ssid < 1 or #ssid > 32 then return nil, "the network name must be 1–32 bytes" end
  if type(pass) ~= "string" or (#pass > 0 and (#pass < 8 or #pass > 63)) then
    return nil, "a Wi-Fi password is 8–63 characters (or none for an open network)"
  end
  local out = { { ssid = ssid, pass = pass } }
  for _, n in ipairs(core.wifiOf(list)) do
    if n.ssid ~= ssid and #out < core.WIFI_MAX then out[#out + 1] = n end
  end
  return out
end

--- The agent with `id` and its worktree, or nil.
function core.agentById(worktrees, id)
  for _, worktree in ipairs(worktrees) do
    for _, agent in ipairs(worktree.agents) do
      if agent.id == id then return agent, worktree end
    end
  end
  return nil
end

return core

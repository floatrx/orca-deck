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
  if (cmd == "say" or cmd == "answer") and via == "ws" then
    return cmd .. " needs the bearer token: use POST /api/cmd"
  end
  return nil
end

--- Longest id Orca takes for an item or an option (AGENT_SESSION_RESPONSE_OPTION_ID_MAX_LENGTH).
local ID_MAX = 1024
core.QUESTION_OPTIONS_MAX = 6
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
  return {
    itemId = raw.itemId,
    revision = raw.revision,
    text = core.clip(raw.text, QUESTION_TEXT_MAX),
    header = type(raw.header) == "string" and core.clip(raw.header, 40) or nil,
    options = options,
    answerable = raw.answerable == true and #options > 0,
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

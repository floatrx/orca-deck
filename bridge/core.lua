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
    for _, agent in ipairs(worktree.agents or {}) do
      agents[#agents + 1] = {
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
      }
    end
    worktrees[#worktrees + 1] = {
      id = worktree.worktreeId,
      name = worktree.displayName or worktree.repo,
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

return core

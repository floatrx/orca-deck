--- Tests for bridge/core.lua. Pure Lua: runs inside Hammerspoon's Lua 5.4 but starts no server and
--- presses no keys. Run with bridge/test/run.sh.

local DIR = debug.getinfo(1, "S").source:match("^@(.*/)")
local core = dofile(DIR .. "../core.lua")

local passed, failed, notes = 0, 0, {}

local function test(name, fn)
  local ok, err = pcall(fn)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    notes[#notes + 1] = "FAIL " .. name .. ": " .. tostring(err)
  end
end

local function eq(actual, expected, label)
  if actual ~= expected then
    error(string.format("%s: expected %s, got %s", label or "value", tostring(expected), tostring(actual)), 2)
  end
end

--- A fake `orca worktree ps --json` document.
local function psDoc(worktrees) return { result = { worktrees = worktrees } } end

local function agent(paneKey, state, extra)
  local a = { paneKey = paneKey, agentType = "claude", state = state, stateStartedAt = 1000 }
  for k, v in pairs(extra or {}) do a[k] = v end
  return a
end

-- bucket ---------------------------------------------------------------------------------------

test("bucket maps Orca's four states", function()
  eq(core.bucket("working"), "working")
  eq(core.bucket("done"), "done")
  eq(core.bucket("blocked"), "attention")
  eq(core.bucket("waiting"), "attention")
end)

test("bucket: unknown or missing state is idle", function()
  eq(core.bucket(nil), "idle")
  eq(core.bucket("unverifiable"), "idle")
  eq(core.bucket(42), "idle")
end)

-- clip -----------------------------------------------------------------------------------------

test("clip collapses whitespace onto one line", function()
  eq(core.clip("  a\n\n b\tc  ", 90), "a b c")
end)

test("clip cuts at the limit and adds an ellipsis", function()
  eq(core.clip("abcdef", 3), "abc…")
  eq(core.clip("abc", 3), "abc")
end)

test("clip never cuts inside a UTF-8 character", function()
  eq(core.clip("привіт", 2), "пр…")
end)

test("clip turns non-strings into an empty string", function()
  eq(core.clip(nil, 5), "")
  eq(core.clip({}, 5), "")
end)

-- worktreesOf ----------------------------------------------------------------------------------

test("worktreesOf maps worktree and agent fields", function()
  local list = core.worktreesOf(psDoc({
    { worktreeId = "w1", displayName = "main", repo = "orca-deck", branch = "refs/heads/main", isActive = true,
      status = "ok", agents = { agent("structured-agent-session:1", "working", { toolName = "Bash", prompt = "do it",
        lastAssistantMessage = "on it", interrupted = false }) } },
  }))
  eq(#list, 1, "worktrees")
  local w = list[1]
  eq(w.id, "w1"); eq(w.name, "main"); eq(w.repo, "orca-deck"); eq(w.branch, "main", "branch")
  eq(w.active, true); eq(w.status, "ok")
  local a = w.agents[1]
  eq(a.id, "structured-agent-session:1"); eq(a.kind, "claude"); eq(a.chat, true, "chat")
  eq(a.state, "working"); eq(a.bucket, "working", "bucket"); eq(a.interrupted, false, "interrupted")
  eq(a.tool, "Bash"); eq(a.prompt, "do it"); eq(a.last, "on it"); eq(a.since, 1000)
end)

test("worktreesOf: name falls back to the repo; a terminal pane is not a chat", function()
  local w = core.worktreesOf(psDoc({ { worktreeId = "w1", repo = "r", agents = { agent("tab:leaf", "done") } } }))[1]
  eq(w.name, "r")
  eq(w.active, false)
  eq(w.agents[1].chat, false)
end)

test("worktreesOf: interrupted is true only when Orca says true", function()
  local w = core.worktreesOf(psDoc({ { worktreeId = "w1", repo = "r", agents = {
    agent("a", "done", { interrupted = true }), agent("b", "done") } } }))[1]
  eq(w.agents[1].interrupted, true)
  eq(w.agents[2].interrupted, false)
end)

test("worktreesOf: the lead is the most urgent agent", function()
  local w = core.worktreesOf(psDoc({ { worktreeId = "w1", repo = "r", agents = {
    agent("a", "done"), agent("b", "working"), agent("c", "blocked"), agent("d", "working") } } }))[1]
  eq(w.lead.id, "c")
end)

test("worktreesOf: lead ties keep Orca's order", function()
  local w = core.worktreesOf(psDoc({ { worktreeId = "w1", repo = "r", agents = {
    agent("a", "done"), agent("b", "working"), agent("c", "working") } } }))[1]
  eq(w.lead.id, "b")
end)

test("worktreesOf: no agents → no lead", function()
  local w = core.worktreesOf(psDoc({ { worktreeId = "w1", repo = "r", agents = {} } }))[1]
  eq(w.lead, nil)
  eq(#w.agents, 0)
end)

test("worktreesOf survives a broken document", function()
  eq(#core.worktreesOf(nil), 0)
  eq(#core.worktreesOf({}), 0)
end)

-- transitions ----------------------------------------------------------------------------------

local function oneWorktree(...)
  return core.worktreesOf(psDoc({ { worktreeId = "w1", displayName = "main", repo = "r", agents = { ... } } }))
end

test("transitions: an agent's first appearance gives no event", function()
  local events, seen = core.transitions({}, oneWorktree(agent("a", "done")))
  eq(#events, 0)
  eq(seen.a, "done")
end)

test("transitions: working → done gives a done event", function()
  local events = core.transitions({ a = "working" }, oneWorktree(agent("a", "done")))
  eq(#events, 1)
  eq(events[1].kind, "done"); eq(events[1].worktreeId, "w1"); eq(events[1].worktree, "main"); eq(events[1].agentId, "a")
end)

test("transitions: an interrupted working → done gives no event", function()
  local events = core.transitions({ a = "working" }, oneWorktree(agent("a", "done", { interrupted = true })))
  eq(#events, 0)
end)

test("transitions: any bucket → attention gives an attention event", function()
  eq(core.transitions({ a = "working" }, oneWorktree(agent("a", "blocked")))[1].kind, "attention")
  eq(core.transitions({ a = "done" }, oneWorktree(agent("a", "waiting")))[1].kind, "attention")
  eq(core.transitions({ a = "idle" }, oneWorktree(agent("a", "blocked")))[1].kind, "attention")
end)

test("transitions: no event when the bucket stays or moves elsewhere", function()
  eq(#core.transitions({ a = "attention" }, oneWorktree(agent("a", "waiting"))), 0)
  eq(#core.transitions({ a = "done" }, oneWorktree(agent("a", "working"))), 0)
  eq(#core.transitions({ a = "attention" }, oneWorktree(agent("a", "done"))), 0)
  eq(#core.transitions({ a = "idle" }, oneWorktree(agent("a", "done"))), 0)
end)

test("transitions: a vanished agent leaves the map", function()
  local _, seen = core.transitions({ a = "working", gone = "done" }, oneWorktree(agent("a", "working")))
  eq(seen.gone, nil)
  eq(seen.a, "working")
end)

-- event log ------------------------------------------------------------------------------------

test("event log: seq starts at the boot value and grows by 1", function()
  local log = core.newEventLog(5000, 100)
  eq(log.seq, 5000)
  local first = log:add({ kind = "done" }, 777)
  eq(first.seq, 5001); eq(first.at, 777); eq(first.type, "event"); eq(first.v, 1)
  eq(log:add({ kind = "attention" }, 778).seq, 5002)
  eq(log.seq, 5002)
end)

test("event log: after(nil) gives nothing — a fresh client replays no pulses", function()
  local log = core.newEventLog(0, 100)
  log:add({ kind = "done" }, 1)
  eq(#log:after(nil), 0)
end)

test("event log: after(since) gives the events with seq > since", function()
  local log = core.newEventLog(0, 100)
  log:add({ kind = "done" }, 1); log:add({ kind = "attention" }, 2); log:add({ kind = "done" }, 3)
  local events = log:after(1)
  eq(#events, 2); eq(events[1].seq, 2); eq(events[2].seq, 3)
  eq(#log:after(3), 0)
end)

test("event log: since ahead of seq gives nothing", function()
  local log = core.newEventLog(0, 100)
  log:add({ kind = "done" }, 1)
  eq(#log:after(99), 0)
end)

test("event log keeps only the newest `capacity` events", function()
  local log = core.newEventLog(0, 3)
  for i = 1, 5 do log:add({ kind = "done" }, i) end
  local events = log:after(0)
  eq(#events, 3); eq(events[1].seq, 3); eq(events[3].seq, 5)
end)

-- HTTP helpers ---------------------------------------------------------------------------------

test("parseTarget splits the path and the query", function()
  local route, query = core.parseTarget("/api/state?since=12&x=a%20b")
  eq(route, "/api/state"); eq(query.since, "12"); eq(query.x, "a b")
  route, query = core.parseTarget("/api/ping")
  eq(route, "/api/ping"); eq(next(query), nil)
end)

test("authorized: Bearer token in any header case", function()
  eq(core.authorized({ Authorization = "Bearer s3cret" }, "s3cret"), true)
  eq(core.authorized({ authorization = "Bearer s3cret" }, "s3cret"), true)
  eq(core.authorized({ AUTHORIZATION = "bearer s3cret" }, "s3cret"), true)
end)

test("authorized: wrong, missing or non-Bearer credentials fail", function()
  eq(core.authorized({ Authorization = "Bearer nope" }, "s3cret"), false)
  eq(core.authorized({}, "s3cret"), false)
  eq(core.authorized(nil, "s3cret"), false)
  eq(core.authorized({ Authorization = "Basic s3cret" }, "s3cret"), false)
end)

test("authorized: an empty configured token never authorizes", function()
  eq(core.authorized({ Authorization = "Bearer " }, ""), false)
  eq(core.authorized({ Authorization = "Bearer x" }, nil), false)
end)

-- ntfy -----------------------------------------------------------------------------------------

test("ntfyMessage: attention is high priority 'needs you'", function()
  local m = core.ntfyMessage({ kind = "attention", worktree = "main" }, "orca-deck", "topic-x")
  eq(m.topic, "topic-x"); eq(m.title, "main · orca-deck"); eq(m.message, "needs you")
  eq(m.tags[1], "warning"); eq(m.priority, 4)
end)

test("ntfyMessage: done is default priority 'done'", function()
  local m = core.ntfyMessage({ kind = "done", worktree = "orca-deck" }, "orca-deck", "t")
  eq(m.title, "orca-deck", "title without a repeated repo"); eq(m.message, "done")
  eq(m.tags[1], "white_check_mark"); eq(m.priority, 3)
end)

test("ntfyMessage carries exactly topic, title, message, tags, priority", function()
  local m = core.ntfyMessage({ kind = "done", worktree = "w", agentId = "secret-pane", prompt = "secret" }, "r", "t")
  local keys = {}
  for k in pairs(m) do keys[#keys + 1] = k end
  table.sort(keys)
  eq(table.concat(keys, ","), "message,priority,tags,title,topic")
end)

return string.format("%d passed, %d failed\n%s", passed, failed, table.concat(notes, "\n"))

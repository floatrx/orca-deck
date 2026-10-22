--- orca-deck bridge — protocol v1 (../protocol.md is the contract). Streams Orca's worktree and agent
--- state to clients and turns their commands into Orca's own key presses and RPCs.
--- Not loaded by init.lua — a Hammerspoon config reload drops it.
---
--- Grown from a snapshot of the spike bridge (owned by another session; never edit the original):
---   /Users/floatrx/orca/workspaces/generic/nursehound/spike/orca-deck/  copied 2026-10-22 22:31
---   bridge.lua 68695f02…  index.html 70460db1… (sha256); reveal.cjs re-copied 23:05 (85cb8874…) and
---   go() ported from the spike's bridge.lua dbecf6b8… (runtime RPCs instead of the Cmd+1…9 search)
---
---   load / reload:  hs -c 'dofile("/Users/floatrx/Projects/Floatrx/orca-deck/bridge/bridge.lua")' </dev/null
---   stop:           hs -c 'deckBridge.stop()' </dev/null
---   mock:           http://localhost:7352/   floating panel: hs -c 'deckBridge.panel()' </dev/null
---   remote API:     http://<host>:7353/api/*  — /api/* only, never the WebSocket or the mock; the only
---                   port a LAN client or a tunnel may reach. Port 7352 always binds localhost.
---   config:         ~/.config/orca-deck/bridge.json (token, ntfy topic, interface), created on first load
---   tests:          bridge/test/run.sh
---
--- The spike bridge (global orcaDeck, port 7351) presses the same keys: this bridge refuses to start
--- while the spike's server runs.

local DIR = debug.getinfo(1, "S").source:match("^@(.*/)")
local core = dofile(DIR .. "core.lua")

local ORCA_CLI = "/opt/homebrew/bin/orca"
local ORCA_BUNDLE = "com.stablyai.orca"
local PORT = 7352
local REMOTE_PORT = 7353
local POLL_SECONDS = 1.5
--- After a key press, read the state again this soon so the clients follow Orca.
local REPOLL_SECONDS = 0.12
--- Orca needs a moment after activate() before it takes key presses.
local ACTIVATE_SECONDS = 0.08
local EVENT_CAPACITY = 100
local NTFY_URL = "https://ntfy.sh/"
local CONFIG_DIR = os.getenv("HOME") .. "/.config/orca-deck"
local CONFIG = CONFIG_DIR .. "/bridge.json"

--- Orca's own shortcuts (shared/keybindings/definitions-core-*.js). Orca's default worktree chords
--- are Cmd+Shift+↓/↑, but a focused chat composer keeps those as text selection
--- (`allowInTextField: 'non-default-chords'`), so "Next/Previous worktree" are remapped in Orca
--- (Settings → Keyboard) to Ctrl+Opt+Cmd+↓/↑. A remapped chord works inside the composer too.
local NEXT_WORKTREE = { mods = { "ctrl", "alt", "cmd" }, key = "down" }
local PREVIOUS_WORKTREE = { mods = { "ctrl", "alt", "cmd" }, key = "up" }
local NEXT_TAB = { mods = { "cmd", "shift" }, key = "]" }
--- reveal.cjs calls Orca's own runtime RPCs, run by Orca's Electron in node mode like the CLI.
local ORCA_ELECTRON = "/Applications/Orca.app/Contents/MacOS/Orca"
--- The mock's floating panel: it never takes focus, so Orca keeps receiving the keys — like the board.
local PANEL = { w = 560, h = 540 }

--- Test trail: every command, event and change of Orca's active workspace, for reading while testing.
local LOG = "/tmp/orca-deck-bridge.log"
local function trail(message)
  local file = io.open(LOG, "a")
  if not file then return end
  file:write(os.date("%H:%M:%S ") .. message .. "\n")
  file:close()
end

local function nowMs() return math.floor(hs.timer.secondsSinceEpoch() * 1000) end

if orcaDeck and orcaDeck.server then
  hs.alert.show("orca-deck: the spike bridge runs on 7351 — stop it first", 4)
  return "refused: the spike bridge (global orcaDeck, port 7351) is running. Stop it first: hs -c 'orcaDeck.stop()'"
end

if deckBridge then deckBridge.stop() end

local function randomHex(bytes)
  local file = assert(io.open("/dev/urandom", "rb"))
  local raw = file:read(bytes)
  file:close()
  return (raw:gsub(".", function(char) return string.format("%02x", char:byte()) end))
end

--- Reads ~/.config/orca-deck/bridge.json, or creates it (mode 600) with a fresh token and ntfy topic.
--- A file that exists but does not parse is an error: overwriting it would lose the token.
local function loadConfig()
  local file = io.open(CONFIG, "r")
  if file then
    local raw = file:read("a")
    file:close()
    local ok, config = pcall(hs.json.decode, raw)
    if not ok or type(config) ~= "table" then return nil, CONFIG .. " is not valid JSON" end
    return config
  end
  local config = { token = randomHex(24), ntfyTopic = "orca-deck-" .. randomHex(12), ntfy = true, interface = "localhost" }
  hs.fs.mkdir(CONFIG_DIR)
  os.execute("chmod 700 '" .. CONFIG_DIR .. "'")
  local out = assert(io.open(CONFIG, "w"))
  out:write(hs.json.encode(config, true), "\n")
  out:close()
  os.execute("chmod 600 '" .. CONFIG .. "'")
  trail("config created: " .. CONFIG)
  return config
end

local config, problem = loadConfig()
if not config then
  hs.alert.show("orca-deck: " .. problem, 4)
  return "refused: " .. problem
end

deckBridge = {
  config = config,
  lan = config.interface == "lan",
  worktrees = {},
  payload = nil,
  buckets = nil,
  events = core.newEventLog(nowMs(), EVENT_CAPACITY),
  polling = false,
  again = false,
  timers = {},
  tasks = {},
}

--- Defers `fn`; the timer is kept because an unreferenced hs.timer can be collected before it fires.
local function later(seconds, fn)
  if #deckBridge.timers > 32 then table.remove(deckBridge.timers, 1) end
  deckBridge.timers[#deckBridge.timers + 1] = hs.timer.doAfter(seconds, fn)
end

local function repoOf(worktreeId)
  for _, worktree in ipairs(deckBridge.worktrees) do
    if worktree.id == worktreeId then return worktree.repo end
  end
  return nil
end

--- Sends an event to the websocket client and to ntfy. `hs.httpserver:send()` reaches one websocket
--- client only, so websocket clients also collect events with `hello {since}`.
local function announce(event)
  local raw = "?"
  for _, worktree in ipairs(deckBridge.worktrees) do
    for _, agent in ipairs(worktree.agents) do
      if agent.id == event.agentId then raw = tostring(agent.state) end
    end
  end
  -- The raw Orca state is logged so the first live `blocked` / `waiting` can be recorded in protocol.md.
  trail(string.format("event %s (orca state %s) · %s · seq %d", event.kind, raw, event.worktree, event.seq))
  if deckBridge.server then deckBridge.server:send(hs.json.encode(event)) end
  local topic = deckBridge.config.ntfyTopic
  if deckBridge.config.ntfy == false or type(topic) ~= "string" or topic == "" then return end
  local body = hs.json.encode(core.ntfyMessage(event, repoOf(event.worktreeId), topic))
  hs.http.asyncPost(NTFY_URL, body, { ["Content-Type"] = "application/json" }, function(status, answer)
    if status ~= 200 then trail(string.format("ntfy failed: %s %s", status, core.clip(answer, 120))) end
  end)
end

--- Runs `orca worktree ps --json` off the main thread, emits events for bucket changes, and pushes
--- the list when it changed. A poll asked for while one runs is remembered and runs right after it.
local function poll()
  if deckBridge.polling then
    deckBridge.again = true
    return
  end
  deckBridge.polling = true
  deckBridge.task = hs.task.new(ORCA_CLI, function(code, out)
    deckBridge.polling = false
    local ok, doc = pcall(hs.json.decode, out or "")
    if code == 0 and ok and type(doc) == "table" then
      local worktrees = core.worktreesOf(doc)
      local fresh, seen = core.transitions(deckBridge.buckets or {}, worktrees)
      deckBridge.buckets = seen
      for _, worktree in ipairs(worktrees) do
        if worktree.active and worktree.id ~= deckBridge.activeId then
          deckBridge.activeId = worktree.id
          trail("orca active → " .. worktree.name .. " (" .. worktree.repo .. ")")
        end
      end
      deckBridge.worktrees = worktrees
      for _, event in ipairs(fresh) do announce(deckBridge.events:add(event, nowMs())) end
      local payload = hs.json.encode({ type = "state", v = core.PROTOCOL, seq = deckBridge.events.seq, worktrees = worktrees })
      if payload ~= deckBridge.payload then
        deckBridge.payload = payload
        if deckBridge.server then deckBridge.server:send(payload) end
      end
    end
    if deckBridge.again then
      deckBridge.again = false
      poll()
    end
  end, { "worktree", "ps", "--json" })
  deckBridge.task:start()
end

--- Presses `chord` in Orca. When Orca is frontmost it is a plain key press. Otherwise `background`
--- posts the key to Orca's process without raising it (the knob turning while you are elsewhere),
--- and without it Orca is brought forward first.
local function press(chord, background)
  local app = hs.application.get(ORCA_BUNDLE)
  if not app then return "Orca is not running" end
  local fire = function(target)
    hs.eventtap.keyStroke(chord.mods, chord.key, 0, target)
    later(REPOLL_SECONDS, poll)
  end
  if app:isFrontmost() then
    fire()
  elseif background then
    fire(app)
  else
    app:activate()
    later(ACTIVATE_SECONDS, fire)
  end
  return nil
end

--- Runs reveal.cjs (Orca's own runtime RPCs) with `args`, logs the runtime's verdict, then reads the
--- state again so the clients follow Orca.
local function runtime(args, label)
  local task = hs.task.new(ORCA_ELECTRON, function(code, out)
    local verdict = (out or ""):match('"navigated":(%a+)') or (out or ""):match('"activated":(%a+)')
    trail(string.format("%s → %s", label, verdict or ("failed, exit " .. code .. " " .. (out or ""))))
    later(REPOLL_SECONDS, poll)
  end, args)
  task:setEnvironment({
    ELECTRON_RUN_AS_NODE = "1",
    HOME = os.getenv("HOME"),
    USER = os.getenv("USER"),
    TMPDIR = os.getenv("TMPDIR"),
    PATH = "/usr/bin:/bin",
  })
  if #deckBridge.tasks > 8 then table.remove(deckBridge.tasks, 1) end
  deckBridge.tasks[#deckBridge.tasks + 1] = task  -- held so a running task is not collected
  task:start()
  return nil
end

--- Jumps Orca to the workspace `id`: straight to the agent `pane` (terminal.reveal) when there is one,
--- otherwise the workspace as it was last left (worktree.activate). Exact for any workspace, no key
--- presses, and keyboard focus stays where it is. A pane key is "<tabId>:<leafId>".
local function go(id, pane)
  if type(id) ~= "string" or id == "" then return "go needs an id" end
  if type(pane) == "string" and pane ~= "" then
    local tabId, leafId = pane:match("^(.-):(.+)$")
    return runtime({ DIR .. "reveal.cjs", "pane", id, tabId or pane, leafId }, "reveal")
  end
  return runtime({ DIR .. "reveal.cjs", "worktree", id }, "activate")
end

--- "Enter" the workspace the knob is on: bring Orca forward. Orca puts focus back where it was,
--- which in a chat workspace is the composer — ready for typing or a → for the Next prompt.
local function enter()
  local app = hs.application.get(ORCA_BUNDLE)
  if not app then return "Orca is not running" end
  app:activate()
  return nil
end

local function ack(cmd, problem)
  return { type = "ack", v = core.PROTOCOL, cmd = cmd, ok = problem == nil, note = problem }
end

--- Runs one client command (both transports) and returns its ack.
local function run(message, via)
  local cmd = message.cmd
  local front = hs.application.frontmostApplication()
  trail(string.format("cmd %s%s via %s · front=%s", tostring(cmd), message.step and (" " .. message.step) or "", via,
    front and front:name() or "?"))
  if cmd == "go" then
    return ack(cmd, go(message.id, message.pane))
  elseif cmd == "turn" then
    return ack(cmd, press((tonumber(message.step) or 1) < 0 and PREVIOUS_WORKTREE or NEXT_WORKTREE, true))
  elseif cmd == "enter" then
    return ack(cmd, enter())
  elseif cmd == "tab" then
    return ack(cmd, press(NEXT_TAB))
  elseif cmd == "next" then
    return ack(cmd, press({ mods = {}, key = "right" }))
  elseif cmd == "send" then
    return ack(cmd, press({ mods = {}, key = "return" }))
  end
  return ack(tostring(cmd), "unknown command")
end

--- The state message; with `since`, it also carries the events after it.
local function stateMessage(since)
  if since == nil then return deckBridge.payload or hs.json.encode({ type = "state", v = core.PROTOCOL, seq = deckBridge.events.seq, worktrees = {} }) end
  return hs.json.encode({ type = "state", v = core.PROTOCOL, seq = deckBridge.events.seq, worktrees = deckBridge.worktrees,
    events = deckBridge.events:after(since) })
end

local function onMessage(raw)
  local ok, message = pcall(hs.json.decode, raw or "")
  if not ok or type(message) ~= "table" then message = {} end
  if message.cmd == "hello" then return stateMessage(message.since) end
  return hs.json.encode(run(message, "ws"))
end

local JSON_HEADERS = { ["Content-Type"] = "application/json", ["Cache-Control"] = "no-store" }

local function json(code, value) return hs.json.encode(value), code, JSON_HEADERS end

--- The token-protected /api/* (protocol.md § HTTP), served on both ports. The callback must answer
--- synchronously — hs.httpserver has no deferred response, hence no long-poll.
local function onApi(method, route, query, headers, body)
  local from = (headers or {})["X-Remote-Addr"] or "?"
  if not core.authorized(headers, deckBridge.config.token) then
    trail(string.format("401 %s %s from %s", method, route, from))
    return json(401, { error = "unauthorized" })
  end
  if method == "GET" and route == "/api/ping" then
    return json(200, { v = core.PROTOCOL, ok = true, at = nowMs() })
  elseif method == "GET" and route == "/api/state" then
    return json(200, { v = core.PROTOCOL, seq = deckBridge.events.seq, worktrees = deckBridge.worktrees,
      events = deckBridge.events:after(query.since) })
  elseif method == "POST" and route == "/api/cmd" then
    local ok, message = pcall(hs.json.decode, body or "")
    if not ok or type(message) ~= "table" or type(message.cmd) ~= "string" or message.cmd == "hello" then
      return json(400, { error = "bad command" })
    end
    return json(200, run(message, "http " .. from))
  elseif method == "POST" and route == "/api/log" then
    local ok, message = pcall(hs.json.decode, body or "")
    if not ok or type(message) ~= "table" or type(message.text) ~= "string" then return json(400, { error = "bad log" }) end
    trail(string.format("client log from %s: %s", from, message.text:sub(1, 4000)))
    return json(200, { v = core.PROTOCOL, ok = true })
  end
  return json(404, { error = "not found" })
end

--- Port 7353, the only port a LAN client or a tunnel may reach: /api/* and nothing else.
local function onRemoteRequest(method, target, headers, body)
  local route, query = core.parseTarget(target)
  if route:sub(1, 5) == "/api/" then return onApi(method, route, query, headers, body) end
  return json(404, { error = "not found" })
end

--- Port 7352, localhost only: /api/* and the mock page (the WebSocket is attached in serve()).
local function onLocalRequest(method, target, headers, body)
  local route, query = core.parseTarget(target)
  if route:sub(1, 5) == "/api/" then return onApi(method, route, query, headers, body) end
  if method ~= "GET" or (route ~= "/" and route ~= "/index.html") then return "not found", 404, {} end
  local file = io.open(DIR .. "index.html", "r")
  if not file then return "index.html missing", 500, {} end
  local page = file:read("a")
  file:close()
  return page, 200, { ["Content-Type"] = "text/html; charset=utf-8", ["Cache-Control"] = "no-store" }
end

deckBridge.poller = hs.timer.doEvery(POLL_SECONDS, poll)
poll()

--- The screen for the panel: one without Orca's window when there is one, so the "device" sits
--- beside Orca instead of on top of it.
local function panelScreen()
  local app = hs.application.get(ORCA_BUNDLE)
  local orcaScreen = app and app:mainWindow() and app:mainWindow():screen()
  for _, screen in ipairs(hs.screen.allScreens()) do
    if screen ~= orcaScreen then return screen end
  end
  return hs.screen.mainScreen()
end

--- Opens the mock in a floating panel at the bottom-right of a screen. It stays above other windows
--- (full-screen Spaces too) and clicking it does not activate Hammerspoon, so focus stays in Orca.
function deckBridge.panel()
  if deckBridge.view then deckBridge.view:delete() end
  local screen = panelScreen():frame()
  local frame = { x = screen.x + screen.w - PANEL.w - 24, y = screen.y + screen.h - PANEL.h - 24, w = PANEL.w, h = PANEL.h }
  deckBridge.view = hs.webview.new(frame)
    :windowStyle({ "titled", "utility", "closable", "nonactivating" })
    :windowTitle("orca-deck")
    :level(hs.drawing.windowLevels.floating)
    :behaviorAsLabels({ "canJoinAllSpaces", "fullScreenAuxiliary" })
    :allowTextEntry(false)
    :url(string.format("http://127.0.0.1:%d/", PORT))
    :show()
  return "panel open"
end

--- Emits a synthetic event for the active worktree, to check ntfy and the clients' haptics:
---   hs -c 'return deckBridge.testEvent("attention")' </dev/null
function deckBridge.testEvent(kind)
  kind = kind == "done" and "done" or "attention"
  local target = deckBridge.worktrees[1] or { id = "test", name = "test" }
  for _, worktree in ipairs(deckBridge.worktrees) do
    if worktree.active then target = worktree end
  end
  local event = deckBridge.events:add({ kind = kind, worktreeId = target.id, worktree = target.name, agentId = "test" }, nowMs())
  announce(event)
  return string.format("test %s event for %s, seq %d", kind, target.name, event.seq)
end

function deckBridge.stop()
  if deckBridge.view then deckBridge.view:delete() end
  deckBridge.view = nil
  if deckBridge.poller then deckBridge.poller:stop() end
  if deckBridge.task and deckBridge.task:isRunning() then deckBridge.task:terminate() end
  for _, timer in ipairs(deckBridge.timers) do timer:stop() end
  if deckBridge.server then deckBridge.server:stop() end
  if deckBridge.remote then deckBridge.remote:stop() end
  deckBridge.server, deckBridge.remote = nil, nil
  return "orca-deck bridge stopped"
end

--- Starts one listener under deckBridge[slot]. A reload can find the port still held by the instance
--- it just stopped (start() then reports port 0), so it retries for a few seconds.
local function listen(slot, port, interface, callback, websocket, attempt)
  attempt = attempt or 1
  local server = hs.httpserver.new(false, false):setInterface(interface):setPort(port):setCallback(callback)
  if websocket then server:websocket("/ws", websocket) end
  server:start()
  if server:getPort() == port then
    deckBridge[slot] = server
    trail(string.format("listening on port %d, interface %s", port, interface or "all"))
    return
  end
  server:stop()
  if attempt < 10 then
    later(0.5, function() listen(slot, port, interface, callback, websocket, attempt + 1) end)
  else
    hs.alert.show("orca-deck: port " .. port .. " stays busy", 3)
  end
end

listen("server", PORT, "localhost", onLocalRequest, onMessage)
listen("remote", REMOTE_PORT, not deckBridge.lan and "localhost" or nil, onRemoteRequest)

return string.format("orca-deck bridge starting: local http://localhost:%d/, remote API %s:%d", PORT,
  deckBridge.lan and "all interfaces" or "localhost", REMOTE_PORT)

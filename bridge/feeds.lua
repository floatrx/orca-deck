--- The knob's feed apps (.work/plans/knob-dev-apps.md): one mechanism for fifteen developer apps. Each app is a
--- *feed* — a header, a hub subtitle and pages of items — built here from Orca's state, git, the CLIs and files;
--- the knob draws every feed with one template (knob/deck/app_feed.cpp) and sends its gestures back as
--- {"cmd":"app","app","action","key","page","text","agent"}. GET /api/feeds returns them all; the relay sends one
--- `G` line per app, on change.
---
--- Slow sources (git, the orca CLI, Electron for a session's context, vercel) refresh in the background on their
--- own clocks — faster for the app open on the knob — and a GET only reads the caches.

local feeds = {}

local C = { working = 0x4CC3FF, attention = 0xFF7A1A, done = 0x3DDC84, idle = 0x5D5B57, red = 0xE5484D,
  yellow = 0xFFD166, ink = 0xF1EFE9, violet = 0xB48CFF }

local ORDER = { "queue", "turns", "git", "context", "launch", "digest", "wrap", "hands", "logs", "diff", "deploy", "ask",
  "shutter", "toggl", "macros" }

local MODELS = { { "Opus", "claude-opus-5-5" }, { "Sonnet", "claude-sonnet-5-5" }, { "Haiku", "claude-haiku-5-5" } }
local ARM_SECONDS = 5
local OPEN_SECONDS = 120  -- the knob says "open" on show and every minute while the app stays on screen

function feeds.new(ctx)
  local fc, trail = ctx.fc, ctx.trail
  local HOME = os.getenv("HOME")
  local S = { lang = "ua", notes = {}, seq = 0, cache = {}, open = nil, openAt = 0, arm = nil, asks = {}, macroResult = {},
    tasks = {} }

  local function T(en, ua) return S.lang == "en" and en or ua end
  local function now() return hs.timer.secondsSinceEpoch() end
  local function note(app, text)
    S.seq = S.seq + 1
    S.notes[app] = { t = fc.clip(text, 120), n = S.seq }
    trail("feed " .. app .. ": " .. fc.clip(text, 60))
  end
  --- An alert the knob shows as a banner with a buzz, once (feed.al / feed.an). `key` makes it once per thing.
  S.alerts, S.alerted = {}, {}
  local function alert(app, key, text)
    if S.alerted[key] then return end
    S.alerted[key] = true
    S.seq = S.seq + 1
    S.alerts[app] = { t = fc.clip(text, 80), n = S.seq }
    trail("feed alert " .. app .. ": " .. fc.clip(text, 60))
  end
  local function isOpen(app) return S.open == app and now() - S.openAt < OPEN_SECONDS end
  local function every(app, fast, slow) return isOpen(app) and fast or slow end

  -- ---- background work ----
  local ENV = { HOME = HOME, USER = os.getenv("USER"), TMPDIR = os.getenv("TMPDIR"),
    PATH = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", LANG = "en_US.UTF-8" }

  local function keep(task)
    if #S.tasks > 12 then table.remove(S.tasks, 1) end
    S.tasks[#S.tasks + 1] = task
  end

  --- Runs `cmd` with `args`; stdout goes to a file (hs.task does not drain a pipe while the task runs, and a big
  --- answer blocked the CLI forever — the bridge's `ps` lesson). done(code, out).
  local function run(cmd, args, done, cwd, input)
    local out = os.tmpname()
    local task = hs.task.new("/bin/sh", function(code)
      local file = io.open(out)
      local text = file and file:read("a") or ""
      if file then file:close() end
      os.remove(out)
      done(code, text)
    end, { "-c", '"$0" "$@" > "' .. out .. '" 2>/dev/null', cmd, table.unpack(args) })
    if not task then return done(-1, "") end
    task:setEnvironment(ENV)
    if cwd then task:setWorkingDirectory(cwd) end
    if input then task:setInput(input) end
    keep(task)
    if not task:start() then done(-1, "") end
  end

  --- The cache `key`, refreshed by `fn(done)` when older than `seconds`; returns what it holds now.
  local function cached(key, seconds, fn)
    local c = S.cache[key]
    if not c then c = {} S.cache[key] = c end
    if not c.busy and (not c.at or now() - c.at >= seconds) then
      c.busy = true
      local ok, problem = pcall(fn, function(value)
        c.busy, c.at = false, now()
        if value ~= nil then c.value = value end
      end)
      if not ok then c.busy, c.at = false, now() trail("feed cache " .. key .. ": " .. tostring(problem)) end
    end
    return c.value
  end
  local function drop(key) if S.cache[key] then S.cache[key].at = nil end end

  -- ---- Orca's state ----
  local function worktrees() return ctx.worktrees() or {} end
  local function active()
    local list = worktrees()
    for _, w in ipairs(list) do if w.active then return w end end
    return list[1]
  end
  local function byId(id)
    for _, w in ipairs(worktrees()) do if w.id == id then return w end end
  end
  local function title(w) return w and (w.repo or w.name) or "?" end
  local function meta(w)
    if not w then return "" end
    return w.branch and w.branch ~= "" and w.branch or w.name or ""
  end
  local function agentOf(id)
    for _, w in ipairs(worktrees()) do
      for _, a in ipairs(w.agents or {}) do if a.id == id then return a, w end end
    end
  end
  --- The chat agent that takes a prompt for worktree `w`: the knob's own target when it is in `w`, else its first.
  local function chatOf(w, prefer)
    if not w then return nil end
    local first
    for _, a in ipairs(w.agents or {}) do
      if a.chat then
        if a.id == prefer then return a end
        first = first or a
      end
    end
    return first
  end
  --- A repeat of the same text to the same agent within 15 s is a second press while the note was on its way
  --- (user, 2026-11-16: «Поясни зміни в …» arrived twice, 5 s apart): it is not sent again.
  S.lastSay = {}
  local function sayTo(agent, text)
    if not agent then return T("no Claude chat there", "там немає чату Claude") end
    local last = S.lastSay[agent.id]
    if last and last.text == text and now() - last.at < 15 then return T("already sent", "вже надіслано") end
    local problem = ctx.say({ cmd = "say", id = agent.id, text = text })
    if not problem then S.lastSay[agent.id] = { text = text, at = now() } end
    return problem
  end
  local function exists(path) return path and hs.fs.attributes(path) ~= nil end
  local function short(path) return (path or ""):gsub("^" .. HOME:gsub("%p", "%%%0"), "~") end
  local function stamp() return os.date("%H:%M") end

  --- A deliberate second tap within ARM_SECONDS does the act; the first arms it and says so.
  local function armed(app, key, what)
    if S.arm and S.arm.app == app and S.arm.key == key and now() - S.arm.at < ARM_SECONDS then
      S.arm = nil
      return true
    end
    S.arm = { app = app, key = key, at = now() }
    note(app, T("again to ", "ще раз — ") .. what)
    return false
  end

  local function item(key, t, detail, color, badge)
    return { k = tostring(key), t = fc.clip(t or "", fc.TITLE_MAX), d = fc.clip(detail or "", fc.DETAIL_MAX), c = color or C.idle,
      b = badge and fc.clip(badge, 12) or nil }
  end

  -- ---- the .work folder of a worktree: its own, else the main repo's ----
  local function workDir(w)
    local path = w and fc.pathOf(w.id)
    if not path then return nil end
    local key = "workdir:" .. path
    local c = S.cache[key]
    if c and c.value ~= nil then return c.value or nil end
    local dir = path .. "/.work"
    if not exists(dir .. "/todo.md") then
      local pipe = io.popen("/usr/bin/git -C '" .. path:gsub("'", "'\\''") .. "' rev-parse --path-format=absolute --git-common-dir 2>/dev/null")
      local root = pipe and fc.repoRootOf(pipe:read("a") or "") or nil
      if pipe then pipe:close() end
      dir = root and exists(root .. "/.work") and root .. "/.work" or (exists(dir) and dir or false)
    end
    S.cache[key] = { value = dir, at = now() }
    return dir or nil
  end

  local function writeFile(path, text)
    local file = io.open(path, "w")
    if not file then return false end
    file:write(text)
    file:close()
    return true
  end

  -- ======================================================================================================
  -- The apps. Each: build() → feed fields; act(action, msg) → nil | problem (a note says what happened).
  -- ======================================================================================================
  local apps = {}

  -- 1. Queue — the active worktree's todo.md / backlog.md; a tap hands the item to the agent on screen.
  local QUEUE_FILES = { { "todo", "todo.md" }, { "backlog", "backlog.md" } }
  apps.queue = {
    name = { "Queue", "Черга" },
    build = function()
      local w = active()
      local dir = workDir(w)
      local pages, open = {}, 0
      S.queue = {}
      for p, f in ipairs(QUEUE_FILES) do
        local items = {}
        local list = dir and fc.todoItems(ctx.readFile(dir .. "/" .. f[2])) or {}
        for _, it in ipairs(list) do
          local key = f[2] .. "|" .. it.line
          S.queue[key] = it.title
          items[#items + 1] = item(key, it.title, it.given and T("given ", "віддано ") .. it.given or it.detail,
            it.given and C.working or C.idle, it.given and "↗" or nil)
        end
        if p == 1 then open = #items end
        local agent = chatOf(w)
        pages[#pages + 1] = { n = f[1] .. " · " .. #items, i = items, hd = 1, tp = 1,
          e = dir and T("nothing open in ", "нічого відкритого в ") .. f[2] or T("no .work/todo.md here", "тут немає .work/todo.md"),
          to = agent and "→ " .. title(w) or T("no chat here", "тут немає чату"),
          ht = T("tap: give · hold: add", "тап: віддати · утримай: +") }
      end
      return { h = title(w), s = open > 0 and open .. T(" open", " у черзі") or T("todo.md", "todo.md"), pg = pages }
    end,
    act = function(action, msg)
      local w = active()
      local dir = workDir(w)
      if not dir then return T("no .work here", "тут немає .work") end
      if action == "dictate" then
        local path = dir .. "/todo.md"
        local text, why = fc.appendItem(ctx.readFile(path) or "# Todo\n", msg.text or "")
        if not text then return why end
        if not writeFile(path, text) then return "cannot write todo.md" end
        note("queue", "+ " .. msg.text)
        return nil
      end
      if action ~= "tap" then return nil end
      local key = tostring(msg.key or "")
      local file, line = key:match("^(.-)|(%d+)$")
      local itemTitle = S.queue and S.queue[key]
      if not file or not itemTitle then return T("the list changed", "список змінився") end
      local agent = chatOf(w, msg.agent)
      local problem = sayTo(agent, T("Take this on: ", "Візьми в роботу: ") .. itemTitle)
      if problem then return problem end
      local path = dir .. "/" .. file
      local marked = fc.markGiven(ctx.readFile(path) or "", tonumber(line), itemTitle, stamp())
      if marked then writeFile(path, marked) end
      note("queue", "✓ " .. itemTitle .. " → " .. title(w))
      return nil
    end,
  }

  -- 2. Turns — the agents that work right now, the longest first; tap: go there, hold twice: stop the turn.
  apps.turns = {
    name = { "Turns", "Довгий хід" },
    build = function()
      local list, nowMs = {}, ctx.nowMs()
      for _, w in ipairs(worktrees()) do
        for _, a in ipairs(w.agents or {}) do
          if a.bucket == "working" then list[#list + 1] = { a = a, w = w, m = fc.minutes(a.since, nowMs) } end
        end
      end
      table.sort(list, function(x, y) return x.m > y.m end)
      local items = {}
      for _, e in ipairs(list) do
        if e.m >= 30 then alert("turns", "long:" .. e.a.id .. ":" .. tostring(e.a.since), title(e.w) .. T(": working 30 min", ": працює вже 30 хв")) end
        local what = e.a.tool and e.a.tool ~= "" and e.a.tool or (e.a.prompt or "")
        items[#items + 1] = item(e.a.id, title(e.w), meta(e.w) .. (what ~= "" and " · " .. what or ""),
          e.m >= 30 and C.attention or e.m >= 10 and C.yellow or C.working, fc.duration(e.m, S.lang ~= "en"))
      end
      local longest = list[1] and fc.duration(list[1].m, S.lang ~= "en")
      local long = 0
      for _, e in ipairs(list) do if e.m >= 30 then long = long + 1 end end
      return { bd = long, bc = C.attention, h = T("working now", "зараз працюють"), s = #list > 0 and #list .. T(" working · ", " працює · ") .. longest or T("nobody works", "ніхто не працює"),
        pg = { { n = T("turns", "ходи") .. " · " .. #list, i = items, hd = 2, tp = 1, e = T("nobody is working", "ніхто не працює"),
          ht = T("tap: go · hold ×2: stop", "тап: туди · утримай ×2: стоп") } } }
    end,
    act = function(action, msg)
      local agent, w = agentOf(msg.key)
      if not agent then return T("it finished", "вже завершив") end
      if action == "tap" then
        note("turns", "→ " .. title(w))
        return ctx.go(w.id, agent.id)
      end
      if action == "hold" then
        local session = ctx.sessionIdOf(agent.id)
        if not session then return T("only chat turns can be stopped here", "зупинити можна лише чат") end
        if not armed("turns", agent.id, T("stop ", "зупинити ") .. title(w)) then return nil end
        ctx.agentScript({ "stop", session }, nil, function(result, problem)
          note("turns", result and "■ " .. T("stopped ", "зупинено ") .. title(w) or T("stop failed: ", "не зупинився: ") .. tostring(problem))
          ctx.poll()
        end)
      end
      return nil
    end,
  }

  -- 3. Git Pulse — every worktree's branch, dirty files and ahead / behind; tap: the right prompt for its state.
  local function gitStates()
    return cached("git", every("git", 15, every("wrap", 15, 60)), function(done)
      local paths, seen = {}, {}
      for _, w in ipairs(worktrees()) do
        local p = fc.pathOf(w.id)
        if p and not seen[p] then seen[p] = true paths[#paths + 1] = { path = p, id = w.id } end
      end
      local out, i = {}, 0
      local function step()
        i = i + 1
        if i > #paths then return done(out) end
        run("/usr/bin/git", { "-C", paths[i].path, "status", "--porcelain=v2", "--branch" }, function(code, text)
          if code == 0 then out[paths[i].id] = fc.gitStatusOf(text) end
          step()
        end)
      end
      step()
    end) or {}
  end
  local function gitLine(s)
    local parts = {}
    if s.dirty > 0 then parts[#parts + 1] = s.dirty .. T(" changed", " змін") end
    if s.ahead > 0 then parts[#parts + 1] = "↑" .. s.ahead end
    if s.behind > 0 then parts[#parts + 1] = "↓" .. s.behind end
    if not s.upstream then parts[#parts + 1] = T("no upstream", "без upstream") end
    return #parts > 0 and table.concat(parts, " · ") or T("clean", "чисто")
  end
  local function gitPrompt(s)
    if s.dirty > 0 then return T("Prepare a commit of the current changes: show me the summary and wait for my ok.",
      "Підготуй коміт поточних змін: покажи summary і чекай мого ок.") end
    if s.ahead > 0 then return T("Push the current branch.", "Запуш поточну гілку.") end
    if s.behind > 0 then return "/actualize-branch" end
    return T("What is next in .work/todo.md?", "Що далі в .work/todo.md?")
  end
  apps.git = {
    name = { "Git Pulse", "Гілки" },
    build = function()
      local states, items, dirty, rank = gitStates(), {}, 0, {}
      for _, w in ipairs(worktrees()) do
        local s = states[w.id]
        if s then
          if s.dirty > 0 or s.ahead > 0 then dirty = dirty + 1 end
          rank[w.id] = (s.dirty > 0 and 3 or s.ahead > 0 and 2 or s.behind > 0 and 1 or 0) * 1000 - #items  -- work first, Orca's order inside
          items[#items + 1] = item(w.id, title(w), (s.branch ~= "" and s.branch or meta(w)) .. " · " .. gitLine(s),
            s.dirty > 0 and C.attention or s.ahead > 0 and C.yellow or s.behind > 0 and C.working or C.done,
            s.dirty > 0 and "●" .. s.dirty or s.ahead > 0 and "↑" .. s.ahead or s.behind > 0 and "↓" .. s.behind or "✓")
        end
      end
      table.sort(items, function(x, y) return rank[x.k] > rank[y.k] end)
      return { h = T("branches", "гілки"), s = dirty > 0 and dirty .. T(" need a push / commit", " чекають коміт / пуш") or T("all clean", "все чисто"),
        pg = { { n = T("worktrees", "воркрі") .. " · " .. #items, i = items, hd = 2, tp = 1, e = T("reading git…", "читаю git…"),
          ht = T("tap: prompt · hold: go", "тап: промпт · утримай: туди") } } }
    end,
    act = function(action, msg)
      local w = byId(msg.key)
      if not w then return T("gone", "зник") end
      if action == "hold" then return ctx.go(w.id) end
      if action ~= "tap" then return nil end
      local s = gitStates()[w.id]
      if not s then return T("not read yet", "ще не прочитано") end
      local text = gitPrompt(s)
      local problem = sayTo(chatOf(w, msg.agent), text)
      if problem then return problem end
      drop("git")
      note("git", "✓ " .. title(w) .. ": " .. text)
      return nil
    end,
  }

  -- 4. Context — how full each chat session's context window is (agentSession.history → contextUsage).
  S.context = {}
  local function refreshContext()
    if S.contextBusy then return end
    local interval = every("context", 20, 90)
    local queue = {}
    for _, w in ipairs(worktrees()) do
      for _, a in ipairs(w.agents or {}) do
        local session = a.chat and ctx.sessionIdOf(a.id)
        local c = session and S.context[a.id]
        if session and (not c or now() - c.at >= interval) then queue[#queue + 1] = { id = a.id, session = session } end
      end
    end
    if #queue == 0 then return end
    S.contextBusy = true
    local i = 0
    local function step()
      i = i + 1
      if i > #queue or i > 4 then S.contextBusy = false return end  -- four Electron runs per pass at most
      ctx.agentScript({ "context", queue[i].session }, nil, function(result)
        S.context[queue[i].id] = { at = now(), used = result and result.used, window = result and result.window,
          pct = result and result.percentage, model = result and result.model }
        step()
      end)
    end
    step()
  end
  apps.context = {
    name = { "Context", "Контекст" },
    build = function()
      refreshContext()
      local items, worst = {}, -1
      for _, w in ipairs(worktrees()) do
        for _, a in ipairs(w.agents or {}) do
          if a.chat then
            local c = S.context[a.id] or {}
            local pct = tonumber(c.pct)
            if pct and pct > worst then worst = pct end
            if pct and pct >= 85 then alert("context", "full:" .. a.id, title(w) .. T(": context ", ": контекст ") .. pct .. "%") end
            local model = (c.model or ""):gsub("^claude%-", "")
            items[#items + 1] = item(a.id, title(w), c.used and fc.tokens(c.used) .. " / " .. fc.tokens(c.window) .. (model ~= "" and " · " .. model or "")
              or T("reading…", "читаю…"), not pct and C.idle or pct >= 80 and C.red or pct >= 60 and C.attention or pct >= 40 and C.yellow or C.done,
              pct and pct .. "%" or "…")
          end
        end
      end
      table.sort(items, function(x, y) return (tonumber((x.b or ""):match("%d+")) or -1) > (tonumber((y.b or ""):match("%d+")) or -1) end)
      local full = 0
      for _, it in ipairs(items) do if (tonumber((it.b or ""):match("%d+")) or 0) >= 80 then full = full + 1 end end
      return { bd = full, bc = C.red, h = T("context", "контекст"), s = worst >= 0 and T("fullest ", "найповніший ") .. worst .. "%" or T("chat sessions", "чат-сесії"),
        pg = { { n = T("sessions", "сесії") .. " · " .. #items, i = items, hd = 2, tp = 1, e = T("no chat sessions", "немає чат-сесій"),
          ht = T("tap: go · hold: summary", "тап: туди · утримай: підсумок") } } }
    end,
    act = function(action, msg)
      local agent, w = agentOf(msg.key)
      if not agent then return T("gone", "зникла") end
      if action == "tap" then return ctx.go(w.id, agent.id) end
      if action == "hold" then
        local problem = sayTo(agent, T("Summarise the state into .work/pause.md so we can continue in a fresh session.",
          "Підсумуй стан у .work/pause.md, далі продовжимо в новій сесії."))
        if not problem then note("context", "✓ " .. title(w) .. T(": summary asked", ": попросив підсумок")) end
        return problem
      end
      return nil
    end,
  }

  -- 5. Launcher — a new Claude chat in a repo, one page per model; tap twice: launch with the default prompt.
  local function repos()
    return cached("repos", 600, function(done)
      run("/opt/homebrew/bin/orca", { "repo", "list", "--json" }, function(code, text)
        local ok, doc = pcall(hs.json.decode, text)
        local list = {}
        for _, r in ipairs(ok and doc and doc.result and doc.result.repos or {}) do
          if r.path and r.kind == "git" then list[#list + 1] = { path = r.path, name = r.displayName or r.path:match("[^/]+$") } end
        end
        done(code == 0 and list or nil)
      end)
    end) or {}
  end
  local function launch(path, model, prompt, label)
    run("/opt/homebrew/bin/orca", { "agent", "launch", "--agent", "claude", "--worktree", "path:" .. path, "--surface", "chat",
      "--model", model, "--prompt", prompt, "--json" }, function(code, text)
      local ok = code == 0 and text:find('"ok":%s*true')
      note("launch", ok and "▶ " .. label or T("launch failed", "не запустився") .. " (" .. code .. ")")
      ctx.later(1.5, ctx.poll)
    end)
  end
  apps.launch = {
    name = { "Launcher", "Запуск" },
    build = function()
      local list, pages = repos(), {}
      -- The repos in use first (a live worktree in Orca, the active one on top), then the rest; 15 at most, so the
      -- three pages stay a few KB on BLE.
      local current = active()
      local rank = {}
      for n, w in ipairs(worktrees()) do
        for _, r in ipairs(list) do
          local p = fc.pathOf(w.id) or ""
          if (r.name == w.repo or p:find(r.path, 1, true) == 1) and not rank[r.path] then rank[r.path] = (w == current and 1000 or 500) - n end
        end
      end
      local sorted = {}
      for n, r in ipairs(list) do sorted[#sorted + 1] = { r = r, k = rank[r.path] or -n } end
      table.sort(sorted, function(x, y) return x.k > y.k end)
      for _, m in ipairs(MODELS) do
        local items = {}
        for n, e in ipairs(sorted) do
          if n > 15 then break end
          items[#items + 1] = item(e.r.path, e.r.name, e.r.path:match("([^/]+)/[^/]+$") or "", C.violet)
        end
        pages[#pages + 1] = { n = m[1], i = items, hd = 1, tp = 1, e = T("reading repos…", "читаю репо…"),
          ht = T("tap ×2: start · hold: prompt", "тап ×2: старт · утримай: промпт") }
      end
      return { h = T("new agent", "новий агент"), s = #list .. T(" repos", " репо"), pg = pages }
    end,
    act = function(action, msg)
      local model = MODELS[(tonumber(msg.page) or 0) + 1] or MODELS[1]
      local path = tostring(msg.key or "")
      if not exists(path) then return T("pick a repo", "вибери репо") end
      local name = path:match("[^/]+$")
      if action == "tap" then
        if not armed("launch", path .. model[2], T("start ", "запустити ") .. model[1] .. " · " .. name) then return nil end
        launch(path, model[2], T("Continue from .work/todo.md.", "Продовжуй з .work/todo.md."), model[1] .. " · " .. name)
      elseif action == "dictate" and (msg.text or "") ~= "" then
        launch(path, model[2], msg.text, model[1] .. " · " .. name)
      end
      return nil
    end,
  }

  -- 6. Digest — today so far: per worktree how long agents worked and waited (the Day timeline, kept on disk), the
  -- dones since the bridge started, today's commits per repo, and the PRs that want something.
  local function commitsToday()
    return cached("commits", every("digest", 60, 600), function(done)
      local paths, seen = {}, {}
      for _, w in ipairs(worktrees()) do
        local p = fc.pathOf(w.id)
        if p and not seen[p] then seen[p] = true paths[#paths + 1] = { path = p, w = w } end
      end
      local out, i = {}, 0
      local function step()
        i = i + 1
        if i > #paths then return done(out) end
        run("/usr/bin/git", { "-C", paths[i].path, "log", "--since=midnight", "--no-merges", "--format=%s" }, function(code, text)
          local subjects = {}
          for line in (text or ""):gmatch("[^\n]+") do subjects[#subjects + 1] = line end
          if code == 0 and #subjects > 0 then out[#out + 1] = { w = paths[i].w, n = #subjects, last = subjects[1] } end
          step()
        end)
      end
      step()
    end) or {}
  end
  apps.digest = {
    name = { "Digest", "Дайджест" },
    build = function()
      local dones = {}
      for _, e in ipairs(ctx.events() or {}) do
        if e.kind == "done" and e.worktreeId then dones[e.worktreeId] = (dones[e.worktreeId] or 0) + 1 end
      end
      local day = ctx.day() or {}
      local rows = {}
      for id, row in pairs(day.rows or {}) do
        local worked, waited = 0, 0
        for _, span in ipairs(row.spans or {}) do
          local m = span[2] - span[1] + 1
          if span[3] == "a" then waited = waited + m else worked = worked + m end
        end
        rows[#rows + 1] = { id = id, row = row, worked = worked, waited = waited }
      end
      table.sort(rows, function(x, y) return x.worked > y.worked end)
      local ua = S.lang ~= "en"
      local items, total = {}, 0
      for _, r in ipairs(rows) do
        total = total + r.worked
        local d = T("worked ", "працював ") .. fc.duration(r.worked, ua)
        if r.waited > 0 then d = d .. T(" · waited ", " · чекав ") .. fc.duration(r.waited, ua) end
        if dones[r.id] then d = d .. " · " .. dones[r.id] .. T(" done", " готово") end
        local w = byId(r.id)
        items[#items + 1] = item("w:" .. r.id, w and title(w) or r.row.name or "?", (r.row.branch or "") .. " · " .. d,
          r.waited > 0 and C.attention or C.working, fc.duration(r.worked, ua))
      end
      local commits, ncommits = {}, 0
      for _, c in ipairs(commitsToday()) do
        ncommits = ncommits + c.n
        commits[#commits + 1] = item("w:" .. c.w.id, title(c.w), c.last, C.done, tostring(c.n))
      end
      table.sort(commits, function(x, y) return tonumber(x.b) > tonumber(y.b) end)
      local prs = {}
      for _, pr in ipairs(ctx.prs() or {}) do
        local bad = pr.checks == "fail" or pr.review == "changes"
        if bad or pr.review == "approved" or not pr.mine then
          prs[#prs + 1] = item("p:" .. pr.key, pr.title, pr.key .. " · " .. (not pr.mine and T("review requested", "просять рев’ю")
            or pr.checks == "fail" and T("checks failed", "чеки впали") or pr.review == "changes" and T("changes asked", "просять зміни")
            or T("approved", "схвалено")), bad and C.red or not pr.mine and C.yellow or C.done)
        end
      end
      local reviews = 0
      for _, pr in ipairs(ctx.prs() or {}) do if not pr.mine then reviews = reviews + 1 end end
      return { bd = reviews, bc = C.yellow, h = T("today", "сьогодні"), s = fc.duration(total, ua) .. T(" of agent work · ", " роботи агентів · ") .. ncommits .. T(" commits", " комітів"),
        pg = { { n = T("agents", "агенти") .. " · " .. #items, i = items, tp = 1, e = T("a quiet day so far", "поки тихий день"), ht = T("tap: go", "тап: перейти") },
          { n = T("commits", "коміти") .. " · " .. ncommits, i = commits, tp = 1, e = T("no commits today", "сьогодні без комітів"), ht = T("tap: go", "тап: перейти") },
          { n = "PR · " .. #prs, i = prs, tp = 1, e = T("no PR news", "PR без новин"), ht = T("tap: open", "тап: відкрити") } } }
    end,
    act = function(action, msg)
      if action ~= "tap" then return nil end
      local key = tostring(msg.key or "")
      if key:sub(1, 2) == "w:" then
        if not byId(key:sub(3)) then return T("that worktree is gone", "цього воркрі вже немає") end
        return ctx.go(key:sub(3))
      end
      for _, pr in ipairs(ctx.prs() or {}) do
        if "p:" .. pr.key == key and pr.url then hs.urlevent.openURL(pr.url) return nil end
      end
      return T("gone", "зник")
    end,
  }

  -- 7. Wrap-up — the end-of-day checklist: agents still working, uncommitted and unpushed work.
  apps.wrap = {
    name = { "Wrap-up", "Кінець дня" },
    build = function()
      local items, states = {}, gitStates()
      for _, w in ipairs(worktrees()) do
        for _, a in ipairs(w.agents or {}) do
          if a.bucket == "working" or a.bucket == "attention" then
            items[#items + 1] = item("a:" .. a.id, title(w), a.bucket == "working" and T("still working", "ще працює") or T("waits for you", "чекає на тебе"),
              a.bucket == "working" and C.working or C.attention, a.chat and T("pause", "пауза") or nil)
          end
        end
        local s = states[w.id]
        if s and (s.dirty > 0 or s.ahead > 0) then
          items[#items + 1] = item("g:" .. w.id, title(w), meta(w) .. " · " .. gitLine(s), s.dirty > 0 and C.attention or C.yellow,
            s.dirty > 0 and "●" .. s.dirty or "↑" .. s.ahead)
        end
      end
      return { h = T("end of day", "кінець дня"), s = #items > 0 and #items .. T(" to close", " закрити") or T("all closed", "все закрито"),
        pg = { { n = T("checklist", "чекліст") .. " · " .. #items, i = items, tp = 1, e = T("all clean — have a good evening ✓", "все чисто — гарного вечора ✓"),
          ht = T("tap: pause / commit", "тап: пауза / коміт") } } }
    end,
    act = function(action, msg)
      if action ~= "tap" then return nil end
      local key = tostring(msg.key or "")
      if key:sub(1, 2) == "a:" then
        local agent, w = agentOf(key:sub(3))
        if not agent then return T("gone", "зник") end
        local problem = sayTo(agent.chat and agent or nil, T("pause", "пауза"))
        if not problem then note("wrap", "■ " .. title(w)) end
        return problem
      end
      local w = byId(key:sub(3))
      local s = w and gitStates()[w.id]
      if not s then return T("gone", "зник") end
      local problem = sayTo(chatOf(w, msg.agent), gitPrompt(s))
      if not problem then note("wrap", "✓ " .. title(w)) drop("git") end
      return problem
    end,
  }

  -- 8. Hands Off — the claudeControl lock (~/.hammerspoon/claude_control.lua): who drives the Mac; tap: take it back.
  apps.hands = {
    name = { "Hands Off", "Руки геть" },
    build = function()
      local cc = rawget(_G, "claudeControl")
      local mode = cc and cc.status() or "none"
      local info = cc and cc.info and cc.info() or {}
      local i
      -- One alert per take-over: the transition into "active" (a key from the clock drifted by a second and alerted twice).
      if mode == "active" and S.handsMode ~= "active" then
        S.alerted["hands"] = nil
        alert("hands", "hands", T("Claude drives the Mac", "Claude керує Маком") .. (info.reason and info.reason ~= "" and ": " .. info.reason or ""))
      end
      S.handsMode = mode
      if mode == "active" then
        i = item("cc", T("Claude drives the Mac", "Claude керує Маком"), (info.reason or "") .. (info.step and info.step ~= "" and " · " .. info.step or ""),
          C.red, info.seconds and (info.seconds < 60 and info.seconds .. T(" s", " с") or fc.duration(info.seconds // 60, S.lang ~= "en")) or nil)
      elseif mode == "interrupted" then
        i = item("cc", T("Interrupted — yours", "Перервано — твій"), info.step or "", C.attention)
      elseif mode == "none" then
        i = item("cc", T("claudeControl not loaded", "claudeControl не завантажено"), "", C.idle)
      else
        i = item("cc", T("The Mac is yours", "Мак вільний"), T("nobody drives it", "ніхто не керує"), C.done)
      end
      return { bd = mode == "active" and 1 or 0, bc = C.red, h = T("hands off", "руки геть"), s = mode == "active" and T("Claude drives!", "Claude керує!") or mode == "interrupted" and T("interrupted", "перервано") or T("the Mac is yours", "Мак вільний"),
        pg = { { n = T("the Mac", "Мак"), i = { i }, tp = 1, ht = mode == "active" and T("tap: take it back", "тап: забрати керування") or "" } } }
    end,
    act = function(action)
      local cc = rawget(_G, "claudeControl")
      if action ~= "tap" or not cc then return nil end
      if cc.status() ~= "active" then return T("nothing to take back", "нема чого забирати") end
      if not cc.interrupt then return "claudeControl.interrupt missing" end
      cc.interrupt(T("taken back from the knob", "забрав керування з шайби"))
      note("hands", T("taken back — the Mac is yours", "керування твоє"))
      return nil
    end,
  }

  -- 9. Log Tail — the last lines of the open terminals (the active worktree first); errors in red; hold: hand a line to the agent.
  --- The open terminals and their last lines, read in one pass: a handle from `terminal list` goes stale at the next
  --- list (measured 2026-11-16: "terminal_handle_stale"), so the reads must use the list they came with.
  local function terminals()
    return cached("terms", every("logs", 8, 600), function(done)
      run("/opt/homebrew/bin/orca", { "terminal", "list", "--json" }, function(code, text)
        local ok, doc = pcall(hs.json.decode, text)
        local list = {}
        for _, t in ipairs(code == 0 and ok and doc and doc.result and doc.result.terminals or {}) do
          if t.connected then list[#list + 1] = t end
        end
        local here = active() and active().id
        table.sort(list, function(x, y)
          if (x.worktreeId == here) ~= (y.worktreeId == here) then return x.worktreeId == here end
          return (tonumber(x.lastOutputAt) or 0) > (tonumber(y.lastOutputAt) or 0)
        end)
        while #list > 5 do table.remove(list) end
        local i = 0
        local function step()
          i = i + 1
          if i > #list then return done(list) end
          run("/opt/homebrew/bin/orca", { "terminal", "read", "--terminal", list[i].handle, "--limit", "40", "--json" }, function(rc, out)
            local fine, read = pcall(hs.json.decode, out)
            list[i].tail = rc == 0 and fine and read and read.result and read.result.terminal and read.result.terminal.tail or nil
            step()
          end)
        end
        step()
      end)
    end) or {}
  end
  local function lineColor(line)
    local l = line:lower()
    if l:find("error") or l:find("failed") or l:find("exception") or l:find("exit code [1-9]") or l:find("✗") then return C.red end
    if l:find("warn") then return C.yellow end
    return C.idle
  end
  apps.logs = {
    name = { "Log Tail", "Лог" },
    build = function()
      local list = terminals()
      local pages, errors = {}, 0
      for n, t in ipairs(list) do
        if n > 5 then break end
        local items = {}
        local lines = t.tail or {}
        for k = math.max(1, #lines - 24), #lines do
          local line = (lines[k] or ""):gsub("\27%[[%d;]*%a", "")
          if line:match("%S") then
            local c = lineColor(line)
            if c == C.red then errors = errors + 1 end
            items[#items + 1] = item((t.ptyId or t.handle) .. "|" .. k, line, "", c)
          end
        end
        if #items == 0 and t.preview then items[1] = item((t.ptyId or t.handle) .. "|0", t.preview, "", C.idle) end
        local repo = (t.worktreePath or ""):match("[^/]+$") or ""
        local same = 0
        for k = 1, n - 1 do if (list[k].worktreePath or "") == (t.worktreePath or "") then same = same + 1 end end
        pages[#pages + 1] = { n = fc.clip(repo ~= "" and repo or "terminal", 20) .. (same > 0 and " · " .. (same + 1) or ""), i = items, hd = 2, last = 1, e = T("reading…", "читаю…"),
          ht = T("hold: fix this line", "утримай: виправ рядок") }
      end
      if #pages == 0 then pages[1] = { n = T("terminals", "термінали"), i = {}, e = T("no open terminals", "немає відкритих терміналів") } end
      return { h = T("terminals", "термінали"), s = errors > 0 and errors .. T(" error lines", " рядків з помилками") or #list .. T(" terminals", " терміналів"), pg = pages }
    end,
    act = function(action, msg)
      if action ~= "hold" then return nil end
      local pty, k = tostring(msg.key or ""):match("^(.*)|(%d+)$")
      local line, term
      for _, t in ipairs(terminals()) do
        if (t.ptyId or t.handle) == pty then line, term = (t.tail or {})[tonumber(k)], t end
      end
      if not line then return T("the log moved", "лог зсунувся") end
      local w = active()
      if term then w = byId(term.worktreeId) or w end
      local problem = sayTo(chatOf(w, msg.agent), T("Fix this error from the terminal: ", "Виправ цю помилку з термінала: ") .. line:gsub("\27%[[%d;]*%a", ""))
      if not problem then note("logs", "✓ " .. T("sent to ", "надіслано в ") .. title(w)) end
      return problem
    end,
  }

  -- 10. Diff — the active worktree's changed files; tap: Orca's diff tab, hold: ask the agent to explain the file.
  local function diffOf(w)
    local path = w and fc.pathOf(w.id)
    if not path then return {} end
    return cached("diff:" .. path, every("diff", 8, 120), function(done)
      run("/usr/bin/git", { "-C", path, "diff", "--numstat", "HEAD" }, function(_, numstat)
        run("/usr/bin/git", { "-C", path, "ls-files", "--others", "--exclude-standard" }, function(_, untracked)
          done(fc.numstatOf(numstat, untracked))
        end)
      end)
    end) or {}
  end
  apps.diff = {
    name = { "Diff", "Дифф" },
    build = function()
      local w = active()
      local files, items, add, del = diffOf(w), {}, 0, 0
      for n, f in ipairs(files) do
        if n > 30 then break end
        add, del = add + f.add, del + f.del
        local dir = f.path:match("^(.*)/[^/]+$")
        items[#items + 1] = item(f.path, f.path:match("[^/]+$"), dir or "./", f.new and C.done or f.del > f.add and C.red or C.working,
          f.new and T("new", "нов") or f.binary and "bin" or "+" .. f.add .. " −" .. f.del)
      end
      return { h = title(w), s = #files > 0 and #files .. T(" files · +", " файлів · +") .. add .. " −" .. del or T("no changes", "без змін"),
        pg = { { n = T("changes", "зміни") .. " · " .. #files, i = items, hd = 2, tp = 1, e = T("no changes", "без змін"),
          ht = T("tap: diff · hold: explain", "тап: дифф · утримай: поясни") } } }
    end,
    act = function(action, msg)
      local w = active()
      local file = tostring(msg.key or "")
      if file == "" or not w then return nil end
      if action == "tap" then
        run("/opt/homebrew/bin/orca", { "file", "diff", "--path", file, "--worktree", "id:" .. w.id, "--focus" }, function(code)
          if code ~= 0 then note("diff", T("diff failed", "дифф не відкрився")) end
        end)
        note("diff", "↗ " .. file:match("[^/]+$"))
      elseif action == "hold" then
        local problem = sayTo(chatOf(w, msg.agent), T("Explain the changes in ", "Поясни зміни в ") .. file)
        if not problem then note("diff", "✓ " .. T("asked about ", "спитав про ") .. file:match("[^/]+$")) end
        return problem
      end
      return nil
    end,
  }

  -- 11. Deploy — the latest Vercel deployment per project (the CLI's login) and the Railway projects.
  apps.deploy = {
    name = { "Deploy", "Деплой" },
    build = function()
      local vercel = cached("vercel", every("deploy", 30, 300), function(done)
        run("/opt/homebrew/bin/vercel", { "list", "--all", "--json", "-n", "40" }, function(code, text)
          local ok, doc = pcall(hs.json.decode, text)
          done(code == 0 and ok and doc and doc.deployments or nil)
        end, "/tmp")
      end) or {}
      local railway = cached("railway", 900, function(done)
        run("/opt/homebrew/bin/railway", { "list", "--json" }, function(code, text)
          local ok, doc = pcall(hs.json.decode, text)
          done(code == 0 and ok and type(doc) == "table" and doc or nil)
        end, "/tmp")
      end) or {}
      local items, seen, failing = {}, {}, 0
      for _, d in ipairs(vercel) do
        if d.name and not seen[d.name] then
          seen[d.name] = true
          local state = d.state or d.readyState or "?"
          if state == "ERROR" then
            failing = failing + 1
            alert("deploy", "err:" .. (d.url or d.name), d.name .. T(": deploy failed", ": деплой впав"))
          end
          local ago = d.createdAt and fc.duration(fc.minutes(d.createdAt, ctx.nowMs()), S.lang ~= "en") or ""
          items[#items + 1] = item("v:" .. (d.url or ""), d.name, (d.target or "preview") .. " · " .. ago .. T(" ago", " тому"),
            state == "READY" and C.done or state == "ERROR" and C.red or (state == "BUILDING" or state == "QUEUED") and C.working or C.idle,
            state == "READY" and "✓" or state == "ERROR" and "✗" or state:sub(1, 5):lower())
        end
      end
      local rail = {}
      for _, p in ipairs(railway) do
        local services = p.services and p.services.edges and #p.services.edges or 0
        rail[#rail + 1] = item("r:" .. (p.id or ""), p.name or "?", services .. T(" services", " сервісів"), C.violet)
      end
      return { bd = failing, bc = C.red, h = T("deploys", "деплої"), s = failing > 0 and failing .. T(" failing", " впало") or #items .. " Vercel · " .. #rail .. " Railway",
        pg = { { n = "Vercel · " .. #items, i = items, tp = 1, e = T("reading vercel…", "читаю vercel…"), ht = T("tap: open", "тап: відкрити") },
          { n = "Railway · " .. #rail, i = rail, tp = 1, e = T("reading railway…", "читаю railway…"), ht = T("tap: open", "тап: відкрити") } } }
    end,
    act = function(action, msg)
      if action ~= "tap" then return nil end
      local key = tostring(msg.key or "")
      if key:sub(1, 2) == "v:" and #key > 2 then hs.urlevent.openURL("https://" .. key:sub(3))
      elseif key:sub(1, 2) == "r:" and #key > 2 then hs.urlevent.openURL("https://railway.com/project/" .. key:sub(3))
      else return nil end
      note("deploy", "↗ " .. T("opened", "відкрито"))
      return nil
    end,
  }

  -- 12. Quick Ask — hold, ask out loud; Haiku answers on the knob (`claude -p`); tap: hand the answer to the agent.
  local function ask(question)
    S.asking = question
    note("ask", T("thinking…", "думаю…"))
    run("/opt/homebrew/bin/claude", { "-p", "--model", "claude-haiku-5-5", "--append-system-prompt",
      T("Answer in at most three short sentences, plain text, no markdown.", "Відповідай українською, не більше трьох коротких речень, простий текст без markdown.") },
      function(code, text)
        S.asking = nil
        text = (text or ""):match("^%s*(.-)%s*$")
        if code ~= 0 or text == "" then note("ask", T("no answer", "немає відповіді") .. " (" .. code .. ")") return end
        table.insert(S.asks, 1, { q = question, a = text, at = os.date("%H:%M") })
        while #S.asks > 6 do table.remove(S.asks) end
        note("ask", T("✓ answered", "✓ відповів"))
      end, HOME, question)
  end
  apps.ask = {
    name = { "Quick Ask", "Питання" },
    build = function()
      local last, answer, history = S.asks[1], {}, {}
      if S.asking then answer[1] = item("wait", S.asking, T("thinking…", "думаю…"), C.working) end
      if last then
        for n, chunk in ipairs(fc.chunks(last.a, 80)) do answer[#answer + 1] = item("a:" .. n, chunk, n == 1 and last.q or "", C.ink) end
      end
      for n, a in ipairs(S.asks) do history[#history + 1] = item("h:" .. n, a.q, a.a, C.violet, a.at) end
      return { h = "Haiku", s = last and fc.clip(last.q, 28) or T("hold and ask", "утримай і спитай"),
        pg = { { n = T("answer", "відповідь"), i = answer, hd = 1, tp = 1, e = T("hold, ask, tap", "утримай, спитай, тапни"), to = "→ " .. title(active()),
          ht = T("hold: ask · tap: to agent", "утримай: питай · тап: агенту") },
          { n = T("history", "історія") .. " · " .. #history, i = history, hd = 1, tp = 1, e = T("nothing yet", "ще нічого"),
            ht = T("tap: to the agent", "тап: агенту") } } }
    end,
    act = function(action, msg)
      if action == "dictate" and (msg.text or ""):match("%S") then ask(msg.text) return nil end
      if action ~= "tap" then return nil end
      local key = tostring(msg.key or "")
      local entry = key:sub(1, 2) == "h:" and S.asks[tonumber(key:sub(3)) or 0] or S.asks[1]
      if not entry or key == "wait" then return nil end
      local w = active()
      local problem = sayTo(chatOf(w, msg.agent), T("Haiku answered my question «", "Haiku відповів на моє питання «") .. entry.q .. "»: " .. entry.a)
      if not problem then note("ask", "✓ → " .. title(w)) end
      return problem
    end,
  }

  -- 13. Shutter — a screenshot of the front window, sent to the agent on screen as a path it can read.
  local SHOTS = HOME .. "/Desktop/knob-shots"
  local function shots()
    local list = {}
    if exists(SHOTS) then
      for name in hs.fs.dir(SHOTS) do
        if name:match("%.png$") then list[#list + 1] = name end
      end
    end
    table.sort(list, function(a, b) return a > b end)
    return list
  end
  local function shoot(comment, agent, w)
    local win = hs.window.frontmostWindow()
    if not win then return T("no front window", "немає переднього вікна") end
    hs.fs.mkdir(SHOTS)
    local app = win:application() and win:application():name() or "window"
    local path = SHOTS .. "/" .. os.date("%Y-%m-%d_%H%M%S") .. "-" .. app:gsub("[^%w]+", "-") .. ".png"
    run("/usr/sbin/screencapture", { "-x", "-o", "-l" .. win:id(), path }, function(code)
      if code ~= 0 or not exists(path) then note("shutter", T("capture failed", "не знялось")) return end
      local text = (comment and comment ~= "" and comment .. "\n\n" or "") .. T("Look at this screenshot: ", "Подивись на скріншот: ") .. path
      local problem = sayTo(agent, text)
      note("shutter", problem and problem or "● " .. app .. " → " .. title(w))
    end)
    return nil
  end
  apps.shutter = {
    name = { "Shutter", "Затвор" },
    build = function()
      local front = hs.window.frontmostWindow()
      local app = front and front:application() and front:application():name() or "?"
      local items = { item("shoot", T("Shoot: ", "Зняти: ") .. app, T("the front window → the agent", "переднє вікно → агенту"), C.attention, "●") }
      for n, name in ipairs(shots()) do
        if n > 8 then break end
        items[#items + 1] = item("f:" .. name, name:match("_(%d%d%d%d%d%d)") and name:match("_(%d%d)(%d%d)") .. ":" .. name:match("_%d%d(%d%d)") .. " · " .. (name:match("%-([^%-]+)%.png$") or "") or name,
          T("tap: send again", "тап: надіслати ще раз"), C.idle)
      end
      local w = active()
      return { h = T("shutter", "затвор"), s = app,
        pg = { { n = T("shots", "знімки"), i = items, hd = 1, tp = 1, to = "→ " .. title(w),
          ht = T("tap: shoot · hold: + words", "тап: зняти · утримай: + слова") } } }
    end,
    act = function(action, msg)
      local w = active()
      local agent = chatOf(w, msg.agent)
      if not agent then return T("no chat here", "тут немає чату") end
      local key = tostring(msg.key or "")
      if action == "dictate" then return shoot(msg.text, agent, w) end
      if action ~= "tap" then return nil end
      if key == "shoot" then return shoot(nil, agent, w) end
      local name = key:match("^f:(.+)$")
      if not name or not exists(SHOTS .. "/" .. name) then return T("gone", "зник") end
      local problem = sayTo(agent, T("Look at this screenshot: ", "Подивись на скріншот: ") .. SHOTS .. "/" .. name)
      if not problem then note("shutter", "● → " .. title(w)) end
      return problem
    end,
  }

  -- 14. Toggl — what runs now, start / stop on a project (token `togglToken` in bridge.json, never in the repo).
  local TOGGL = "https://api.track.toggl.com/api/v9"
  local function toggl(method, path, body, done)
    local token = ctx.config().togglToken
    if not token then return done(nil) end
    local headers = { Authorization = "Basic " .. hs.base64.encode(token .. ":api_token"), ["Content-Type"] = "application/json" }
    hs.http.doAsyncRequest(TOGGL .. path, method, body and hs.json.encode(body) or nil, headers, function(code, text)
      local ok, doc = pcall(hs.json.decode, text or "")
      done(code >= 200 and code < 300 and (ok and doc or true) or nil, code)
    end)
  end
  local function togglState()
    if not ctx.config().togglToken then return nil end
    local me = cached("toggl:me", 3600, function(done) toggl("GET", "/me?with_related_data=true", nil, function(doc) done(doc) end) end)
    local current = cached("toggl:current", every("toggl", 10, 120), function(done)
      toggl("GET", "/me/time_entries/current", nil, function(doc) done(doc or false) end)
    end)
    return me, current
  end
  apps.toggl = {
    name = { "Toggl", "Час" },
    build = function()
      if not ctx.config().togglToken then
        return { h = "Toggl", s = T("no token", "немає токена"), pg = { { n = "Toggl", i = {},
          e = T("add togglToken to ~/.config/orca-deck/bridge.json", "додай togglToken у ~/.config/orca-deck/bridge.json") } } }
      end
      local me, current = togglState()
      local projects, names = {}, {}
      for _, p in ipairs(me and me.projects or {}) do
        if p.active ~= false then projects[#projects + 1] = p names[p.id] = p.name end
      end
      local w = active()
      local repo = (w and w.repo or ""):lower()
      table.sort(projects, function(a, b)
        local am, bm = repo ~= "" and a.name:lower():find(repo, 1, true) ~= nil, repo ~= "" and b.name:lower():find(repo, 1, true) ~= nil
        if am ~= bm then return am end
        return a.name < b.name
      end)
      local items = {}
      if type(current) == "table" and current.id then
        items[1] = item("stop", "■ " .. (names[current.project_id] or current.description or T("running", "іде")),
          (current.description or "") ~= "" and current.description or T("tap: stop", "тап: стоп"), C.red,
          fc.duration(fc.minutes(fc.isoMs(current.start), ctx.nowMs()), S.lang ~= "en"))
      else
        items[1] = item("none", T("Nothing runs", "Нічого не йде"), T("tap a project to start", "тапни проєкт — старт"), C.idle)
      end
      for _, p in ipairs(projects) do
        items[#items + 1] = item("p:" .. p.id, p.name, p.client_name or "", tonumber((p.color or ""):gsub("#", ""), 16) or C.violet, "▶")
      end
      local running = type(current) == "table" and current.id
      return { h = "Toggl", s = running and "▶ " .. (names[current.project_id] or T("running", "іде")) or T("stopped", "зупинено"),
        pg = { { n = T("projects", "проєкти") .. " · " .. #projects, i = items, tp = 1, e = T("reading Toggl…", "читаю Toggl…"),
          ht = T("tap: start / stop", "тап: старт / стоп") } } }
    end,
    act = function(action, msg)
      if action ~= "tap" then return nil end
      local me, current = togglState()
      if not me then return T("Toggl not read yet", "Toggl ще не прочитано") end
      local key = tostring(msg.key or "")
      local wid = me.default_workspace_id
      if key == "stop" and type(current) == "table" and current.id then
        toggl("PATCH", "/workspaces/" .. current.workspace_id .. "/time_entries/" .. current.id .. "/stop", nil, function(ok)
          note("toggl", ok and "■ " .. T("stopped", "зупинено") or T("stop failed", "не зупинилось"))
          drop("toggl:current")
        end)
        return nil
      end
      local pid = tonumber(key:match("^p:(%d+)$"))
      if not pid then return nil end
      local w = active()
      toggl("POST", "/workspaces/" .. wid .. "/time_entries", { created_with = "orca-deck knob", workspace_id = wid, project_id = pid,
        start = os.date("!%Y-%m-%dT%H:%M:%SZ"), duration = -1, description = w and title(w) or "" }, function(ok, code)
        note("toggl", ok and "▶ " .. T("started", "пішло") or T("start failed ", "не стартувало ") .. tostring(code))
        drop("toggl:current")
      end)
      return nil
    end,
  }

  -- 15. Macros — named shell commands from ~/.config/orca-deck/macros.json, run in the active worktree.
  local MACROS_FILE = HOME .. "/.config/orca-deck/macros.json"
  local DEFAULT_MACROS = {
    { name = "todo.md → VS Code", cmd = "code .work/todo.md" },
    { name = "Finder", cmd = "open ." },
    { name = "WebStorm", cmd = "ws ." },
    { name = "git fetch", cmd = "git fetch --all --prune" },
  }
  local function macros()
    local ok, doc = pcall(hs.json.decode, ctx.readFile(MACROS_FILE) or "")
    local list = {}
    for _, m in ipairs(ok and type(doc) == "table" and doc or {}) do
      if type(m) == "table" and type(m.name) == "string" and type(m.cmd) == "string" then list[#list + 1] = m end
    end
    return #list > 0 and list or DEFAULT_MACROS, #list > 0
  end
  apps.macros = {
    name = { "Macros", "Макроси" },
    build = function()
      local list, own = macros()
      local items = {}
      for n, m in ipairs(list) do
        local r, badge = S.macroResult[m.name], nil
        if r then badge = r == 0 and "✓" or "✗ " .. r end
        items[#items + 1] = item(n, m.name, m.cmd, r == nil and C.violet or r == 0 and C.done or C.red, badge)
      end
      local w = active()
      return { h = title(w), s = #list .. T(" macros", " макросів") .. (own and "" or T(" (defaults)", " (стандартні)")),
        pg = { { n = T("macros", "макроси"), i = items, tp = 1, e = "", ht = T("tap ×2: run", "тап ×2: запуск") } } }
    end,
    act = function(action, msg)
      if action ~= "tap" then return nil end
      local list = macros()
      local m = list[tonumber(msg.key) or 0]
      local w = active()
      local cwd = w and fc.pathOf(w.id)
      if not m or not cwd then return nil end
      if not armed("macros", m.name, T("run ", "запустити ") .. m.name) then return nil end
      run("/bin/zsh", { "-lic", m.cmd }, function(code)
        S.macroResult[m.name] = code
        note("macros", (code == 0 and "✓ " or "✗ ") .. m.name)
      end, cwd)
      return nil
    end,
  }

  -- ======================================================================================================
  local self = {}

  --- GET /api/feeds: every app's feed. Builders that fail return an error feed, never break the rest.
  function self.all()
    local out = {}
    for _, id in ipairs(ORDER) do
      local ok, feed = pcall(apps[id].build)
      if not ok then
        trail("feed " .. id .. " failed: " .. tostring(feed))
        feed = { h = id, s = "error", pg = { { n = id, i = {}, e = fc.clip(tostring(feed), 80) } } }
      end
      feed.a = id
      local n = S.notes[id]
      if n then feed.m, feed.mt = n.t, n.n end
      local a = S.alerts[id]
      if a then feed.al, feed.an = a.t, a.n end
      out[#out + 1] = feed
    end
    return out
  end

  --- cmd "app": a gesture from a feed app. Returns a problem, or nil.
  function self.command(msg)
    if msg.app == "*" then
      if msg.action == "lang" then S.lang = msg.text == "en" and "en" or "ua" end
      return nil
    end
    local app = apps[msg.app]
    if not app then return "unknown app" end
    if msg.action == "open" then
      S.open, S.openAt = msg.app, now()
      return nil
    end
    local ok, problem = pcall(app.act, msg.action, msg)
    if not ok then
      trail("feed " .. msg.app .. " " .. tostring(msg.action) .. " crashed: " .. tostring(problem))
      return "error"
    end
    if problem then note(msg.app, "✗ " .. problem) end
    return problem
  end

  return self
end

return feeds

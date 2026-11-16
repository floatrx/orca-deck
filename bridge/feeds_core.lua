--- The pure part of the knob's feed apps (bridge/feeds.lua): todo.md items, marking and appending, git status,
--- numstat, context usage. No hs.* here, so bridge/test/ can test it.

local fc = {}

fc.QUEUE_MAX = 30
fc.TITLE_MAX = 90
fc.DETAIL_MAX = 160
fc.MARK = "→ віддано агенту"

local function trim(s) return (s or ""):match("^%s*(.-)%s*$") end

--- Cuts text to `limit` characters (UTF-8 aware), on one line.
function fc.clip(text, limit)
  if type(text) ~= "string" then return "" end
  text = trim(text:gsub("%s+", " "))
  local n = utf8.len(text)
  if n and n > limit then text = text:sub(1, utf8.offset(text, limit + 1) - 1) .. "…" end
  return text
end

--- Markdown inline noise off: `code`, **bold**, links.
local function plain(s)
  s = s:gsub("%*%*(.-)%*%*", "%1"):gsub("`([^`]*)`", "%1"):gsub("%[([^%]]*)%]%([^)]*%)", "%1")
  return trim(s)
end

--- Top-level open items of a todo.md: `- [ ] **Title** — goal` at column 0, in file order, at most `max`.
--- → { {line = n, title, detail, given = "HH:MM" | nil}, … }
function fc.todoItems(text, max)
  local items, n = {}, 0
  max = max or fc.QUEUE_MAX
  for line in ((text or "") .. "\n"):gmatch("([^\n]*)\n") do
    n = n + 1
    local rest = line:match("^[-*] %[ %] (.+)$")
    if rest and #items < max then
      local given = rest:match(fc.MARK .. " (%d%d?:%d%d)")
      rest = rest:gsub("%s*" .. fc.MARK .. "[^\n]*$", "")
      local title, after = rest:match("^%*%*(.-)%*%*%s*(.*)$")
      if not title then title, after = rest, "" end
      after = after:gsub("^[—–%-:]+%s*", "")
      items[#items + 1] = { line = n, title = fc.clip(plain(title), fc.TITLE_MAX), detail = fc.clip(plain(after), fc.DETAIL_MAX),
        given = given }
    end
  end
  return items
end

local function lines(text)
  local out = {}
  for line in ((text or "") .. "\n"):gmatch("([^\n]*)\n") do out[#out + 1] = line end
  if out[#out] == "" then out[#out] = nil end  -- the file's own trailing newline
  return out
end

--- Marks the item `title` (expected at `lineNo`; searched for when the file moved) as given to an agent at `stamp`.
--- Returns the new text, or nil and why.
function fc.markGiven(text, lineNo, title, stamp)
  local all = lines(text)
  local function matches(i)
    local items = fc.todoItems(all[i] or "", 1)
    return items[1] and items[1].title == title
  end
  local at = matches(lineNo) and lineNo or nil
  if not at then
    for i = 1, #all do if matches(i) then at = i break end end
  end
  if not at then return nil, "item not found" end
  all[at] = all[at]:gsub("%s*" .. fc.MARK .. ".*$", "") .. " " .. fc.MARK .. " " .. stamp
  return table.concat(all, "\n") .. "\n"
end

--- Appends `- [ ] **item**` at the end of a todo.md.
function fc.appendItem(text, item)
  item = fc.clip(item, 200):gsub("%*", "")
  if item == "" then return nil, "empty item" end
  text = text or ""
  if text ~= "" and text:sub(-1) ~= "\n" then text = text .. "\n" end
  return text .. "- [ ] **" .. item .. "**\n"
end

--- `git status --porcelain=v2 --branch` → {branch, upstream, ahead, behind, dirty}.
function fc.gitStatusOf(out)
  local s = { branch = "", upstream = nil, ahead = 0, behind = 0, dirty = 0 }
  for line in ((out or "") .. "\n"):gmatch("([^\n]*)\n") do
    local head = line:match("^# branch%.head (.+)$")
    if head then s.branch = head end
    local up = line:match("^# branch%.upstream (.+)$")
    if up then s.upstream = up end
    local a, b = line:match("^# branch%.ab %+(%d+) %-(%d+)$")
    if a then s.ahead, s.behind = tonumber(a), tonumber(b) end
    if line:match("^[12u?] ") then s.dirty = s.dirty + 1 end
  end
  return s
end

--- `git diff --numstat HEAD` (+ untracked paths) → { {path, add, del}, … }, biggest first.
function fc.numstatOf(out, untracked)
  local files = {}
  for line in ((out or "") .. "\n"):gmatch("([^\n]*)\n") do
    local a, d, path = line:match("^(%S+)\t(%S+)\t(.+)$")
    if path then files[#files + 1] = { path = path, add = tonumber(a) or 0, del = tonumber(d) or 0, binary = a == "-" } end
  end
  for line in ((untracked or "") .. "\n"):gmatch("([^\n]*)\n") do
    if line ~= "" then files[#files + 1] = { path = line, add = 0, del = 0, new = true } end
  end
  table.sort(files, function(x, y) return x.add + x.del > y.add + y.del end)
  return files
end

--- "357k" / "1M" / "980".
function fc.tokens(n)
  n = tonumber(n) or 0
  if n >= 1000000 then
    local m = n / 1000000
    return (m == math.floor(m) and string.format("%dM", m) or string.format("%.1fM", m))
  end
  if n >= 1000 then return string.format("%dk", math.floor(n / 1000 + 0.5)) end
  return tostring(math.floor(n))
end

--- The parent of a `--git-common-dir` (…/repo/.git → …/repo).
function fc.repoRootOf(commonDir)
  commonDir = trim(commonDir)
  if commonDir == "" then return nil end
  return (commonDir:gsub("/%.git/?$", ""))
end

--- The worktree path in a bridge worktree id (`<uuid>::<path>`).
function fc.pathOf(worktreeId)
  return type(worktreeId) == "string" and worktreeId:match("::(/.+)$") or nil
end

--- Splits a long answer into chunks of about `size` characters at sentence or word ends.
function fc.chunks(text, size)
  local out, current = {}, ""
  text = trim((text or ""):gsub("%s+", " "))
  for word in text:gmatch("%S+") do
    local next = current == "" and word or current .. " " .. word
    if (utf8.len(next) or #next) > size and current ~= "" then
      out[#out + 1] = current
      current = word
    else
      current = next
    end
    if current:match("[%.!?]$") and (utf8.len(current) or #current) > size * 0.6 then
      out[#out + 1] = current
      current = ""
    end
  end
  if current ~= "" then out[#out + 1] = current end
  return out
end

--- The minutes between two epoch ms values, at least 0.
function fc.minutes(fromMs, toMs)
  return math.max(0, math.floor(((toMs or 0) - (fromMs or 0)) / 60000))
end

--- "47 хв" / "2 год 05" / "4 дн" (Ukrainian) or "47 m" / "2 h 05" / "4 d".
function fc.duration(minutes, ua)
  if minutes >= 2880 then return (minutes // 1440) .. (ua and " дн" or " d") end
  if minutes < 60 then return minutes .. (ua and " хв" or " m") end
  return string.format(ua and "%d год %02d" or "%d h %02d", minutes // 60, minutes % 60)
end

--- An ISO-8601 UTC time ("2026-11-16T22:10:00Z" / "+00:00") → epoch ms; 0 when it does not parse.
function fc.isoMs(iso)
  local y, mo, d, h, mi, se = tostring(iso or ""):match("^(%d+)-(%d+)-(%d+)T(%d+):(%d+):(%d+)")
  if not y then return 0 end
  local t = { year = tonumber(y), month = tonumber(mo), day = tonumber(d), hour = tonumber(h), min = tonumber(mi), sec = tonumber(se) }
  local localTime = os.time(t)
  local utcOffset = os.time(os.date("*t", localTime)) - os.time(os.date("!*t", localTime))
  return (localTime + utcOffset) * 1000
end

return fc

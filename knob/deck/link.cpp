// The link layer: shared data, the command queue, the relay line protocol, and which link carries commands.
// Priority: BLE → USB → Wi-Fi. BLE and USB carry relay lines (knob/knob-bridge-relay.py): S state, K keepalive,
// D agent detail, A ack down; C command up. Wi-Fi talks HTTP to the bridge itself (link_wifi.cpp).
#include "deck.h"
#include <Preferences.h>
#include <sys/time.h>

SemaphoreHandle_t shared_mux;
Shared shared;

static volatile uint32_t seen_at[4];
static String sent_detail_for = "\x01";  // the want_detail last sent to a relay ("\x01" = never)
static LinkKind sent_detail_via = LINK_NONE;

const char *link_name(LinkKind kind)
{
  switch (kind) {
    case LINK_BLE: return "ble";
    case LINK_USB: return "usb";
    case LINK_WIFI: return "wifi";
    default: return "none";
  }
}

void link_seen(LinkKind kind) { seen_at[kind] = millis(); }
uint32_t link_seen_at(LinkKind kind) { return seen_at[kind]; }
bool link_alive(LinkKind kind) { return seen_at[kind] && millis() - seen_at[kind] < LINK_ALIVE_MS; }

void set_status(bool online, const String &text)
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.online = online;
  shared.status = text;
  xSemaphoreGive(shared_mux);
}

String status_problem()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool online = shared.online;
  String status = shared.status;
  xSemaphoreGive(shared_mux);
  if (!online || status.indexOf("failed") >= 0 || status.indexOf("dropped") >= 0) return status;
  return "";
}

static String one_line(const char *text)
{
  String s = text ? text : "";
  s.replace('\n', ' ');
  return s;
}

bool apply_state(const String &body, const String &via)
{
  JsonDocument filter;
  JsonObject wf = filter["worktrees"][0].to<JsonObject>();
  wf["id"] = wf["name"] = wf["repo"] = wf["active"] = true;
  JsonObject af = wf["agents"][0].to<JsonObject>();
  af["id"] = af["kind"] = af["chat"] = af["bucket"] = af["tool"] = af["last"] = af["prompt"] = af["since"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  std::vector<Worktree> fresh;
  for (JsonObject w : doc["worktrees"].as<JsonArray>()) {
    Worktree wt;
    wt.id = w["id"] | "";
    wt.repo = w["repo"] | "";
    wt.name = w["name"] | wt.repo.c_str();
    wt.active = w["active"] | false;
    for (JsonObject a : w["agents"].as<JsonArray>()) {
      Agent ag;
      ag.id = a["id"] | "";
      ag.kind = a["kind"] | "";
      ag.chat = a["chat"] | false;
      ag.bucket = a["bucket"] | "idle";
      ag.tool = a["tool"] | "";
      ag.last = one_line(a["last"] | "");
      ag.prompt = one_line(a["prompt"] | "");
      ag.since = a["since"] | (int64_t)0;
      wt.agents.push_back(ag);
    }
    fresh.push_back(wt);
  }

  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.list.swap(fresh);
  shared.version++;
  shared.online = true;
  shared.status = L("bridge online · ", "бридж на зв’язку · ") + via;
  xSemaphoreGive(shared_mux);
  return true;
}

bool apply_detail(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  AgentDetail d;
  d.id = doc["id"] | "";
  d.bucket = doc["bucket"] | "idle";
  d.message = doc["message"] | "";
  for (JsonVariant v : doc["next"].as<JsonArray>()) d.next.push_back(v.as<String>());
  JsonObject q = doc["question"];
  if (!q.isNull()) {
    d.has_question = true;
    d.answerable = q["answerable"] | false;
    d.item_id = q["itemId"] | "";
    d.revision = q["revision"] | 0;
    d.question = q["text"] | "";
    for (JsonObject o : q["options"].as<JsonArray>()) d.options.push_back({o["id"] | "", o["label"] | ""});
    d.group_answerable = q["groupAnswerable"] | false;
    for (JsonObject qq : q["questions"].as<JsonArray>()) {
      Question one;
      one.id = qq["id"] | "";
      one.text = qq["text"] | "";
      one.header = qq["header"] | "";
      one.multi = qq["multiSelect"] | false;
      for (JsonObject o : qq["options"].as<JsonArray>()) one.options.push_back({o["id"] | "", o["label"] | ""});
      d.questions.push_back(one);
    }
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  if (d.id == shared.want_detail) {
    shared.detail = d;
    shared.detail_version++;
  }
  xSemaphoreGive(shared_mux);
  return true;
}

// The relay's clock ("now", epoch ms, in K and S): with BLE or USB, Wi-Fi is off and NTP never runs, yet
// the ages ("· 9m") and Hide completed need the time.
static void sync_clock(const String &body)
{
  int at = body.lastIndexOf("\"now\":");
  if (at < 0) return;
  int64_t ms = atoll(body.c_str() + at + 6);
  if (ms < 1700000000000LL) return;
  struct timeval now;
  gettimeofday(&now, nullptr);
  int64_t mine = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
  if (llabs(mine - ms) < 2000) return;
  struct timeval set = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
  settimeofday(&set, nullptr);
}

static volatile int tz_min = 0;
static volatile bool tz_dirty = false;

// At boot, from the UI's setup: NVS reads touch flash too, and a lazy read from the BLE task (PSRAM stack) asserted.
void tz_load()
{
  Preferences prefs;
  prefs.begin("deck", true);
  tz_min = prefs.getInt("tz", 0);
  prefs.end();
}

int tz_offset_min() { return tz_min; }

static void apply_mac(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  MacStatus mac;
  mac.volume = doc["volume"] | -1;
  mac.battery = doc["battery"] | -1;
  mac.cpu = doc["cpu"] | -1;
  mac.muted = doc["muted"] | false;
  mac.charging = doc["charging"] | false;
  mac.output = doc["output"] | "";
  mac.source = doc["source"] | "";
  mac.title = doc["title"] | "";
  mac.claude_session = doc["cs"] | -1;
  mac.claude_week = doc["cw"] | -1;
  mac.session_reset_min = doc["csr"] | -1;
  mac.week_reset_min = doc["cwr"] | -1;
  mac.focus_error = doc["fe"] | "";
  mac.week_severity = doc["csv"] | "";
  mac.models = doc["cmod"] | "";
  mac.breakdown = doc["cbd"] | "";
  mac.mic = doc["mi"] | "";
  mac.mic_muted = doc["mm"] | false;
  mac.mic_volume = doc["mv"] | -1;
  mac.at = millis();
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.mac = mac;
  xSemaphoreGive(shared_mux);
  int tz = doc["tz"] | tz_offset_min();
  if (tz != tz_offset_min()) {
    tz_min = tz;
    tz_dirty = true;  // saved by the LVGL task: this one's stack is in PSRAM, and a flash write turns PSRAM off
  }
}

// From the LVGL task (internal-RAM stack) only: an NVS write from a link task asserted in cache_utils.c and the
// knob rebooted every 3 s.
void tz_persist()
{
  if (!tz_dirty) return;
  tz_dirty = false;
  Preferences prefs;
  prefs.begin("deck", false);
  prefs.putInt("tz", tz_min);
  prefs.end();
}

static void apply_prompts(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  std::vector<PromptGroup> groups;
  for (JsonArray g : doc["g"].as<JsonArray>()) {
    PromptGroup group;
    group.name = g[0] | "";
    for (JsonVariant p : g[1].as<JsonArray>()) group.prompts.push_back(p | "");
    if (group.prompts.size()) groups.push_back(group);
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.prompts = groups;
  shared.prompts_version++;
  xSemaphoreGive(shared_mux);
}

static void apply_prs(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  std::vector<PrItem> prs;
  for (JsonArray p : doc["p"].as<JsonArray>()) {
    PrItem pr;
    pr.key = p[0] | "";
    pr.title = p[1] | "";
    pr.checks = p[2] | "none";
    pr.mine = p[3] | true;
    pr.review = p[4] | "";
    pr.rerun = p[5] | false;
    prs.push_back(pr);
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.prs = prs;
  shared.prs_error = doc["e"] | "";
  shared.prs_loaded = doc["l"] | false;
  shared.prs_version++;
  xSemaphoreGive(shared_mux);
}

static void apply_feed(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  Feed f;
  f.id = doc["a"] | "";
  if (!f.id.length()) return;
  f.header = doc["h"] | "";
  f.sub = doc["s"] | "";
  f.note = doc["m"] | "";
  f.note_seq = doc["mt"] | 0;
  f.alert = doc["al"] | "";
  f.alert_seq = doc["an"] | 0;
  f.badge = doc["bd"] | 0;
  f.badge_color = doc["bc"] | C_ATTENTION;
  for (JsonObject p : doc["pg"].as<JsonArray>()) {
    FeedPage page;
    page.name = p["n"] | "";
    page.empty = p["e"] | "";
    page.hint = p["ht"] | "";
    page.to = p["to"] | "";
    page.hold = p["hd"] | 0;
    page.tap = (p["tp"] | 0) == 1;
    page.last = (p["last"] | 0) == 1;
    for (JsonObject i : p["i"].as<JsonArray>()) {
      FeedItem it;
      it.key = i["k"] | "";
      it.title = i["t"] | "";
      it.detail = i["d"] | "";
      it.badge = i["b"] | "";
      it.color = i["c"] | C_IDLE;
      page.items.push_back(it);
    }
    f.pages.push_back(page);
  }
  feed_put(f);
}

void feed_put(const Feed &given)
{
  Feed f = given;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool found = false;
  for (Feed &old : shared.feeds) {
    if (old.id == f.id) {
      f.version = old.version + 1;
      old = f;
      found = true;
    }
  }
  if (!found) {
    f.version = 1;
    shared.feeds.push_back(f);
  }
  xSemaphoreGive(shared_mux);
}

int feed_badge(const char *id, uint32_t &color)
{
  int n = 0;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  for (const Feed &f : shared.feeds) {
    if (f.id == id) {
      n = f.badge;
      color = f.badge_color;
    }
  }
  xSemaphoreGive(shared_mux);
  return n;
}

bool feed_take(const String &id, Feed &out)
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool found = false;
  for (const Feed &f : shared.feeds) {
    if (f.id == id) {
      out = f;
      found = true;
    }
  }
  xSemaphoreGive(shared_mux);
  return found;
}

static void apply_wifi(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  std::vector<WifiNet> nets;
  for (JsonArray n : doc["n"].as<JsonArray>()) nets.push_back({n[0] | "", n[1] | "", true});
  std::vector<String> lan;
  for (JsonVariant v : doc["lan"].as<JsonArray>()) lan.push_back(v.as<String>());
  wifi_store_set_mac(nets, lan);
}

static void apply_transcript(const String &body);
static void apply_day(const String &body);
static void apply_prompts(const String &body);
static void apply_prs(const String &body);
static void apply_wifi(const String &body);

void handle_line(const String &line, LinkKind via)
{
  if (line.length() < 2 || line[1] != ' ') {
    if (line.length()) Serial.printf("%s: %s\n", link_name(via), line.c_str());
    return;
  }
  char kind = line[0];
  String body = line.substring(2);
  if (kind == 'K') {
    link_seen(via);
    sync_clock(body);
  } else if (kind == 'S') {
    link_seen(via);
    sync_clock(body);
    apply_state(body, link_name(via));
  } else if (kind == 'D') {
    apply_detail(body);
  } else if (kind == 'M') {
    apply_mac(body);
  } else if (kind == 'X') {
    apply_transcript(body);
  } else if (kind == 'Y') {
    apply_day(body);
  } else if (kind == 'P') {
    apply_prompts(body);
  } else if (kind == 'R') {
    apply_prs(body);
  } else if (kind == 'G') {
    apply_feed(body);
  } else if (kind == 'W' && via != LINK_WIFI) {  // the networks ride only BLE / USB, never the air they configure
    apply_wifi(body);
  } else if (kind == 'T' && via == LINK_USB) {
    ui_test_enqueue(body);  // test input over the cable only: it needs physical access
  } else if (kind == 'A') {
    JsonDocument ack;
    if (!deserializeJson(ack, body)) {
      String cmd = ack["cmd"] | "?";
      String note = ack["note"] | "";
      bool ok = ack["ok"] | false;
      set_status(true, ok ? cmd + L(" ok", " ок") : cmd + L(" failed", " не вдалось") + (note.length() ? ": " + note : ""));
    }
  }
}

void push_voice(const String &line)
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  if (shared.voice.size() < 600) shared.voice.push_back(line);  // 19 s of audio waiting: the link is gone anyway
  xSemaphoreGive(shared_mux);
}

static void apply_day(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  DayData day;
  day.now = doc["now"] | -1;
  day.waited = doc["wa"] | 0;
  day.worked = doc["wo"] | 0;
  for (JsonArray row : doc["r"].as<JsonArray>()) {
    DayRow r;
    r.name = row[0] | "";
    for (JsonArray s : row[1].as<JsonArray>()) r.spans.push_back({(int16_t)(s[0] | 0), (int16_t)(s[1] | 0), (s[2] | 0) == 1});
    day.rows.push_back(r);
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.day = day;
  shared.day_version++;
  xSemaphoreGive(shared_mux);
}

static void apply_transcript(const String &body)
{
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.transcript = doc["text"] | "";
  shared.transcript_error = doc["error"] | "";
  shared.transcript_version++;
  xSemaphoreGive(shared_mux);
}

void push_command(JsonDocument &body)
{
  Command c;
  c.kind = body["cmd"] | "?";
  serializeJson(body, c.body);
  c.queued_at = millis();
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  // Only the newest `go` matters: the crown's last choice.
  if (c.kind == "go") {
    for (auto it = shared.commands.begin(); it != shared.commands.end();) {
      it = it->kind == "go" ? shared.commands.erase(it) : it + 1;
    }
  }
  shared.commands.push_back(c);
  xSemaphoreGive(shared_mux);
  Serial.printf("cmd queued: %s\n", c.kind.c_str());
}

void push_simple(const char *cmd, const String &id, const String &pane)
{
  JsonDocument body;
  body["cmd"] = cmd;
  if (id.length()) body["id"] = id;
  if (pane.length()) body["pane"] = pane;
  push_command(body);
}

bool pop_command(Command &out)
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool has = !shared.commands.empty();
  if (has) {
    out = shared.commands.front();
    shared.commands.pop_front();
  }
  xSemaphoreGive(shared_mux);
  return has;
}

void drop_stale_commands()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  while (!shared.commands.empty() && millis() - shared.commands.front().queued_at > RETRY_FOR_MS) {
    shared.commands.pop_front();
  }
  xSemaphoreGive(shared_mux);
}

// The relay polls /api/agent for the knob; it needs to hear which agent, once per change and per link.
String detail_request()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  String want = shared.want_detail;
  LinkKind via = shared.link;
  xSemaphoreGive(shared_mux);
  if (want == sent_detail_for && via == sent_detail_via) return "";
  sent_detail_for = want;
  sent_detail_via = via;
  JsonDocument body;
  body["cmd"] = "detail";
  body["id"] = want;
  String json;
  serializeJson(body, json);
  return json;
}

// Sends queued commands over the best relay link. While neither BLE nor USB is alive, Wi-Fi takes them.
static void dispatch_task(void *)
{
  uint32_t last_report = 0;
  for (;;) {
    if (millis() - last_report > 10000) {
      last_report = millis();
      auto age = [](LinkKind k) { return seen_at[k] ? (long)(millis() - seen_at[k]) : -1L; };
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      LinkKind active = shared.link;
      xSemaphoreGive(shared_mux);
      Serial.printf("link: %s · last line ble %ld ms, usb %ld ms ago · clock %ld\n", link_name(active), age(LINK_BLE),
                    age(LINK_USB), (long)time(nullptr));
    }
    LinkKind link = link_alive(LINK_BLE) ? LINK_BLE : link_alive(LINK_USB) ? LINK_USB : LINK_NONE;
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    bool wifi_live = shared.link == LINK_WIFI && link == LINK_NONE;
    if (link != LINK_NONE) shared.link = link;
    else if (shared.link != LINK_WIFI) shared.link = LINK_NONE;
    xSemaphoreGive(shared_mux);
    if (link == LINK_NONE) {
      if (!wifi_live) drop_stale_commands();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    // Dictation audio first, as fast as the link takes it: a full BLE buffer is retried, not dropped.
    for (int n = 0; n < 8; n++) {
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      bool any = !shared.voice.empty();
      String v = any ? shared.voice.front() : String();
      xSemaphoreGive(shared_mux);
      if (!any) break;
      bool sent = link == LINK_BLE ? ble_send_line(v) : (serial_send_line(v), true);
      if (!sent) {
        vTaskDelay(pdMS_TO_TICKS(10));
        break;
      }
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      if (!shared.voice.empty()) shared.voice.pop_front();
      xSemaphoreGive(shared_mux);
    }
    static bool boot_told = false;  // once a boot, to the relay's log (any non-C line is logged there)
    if (!boot_told && link == LINK_BLE) boot_told = ble_send_line("L " + boot_note + " · heap " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    static uint32_t bat_told = 0;  // the battery probe: a line a minute to the relay's log
    if (link == LINK_BLE && millis() - bat_told > 60000) {
      bat_told = millis();
      ble_send_line("L " + battery_report());
    }
    String detail = detail_request();
    if (detail.length()) {
      if (link == LINK_BLE) ble_send_line("C " + detail);
      else serial_send_line("C " + detail);
    }
    Command c;
    if (pop_command(c)) {
      bool sent = link == LINK_BLE ? ble_send_line("C " + c.body) : (serial_send_line("C " + c.body), true);
      Serial.printf("cmd %s: %s via %s\n", c.kind.c_str(), sent ? "sent" : "NOT sent", link_name(link));
      if (!sent) {
        xSemaphoreTake(shared_mux, portMAX_DELAY);
        shared.commands.push_front(c);  // retried while young; drop_stale_commands ages it out
        xSemaphoreGive(shared_mux);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void link_init()
{
  if (!shared_mux) shared_mux = xSemaphoreCreateMutex();  // ui_init made it first
  serial_link_start();
  ble_link_start();
  wifi_link_start();
  xTaskCreatePinnedToCoreWithCaps(dispatch_task, "dispatch", 6 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

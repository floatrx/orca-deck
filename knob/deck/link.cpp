// The link layer: shared data, the command queue, the relay line protocol, and which link carries commands.
// Priority: BLE → USB → Wi-Fi. BLE and USB carry relay lines (knob/relay.py): S state, K keepalive,
// D agent detail, A ack down; C command up. Wi-Fi talks HTTP to the bridge itself (link_wifi.cpp).
#include "deck.h"
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
  shared.status = "bridge online · " + via;
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
  } else if (kind == 'T' && via == LINK_USB) {
    ui_test_enqueue(body);  // test input over the cable only: it needs physical access
  } else if (kind == 'A') {
    JsonDocument ack;
    if (!deserializeJson(ack, body)) {
      String cmd = ack["cmd"] | "?";
      String note = ack["note"] | "";
      bool ok = ack["ok"] | false;
      set_status(true, ok ? cmd + " ok" : cmd + " failed" + (note.length() ? ": " + note : ""));
    }
  }
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
  shared_mux = xSemaphoreCreateMutex();
  serial_link_start();
  ble_link_start();
  wifi_link_start();
  xTaskCreatePinnedToCoreWithCaps(dispatch_task, "dispatch", 6 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

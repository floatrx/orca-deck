// orca-deck knob: the spike panel (.work/spike/orca-deck/index.html) as firmware.
// crown → workspace (Orca follows after 1 s of rest) · tap → enter · hold → next agent ·
// double tap → send · swipe right → next. The network task talks to the bridge; the LVGL task owns the widgets.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <deque>
#include <map>
#include <vector>
#include <math.h>
#include <time.h>
#include "lvgl.h"
#include "driver/i2c.h"
#include "bidi_switch_knob.h"
#include "secrets.h"

LV_FONT_DECLARE(deck_14);
LV_FONT_DECLARE(deck_16);
LV_FONT_DECLARE(deck_bold_16);
LV_FONT_DECLARE(deck_bold_30);

#define KNOB_PIN_A 8
#define KNOB_PIN_B 7

#define DRV2605_ADDR 0x5A
#define HAPTIC_TICK 24   // "Sharp Tick 1"
#define HAPTIC_CLICK 1   // "Strong Click 100%"
#define HAPTIC_BUZZ 47   // "Buzz 1 100%"

// Same timings as the spike.
#define POLL_MS 1000
#define COMMIT_MS 1000       // the crown rests this long before Orca is told to switch
#define GO_PATIENCE_MS 4000  // how long the screen keeps the choice while Orca gets there
#define DOUBLE_TAP_MS 260    // a tap waits this long to see whether it becomes a double tap
#define RETRY_FOR_MS 5000    // an undelivered command is retried this long; later it would surprise

#define MAX_DOTS 48
#define RING_RADIUS 163

static const uint32_t C_WORKING = 0x4CC3FF, C_ATTENTION = 0xFF7A1A, C_DONE = 0x3DDC84, C_IDLE = 0x5D5B57;
static const uint32_t C_INK = 0xF1EFE9, C_MUTED = 0x8A8780, C_DETAIL = 0xC9C6BF;

struct Agent {
  String id, kind, bucket, tool, last, prompt;
  bool chat = false;
  int64_t since = 0;
};

struct Worktree {
  String id, name, repo;
  bool active = false;
  std::vector<Agent> agents;
};

struct Command {
  String cmd, id, pane;
  uint32_t queued_at = 0;
  int attempts = 0;
};

// Shared between the tasks, guarded by `shared_mux`.
static SemaphoreHandle_t shared_mux;
static std::vector<Worktree> shared_list;
static uint32_t shared_version = 0;
static bool shared_online = false;
static String shared_status = "wifi…";
static std::deque<Command> commands;

// LVGL-task state.
static std::vector<Worktree> worktrees;
static uint32_t seen_version = 0;
static int active_index = 0;
static String chosen_id;
static uint32_t commit_at = 0, chosen_until = 0, single_tap_at = 0;
static std::map<String, String> picked_agent;  // worktree id → agent picked by holding
static std::map<String, String> last_bucket;   // agent id → bucket, for the done / needs-you buzz
static volatile int pending_steps = 0;

static lv_obj_t *dots[MAX_DOTS];
static int dot_count = 0;
static String dot_ids;
static lv_obj_t *halo, *summary, *state_label, *name_label, *meta_label, *detail_label, *counter_label, *status_label;

// ---- haptics (DRV2605 on the legacy I2C driver that Touch_Init installs) ----

static void drv_write(uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = {reg, val};
  i2c_master_write_to_device(I2C_NUM_0, DRV2605_ADDR, buf, 2, pdMS_TO_TICKS(20));
}

static void haptic_init()
{
  drv_write(0x01, 0x00);  // MODE: out of standby, internal trigger
  drv_write(0x1A, 0xB6);  // FEEDBACK: LRA
  drv_write(0x03, 0x06);  // LIBRARY: LRA
  drv_write(0x1D, 0xA0);  // CONTROL3: closed-loop LRA defaults
}

// Plays `effect` `times` times, 250 ms apart (a waveform-sequencer wait between them).
static void haptic(uint8_t effect, int times = 1)
{
  uint8_t reg = 0x04;
  for (int k = 0; k < times && reg < 0x0B; k++) {
    if (k) drv_write(reg++, 0x80 | 25);  // wait 25 × 10 ms
    drv_write(reg++, effect);
  }
  drv_write(reg, 0x00);
  drv_write(0x0C, 0x01);  // GO
}

// ---- network task ----

static void set_status(bool online, const String &text)
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared_online = online;
  shared_status = text;
  xSemaphoreGive(shared_mux);
}

static String one_line(const char *text)
{
  String s = text ? text : "";
  s.replace('\n', ' ');
  return s;
}

// A weak signal breaks single polls (a refused connect, a body cut short). The screen keeps the last good
// state and reports a problem only after this many failures in a row.
#define FAILS_TO_REPORT 3
static int fails_in_row = 0;

static void poll_failed(const String &why)
{
  Serial.printf("poll failed: %s\n", why.c_str());
  if (++fails_in_row >= FAILS_TO_REPORT) set_status(false, why);
}

static bool apply_state(const String &body, const String &via);

static void fetch_state()
{
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding: the body length is known up front
  http.setTimeout(5000);
  http.begin(String(BRIDGE_URL) + "/api/state");
  http.addHeader("Authorization", String("Bearer ") + BRIDGE_TOKEN);
  int code = http.GET();
  if (code != 200) {
    http.end();
    poll_failed(code > 0 ? "bridge HTTP " + String(code) : "bridge offline (" + String(code) + ")");
    return;
  }
  // Read the whole body first: parsing straight from the socket gave up on a slow, lossy link.
  int expected = http.getSize();
  String body = http.getString();
  http.end();
  if (expected > 0 && (int)body.length() != expected) {
    poll_failed("bridge: body cut short " + String(body.length()) + "/" + String(expected));
    return;
  }
  if (apply_state(body, "wifi " + String(WiFi.RSSI()) + " dBm")) fails_in_row = 0;
}

// Parses a state document (from HTTP or from the USB relay) into the shared list.
static bool apply_state(const String &body, const String &via)
{
  JsonDocument filter;
  JsonObject wf = filter["worktrees"][0].to<JsonObject>();
  wf["id"] = wf["name"] = wf["repo"] = wf["active"] = true;
  JsonObject af = wf["agents"][0].to<JsonObject>();
  af["id"] = af["kind"] = af["chat"] = af["bucket"] = af["tool"] = af["last"] = af["prompt"] = af["since"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    poll_failed(String("bad json: ") + err.c_str());
    return false;
  }

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
  shared_list = fresh;
  shared_version++;
  shared_online = true;
  shared_status = "bridge online · " + via;
  xSemaphoreGive(shared_mux);
  return true;
}

// ---- USB link (knob/relay.py on the Mac) ----
// The relay sends "S <state>" every ≤ 2 s and "A <ack>" after each command; the knob sends "C <command>".
#define USB_ALIVE_MS 3000
static volatile uint32_t usb_seen_at = 0;

static bool usb_alive()
{
  return usb_seen_at && millis() - usb_seen_at < USB_ALIVE_MS;
}

static void usb_send_command(const Command &c)
{
  JsonDocument body;
  body["cmd"] = c.cmd;
  if (c.id.length()) body["id"] = c.id;
  if (c.pane.length()) body["pane"] = c.pane;
  String json;
  serializeJson(body, json);  // serializeJson replaces a String's content, so the prefix is added after
  String line = "C " + json + "\n";
  Serial.write((const uint8_t *)line.c_str(), line.length());  // one write, so a log line cannot split it
}

static void usb_task(void *)
{
  String line;
  for (;;) {
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch != '\n') {
        line += ch;
        continue;
      }
      if (line.startsWith("S ")) {
        usb_seen_at = millis();
        if (apply_state(line.substring(2), "usb")) fails_in_row = 0;
      } else if (line.startsWith("A ")) {
        JsonDocument ack;
        if (!deserializeJson(ack, line.substring(2))) {
          String cmd = ack["cmd"] | "?";
          String note = ack["note"] | "";
          bool ok = ack["ok"] | false;
          set_status(true, ok ? cmd + " ok" : cmd + " failed" + (note.length() ? ": " + note : ""));
        }
      }
      line = "";
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// HTTPClient codes that mean the request never reached the bridge, so sending it again cannot repeat it.
// A read timeout (-11) or a lost connection (-5) may come after the bridge acted: those are not retried,
// or a double tap could press Enter twice.
static bool undelivered(int code)
{
  return code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
         code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED;
}

// Returns false when the command did not reach the bridge and may be sent again.
static bool send_command(const Command &c)
{
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String(BRIDGE_URL) + "/api/cmd");
  http.addHeader("Authorization", String("Bearer ") + BRIDGE_TOKEN);
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["cmd"] = c.cmd;
  if (c.id.length()) body["id"] = c.id;
  if (c.pane.length()) body["pane"] = c.pane;
  String text;
  serializeJson(body, text);
  int code = http.POST(text);
  Serial.printf("cmd %s → HTTP %d\n", text.c_str(), code);
  String note;
  bool ok = false;
  if (code == 200) {
    JsonDocument ack;
    if (!deserializeJson(ack, http.getString())) {
      ok = ack["ok"] | false;
      note = ack["note"] | "";
    }
  }
  http.end();
  if (undelivered(code)) return false;
  set_status(code == 200, ok ? c.cmd + " ok" : c.cmd + " failed" + (code != 200 ? " HTTP " + String(code) : "") +
                                (note.length() ? ": " + note : ""));
  return true;
}

// Puts an undelivered command back at the front, unless it is too old or a newer `go` replaced it.
static void retry_later(Command c)
{
  c.attempts++;
  bool stale = millis() - c.queued_at > RETRY_FOR_MS;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool superseded = false;
  for (const Command &queued : commands) {
    if (c.cmd == "go" && queued.cmd == "go") superseded = true;
  }
  if (!stale && !superseded) commands.push_front(c);
  xSemaphoreGive(shared_mux);
  Serial.printf("cmd %s: undelivered, %s\n", c.cmd.c_str(), stale ? "dropped (stale)" : superseded ? "dropped (newer go)" : "will retry");
  if (stale) set_status(false, c.cmd + " dropped: bridge unreachable");
}

// Why the last Wi-Fi attempt failed, as the station saw it (0 = none yet).
static volatile int wifi_reason = 0;
// Set after the first join: the password is then known to be right, and a handshake failure means signal.
static bool ever_joined = false;
static int last_rssi = 0;

static String wifi_problem()
{
  if (ever_joined && wifi_reason) return "wifi: weak signal (" + String(last_rssi) + " dBm) · rejoining";
  switch (wifi_reason) {
    case 0: return "wifi…";
    case WIFI_REASON_NO_AP_FOUND: return "wifi: network not found on 2.4 GHz";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wifi: wrong password?";
    default: return "wifi: reason " + String(wifi_reason);
  }
}

// The ESP32-S3 sees 2.4 GHz only. A dual-band router often names its 5 GHz network "<name>_5G" (or -5G,
// " 5GHz"); the 2.4 GHz twin is then "<name>" with the same password.
static String band_twin(const String &ssid)
{
  String low = ssid;
  low.toLowerCase();
  for (const char *tail : {"_5ghz", "-5ghz", " 5ghz", "5ghz", "_5g", "-5g", " 5g", "5g"}) {
    if (low.endsWith(tail)) return ssid.substring(0, ssid.length() - strlen(tail));
  }
  return "";
}

// Picks the network to join: the configured SSID if the scan sees it, else its 2.4 GHz twin.
static String pick_ssid()
{
  set_status(false, "wifi: scanning…");
  String twin = band_twin(WIFI_SSID);
  int n = WiFi.scanNetworks();
  bool seen = false, twin_seen = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) seen = true;
    if (twin.length() && WiFi.SSID(i) == twin) twin_seen = true;
  }
  WiFi.scanDelete();
  Serial.printf("wifi: %d networks, configured %s, twin %s\n", n, seen ? "seen" : "not seen",
                twin_seen ? "seen" : "not seen");
  if (seen || !twin_seen) return WIFI_SSID;
  return twin;
}

static void network_task(void *)
{
  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    int reason = info.wifi_sta_disconnected.reason;
    if (reason != WIFI_REASON_STA_LEAVING && reason != WIFI_REASON_ASSOC_LEAVE) wifi_reason = reason;
    Serial.printf("wifi: disconnected, reason %d\n", reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
    Serial.printf("wifi: connected, ip %s\n", WiFi.localIP().toString().c_str());
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.setSleep(false);  // modem power-save drops the link on a weak 2.4 GHz signal
  WiFi.setAutoReconnect(true);
  String ssid = pick_ssid();
  WiFi.begin(ssid.c_str(), WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.apple.com");
  uint32_t last_poll = 0, joined_at = 0, lost_at = 0;
  for (;;) {
    if (usb_alive()) {
      // The cable carries state and commands; Wi-Fi keeps joining in the background as the fallback.
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      bool has = !commands.empty();
      Command c;
      if (has) {
        c = commands.front();
        commands.pop_front();
      }
      xSemaphoreGive(shared_mux);
      if (has) usb_send_command(c);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      // Commands wait for the link only RETRY_FOR_MS: an old `go` after a long outage would surprise.
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      while (!commands.empty() && millis() - commands.front().queued_at > RETRY_FOR_MS) commands.pop_front();
      xSemaphoreGive(shared_mux);
      // The stack's own reconnect can stall after a handshake timeout: start over after 20 s.
      // Sooner, it collides with the stack's own attempt ("sta is connecting, cannot set config").
      if (!lost_at) lost_at = millis();
      if (millis() - lost_at > 20000) {
        Serial.println("wifi: restarting the join");
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        WiFi.begin(ssid.c_str(), WIFI_PASS);
        lost_at = millis();
      }
      String problem = wifi_problem();
      if (ssid != WIFI_SSID) problem += " · " + ssid;
      set_status(false, strlen(WIFI_PASS) ? problem : "no wifi password");
      joined_at = 0;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    lost_at = 0;
    last_rssi = WiFi.RSSI();
    if (!joined_at) {
      joined_at = millis();
      ever_joined = true;
      wifi_reason = 0;
      set_status(false, "wifi: " + ssid + " · " + String(WiFi.RSSI()) + " dBm");
      Serial.printf("wifi: joined %s, ip %s, rssi %d\n", ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      vTaskDelay(pdMS_TO_TICKS(1500));  // let the joined network show for a moment
    }
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    bool has = !commands.empty();
    Command c;
    if (has) {
      c = commands.front();
      commands.pop_front();
    }
    xSemaphoreGive(shared_mux);
    if (has) {
      if (send_command(c)) {
        last_poll = millis() - POLL_MS + 250;  // show where Orca went soon after
      } else {
        retry_later(c);
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      continue;
    }
    if (millis() - last_poll >= POLL_MS) {
      last_poll = millis();
      fetch_state();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void push(const String &cmd, const String &id = "", const String &pane = "")
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  Command c;
  c.cmd = cmd;
  c.id = id;
  c.pane = pane;
  c.queued_at = millis();
  commands.push_back(c);
  xSemaphoreGive(shared_mux);
}

// ---- spike logic (LVGL task) ----

static int urgency(const String &bucket)
{
  if (bucket == "attention") return 3;
  if (bucket == "working") return 2;
  if (bucket == "done") return 1;
  return 0;
}

static uint32_t color_of(const String &bucket)
{
  if (bucket == "attention") return C_ATTENTION;
  if (bucket == "working") return C_WORKING;
  if (bucket == "done") return C_DONE;
  return C_IDLE;
}

static const Agent *lead_agent(const Worktree &w)
{
  const Agent *best = nullptr;
  for (const Agent &a : w.agents) {
    if (!best || urgency(a.bucket) > urgency(best->bucket)) best = &a;
  }
  return best;
}

static const Agent *shown_agent(const Worktree &w)
{
  auto it = picked_agent.find(w.id);
  if (it != picked_agent.end()) {
    for (const Agent &a : w.agents) {
      if (a.id == it->second) return &a;
    }
  }
  return lead_agent(w);
}

static String ago(int64_t since)
{
  time_t now = time(nullptr);
  if (!since || now < 1700000000) return "";
  int64_t s = (int64_t)now - since / 1000;
  if (s < 0) s = 0;
  if (s < 60) return String((int)s) + "s";
  if (s < 3600) return String((int)lround(s / 60.0)) + "m";
  if (s < 86400) return String((int)lround(s / 3600.0)) + "h";
  return String((int)lround(s / 86400.0)) + "d";
}

static String upper(String s)
{
  s.toUpperCase();
  return s;
}

static void build_dots()
{
  String ids;
  for (const Worktree &w : worktrees) ids += w.id + "|";
  if (ids == dot_ids) return;
  dot_ids = ids;
  int n = min((int)worktrees.size(), MAX_DOTS);
  for (int i = 0; i < MAX_DOTS; i++) {
    if (i < n && !dots[i]) {
      dots[i] = lv_obj_create(lv_scr_act());
      lv_obj_clear_flag(dots[i], (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
      lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_pad_all(dots[i], 0, 0);
    }
    if (dots[i]) {
      if (i < n) lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  dot_count = n;
}

static void set_text(lv_obj_t *label, const String &text)
{
  if (strcmp(lv_label_get_text(label), text.c_str()) != 0) lv_label_set_text(label, text.c_str());
}

static void render()
{
  int n = worktrees.size();
  int attention = 0, working = 0, done = 0;
  for (const Worktree &w : worktrees) {
    for (const Agent &a : w.agents) {
      if (a.bucket == "attention") attention++;
      else if (a.bucket == "working") working++;
      else if (a.bucket == "done") done++;
    }
  }
  String sum;
  if (attention) sum += String(attention) + " WAITING";
  if (working) sum += String(sum.length() ? " · " : "") + working + " WORKING";
  if (done) sum += String(sum.length() ? " · " : "") + done + " DONE";
  set_text(summary, sum.length() ? sum : (n ? "ALL QUIET" : "NO WORKSPACES"));

  build_dots();
  for (int i = 0; i < dot_count; i++) {
    // The dial turns so the chosen workspace sits at 12 o'clock.
    float angle = (float)(i - active_index) / dot_count * 2 * M_PI - M_PI / 2;
    const Worktree &w = worktrees[i];
    const Agent *lead = lead_agent(w);
    String bucket = lead ? lead->bucket : "idle";
    bool sel = i == active_index;
    int size = sel ? 18 : 10;
    lv_obj_set_size(dots[i], size, size);
    lv_obj_set_pos(dots[i], 180 + RING_RADIUS * cosf(angle) - size / 2, 180 + RING_RADIUS * sinf(angle) - size / 2);
    lv_color_t c = lv_color_hex(color_of(bucket));
    lv_obj_set_style_bg_color(dots[i], c, 0);
    lv_obj_set_style_bg_opa(dots[i], w.agents.empty() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dots[i], w.agents.empty() ? lv_color_hex(0x444444) : c, 0);
    lv_obj_set_style_border_width(dots[i], 2, 0);
    lv_obj_set_style_shadow_width(dots[i], sel ? 14 : 0, 0);
    lv_obj_set_style_shadow_color(dots[i], c, 0);
  }

  if (!n) {
    set_text(state_label, "");
    set_text(name_label, "orca-deck");
    set_text(meta_label, "");
    set_text(detail_label, "");
    set_text(counter_label, "");
    return;
  }

  const Worktree &w = worktrees[active_index];
  const Agent *a = shown_agent(w);
  String bucket = a ? a->bucket : "idle";
  lv_color_t c = lv_color_hex(color_of(bucket));
  lv_obj_set_style_arc_color(halo, c, LV_PART_MAIN);
  lv_obj_set_style_text_color(state_label, c, 0);
  set_text(state_label, !a ? "NO AGENTS" : bucket == "attention" ? "NEEDS YOU" : upper(bucket));
  set_text(name_label, w.name);

  String meta;
  if (w.repo.length() && w.repo != w.name) meta = w.repo;
  if (a) {
    meta += String(meta.length() ? " · " : "") + (a->chat ? "chat" : "terminal");
    if (a->kind.length()) meta += " · " + a->kind;
  }
  set_text(meta_label, meta);

  String detail;
  if (a) {
    if (bucket == "working" && a->tool.length()) {
      String tool = a->tool;
      int cut = tool.lastIndexOf("__");
      if (cut >= 0) tool = tool.substring(cut + 2);
      detail = "→ " + tool + (a->last.length() ? " — " + a->last : "");
    } else {
      detail = a->last.length() ? a->last : a->prompt;
    }
  }
  set_text(detail_label, detail);

  String counter = String(active_index + 1) + "/" + n;
  if (w.agents.size() > 1 && a) {
    int at = 0;
    for (size_t k = 0; k < w.agents.size(); k++) {
      if (w.agents[k].id == a->id) at = k;
    }
    counter += " · agent " + String(at + 1) + "/" + String((int)w.agents.size());
  }
  if (a) {
    String age = ago(a->since);
    if (age.length()) counter += " · " + age;
  }
  set_text(counter_label, counter);
}

static void on_state(std::vector<Worktree> &list)
{
  worktrees.swap(list);
  int pulses = 0;
  for (const Worktree &w : worktrees) {
    for (const Agent &a : w.agents) {
      auto it = last_bucket.find(a.id);
      if (it != last_bucket.end() && it->second != a.bucket) {
        if (a.bucket == "attention") pulses = 2;
        else if (a.bucket == "done" && it->second == "working") pulses = max(pulses, 1);
      }
      last_bucket[a.id] = a.bucket;
    }
  }
  int active = -1, chosen = -1;
  for (int i = 0; i < (int)worktrees.size(); i++) {
    if (worktrees[i].active) active = i;
    if (chosen_id.length() && worktrees[i].id == chosen_id) chosen = i;
  }
  if (chosen >= 0 && (commit_at || millis() < chosen_until) && active != chosen) {
    active_index = chosen;  // still turning, or Orca is on its way there: keep the knob's choice
  } else {
    chosen_id = "";
    active_index = active >= 0 ? active : min(active_index, max(0, (int)worktrees.size() - 1));
  }
  render();
  if (pulses) haptic(HAPTIC_BUZZ, pulses);
}

// Sends the knob's choice to Orca. The screen holds that choice while Orca gets there.
static void commit()
{
  commit_at = 0;
  if (worktrees.empty()) return;
  const Worktree &w = worktrees[active_index];
  const Agent *a = shown_agent(w);
  chosen_until = millis() + GO_PATIENCE_MS;
  Serial.printf("commit → %s\n", w.name.c_str());
  push("go", w.id, a ? a->id : "");
}

static void turn(int step)
{
  if (worktrees.empty()) return;
  haptic(HAPTIC_TICK);
  int n = worktrees.size();
  active_index = ((active_index + step) % n + n) % n;
  chosen_id = worktrees[active_index].id;
  commit_at = millis() + COMMIT_MS;
  render();
}

// Hold: the next agent of the workspace on screen, shown in Orca right away.
static void next_agent()
{
  if (worktrees.empty()) return;
  const Worktree &w = worktrees[active_index];
  if (w.agents.size() < 2) {
    haptic(HAPTIC_BUZZ);
    return;
  }
  const Agent *a = shown_agent(w);
  size_t at = 0;
  for (size_t k = 0; k < w.agents.size(); k++) {
    if (a && w.agents[k].id == a->id) at = k;
  }
  picked_agent[w.id] = w.agents[(at + 1) % w.agents.size()].id;
  chosen_id = w.id;
  haptic(HAPTIC_CLICK);
  render();
  commit();
}

static void on_short_click(lv_event_t *)
{
  if (single_tap_at) {
    single_tap_at = 0;  // the second tap of a double tap
    haptic(HAPTIC_CLICK, 2);
    push("send");
    return;
  }
  single_tap_at = millis() + DOUBLE_TAP_MS;
}

static void on_long_press(lv_event_t *)
{
  next_agent();
}

static void on_gesture(lv_event_t *)
{
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_RIGHT) return;
  single_tap_at = 0;
  haptic(HAPTIC_CLICK);
  push("next");
}

static void knob_left(void *, void *) { pending_steps--; }
static void knob_right(void *, void *) { pending_steps++; }

static void loop_tick(lv_timer_t *)
{
  static uint32_t last_render = 0;
  std::vector<Worktree> fresh;
  bool got = false;

  xSemaphoreTake(shared_mux, portMAX_DELAY);
  if (shared_version != seen_version) {
    seen_version = shared_version;
    fresh = shared_list;
    got = true;
  }
  bool online = shared_online;
  String status = shared_status;
  xSemaphoreGive(shared_mux);

  if (got) on_state(fresh);

  int steps = pending_steps;
  if (steps) {
    pending_steps -= steps;
    int dir = steps > 0 ? 1 : -1;
    for (int k = 0; k != steps; k += dir) turn(dir);
  }

  uint32_t now = millis();
  if (commit_at && now >= commit_at) commit();
  if (single_tap_at && now >= single_tap_at) {
    single_tap_at = 0;
    if (commit_at) commit();
    haptic(HAPTIC_CLICK);
    push("enter");
  }
  if (now - last_render >= 1000) {
    last_render = now;
    render();  // keeps the "9m" ages fresh
  }

  set_text(status_label, "• " + status);
  lv_obj_set_style_text_color(status_label, lv_color_hex(online ? C_DONE : C_ATTENTION), 0);
}

static lv_obj_t *make_label(const lv_font_t *font, uint32_t color, int y, int width = 236)
{
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_label_set_text(label, "");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
  return label;
}

extern "C" void ui_init(void)
{
  shared_mux = xSemaphoreCreateMutex();

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x050506), 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(screen, on_short_click, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(screen, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);

  // The halo: a thin ring at the edge in the colour of the agent on screen.
  halo = lv_arc_create(screen);
  lv_obj_set_size(halo, 356, 356);
  lv_obj_center(halo);
  lv_arc_set_bg_angles(halo, 0, 360);
  lv_obj_remove_style(halo, NULL, LV_PART_KNOB);
  lv_obj_remove_style(halo, NULL, LV_PART_INDICATOR);
  lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(halo, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(halo, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_color(halo, lv_color_hex(C_IDLE), LV_PART_MAIN);

  summary = make_label(&deck_14, C_MUTED, -96, 220);
  state_label = make_label(&deck_bold_16, C_IDLE, -70);
  name_label = make_label(&deck_bold_30, C_INK, -40, 250);
  meta_label = make_label(&deck_14, C_MUTED, -10);
  detail_label = make_label(&deck_16, C_DETAIL, 30, 240);
  lv_obj_set_height(detail_label, 42);  // two lines, then an ellipsis
  counter_label = make_label(&deck_14, C_MUTED, 72);
  status_label = make_label(&deck_14, C_MUTED, 112, 200);
  lv_obj_set_style_text_letter_space(summary, 1, 0);
  lv_obj_set_style_text_letter_space(state_label, 2, 0);
  set_text(name_label, "orca-deck");

  haptic_init();
  knob_config_t cfg = {
    .gpio_encoder_a = KNOB_PIN_A,
    .gpio_encoder_b = KNOB_PIN_B,
  };
  knob_handle_t knob = iot_knob_create(&cfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right, NULL);
  }
  lv_timer_create(loop_tick, 16, NULL);
  xTaskCreatePinnedToCore(network_task, "net", 16 * 1024, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(usb_task, "usb", 16 * 1024, NULL, 1, NULL, 0);
}

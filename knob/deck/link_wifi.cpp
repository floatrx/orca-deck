// Wi-Fi link: HTTP straight to the bridge, the fallback when BLE and USB are both gone for 10 s
// (Settings → Wi-Fi: off / fallback). Wi-Fi and BLE share one radio, so Wi-Fi is off while a relay link lives.
#include "deck.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

#define FALLBACK_AFTER_MS 10000
#define FAILS_TO_REPORT 3  // a weak signal breaks single polls; report only after this many in a row

static int fails_in_row = 0;
static volatile int wifi_reason = 0;
static bool ever_joined = false;
static int last_rssi = 0;
static String ssid;
static bool radio_on = false;

static void poll_failed(const String &why)
{
  Serial.printf("wifi poll failed: %s\n", why.c_str());
  if (++fails_in_row >= FAILS_TO_REPORT) set_status(false, why);
}

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
static String band_twin(const String &name)
{
  String low = name;
  low.toLowerCase();
  for (const char *tail : {"_5ghz", "-5ghz", " 5ghz", "5ghz", "_5g", "-5g", " 5g", "5g"}) {
    if (low.endsWith(tail)) return name.substring(0, name.length() - strlen(tail));
  }
  return "";
}

static String pick_ssid()
{
  String twin = band_twin(WIFI_SSID);
  int n = WiFi.scanNetworks();
  bool seen = false, twin_seen = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) seen = true;
    if (twin.length() && WiFi.SSID(i) == twin) twin_seen = true;
  }
  WiFi.scanDelete();
  Serial.printf("wifi: %d networks, configured %s, twin %s\n", n, seen ? "seen" : "not seen", twin_seen ? "seen" : "not seen");
  return seen || !twin_seen ? String(WIFI_SSID) : twin;
}

static HTTPClient *open_request(HTTPClient &http, const String &path, int timeout)
{
  http.useHTTP10(true);  // no chunked encoding: the body length is known up front
  http.setTimeout(timeout);
  http.begin(String(BRIDGE_URL) + path);
  http.addHeader("Authorization", String("Bearer ") + BRIDGE_TOKEN);
  return &http;
}

// Reads a whole body: parsing straight from the socket gave up on a slow, lossy link.
static bool get_body(const String &path, String &body, String &why)
{
  HTTPClient http;
  open_request(http, path, 5000);
  int code = http.GET();
  if (code != 200) {
    http.end();
    why = code > 0 ? "bridge HTTP " + String(code) : "bridge offline (" + String(code) + ")";
    return false;
  }
  int expected = http.getSize();
  body = http.getString();
  http.end();
  if (expected > 0 && (int)body.length() != expected) {
    why = "bridge: body cut short";
    return false;
  }
  return true;
}

// HTTPClient codes that mean the request never reached the bridge, so sending it again cannot repeat it.
// -5 / -11 may come after the bridge acted: not retried, or a double tap could press Enter twice.
static bool undelivered(int code)
{
  return code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
         code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED;
}

static bool send_command(const Command &c)
{
  HTTPClient http;
  open_request(http, "/api/cmd", 3000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(c.body);
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
  set_status(code == 200, ok ? c.kind + " ok" : c.kind + " failed" + (code != 200 ? " HTTP " + String(code) : "") +
                              (note.length() ? ": " + note : ""));
  return true;
}

static void retry_later(const Command &c)
{
  if (millis() - c.queued_at > RETRY_FOR_MS) {
    set_status(false, c.kind + " dropped: bridge unreachable");
    return;
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool superseded = false;
  for (const Command &queued : shared.commands) superseded |= c.kind == "go" && queued.kind == "go";
  if (!superseded) shared.commands.push_front(c);
  xSemaphoreGive(shared_mux);
}

static void radio(bool on)
{
  if (on == radio_on) return;
  radio_on = on;
  if (on) {
    Serial.println("wifi: fallback on");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem power-save drops the link on a weak 2.4 GHz signal
    WiFi.setAutoReconnect(true);
    ssid = pick_ssid();
    WiFi.begin(ssid.c_str(), WIFI_PASS);
  } else {
    Serial.println("wifi: off");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    if (shared.link == LINK_WIFI) shared.link = LINK_NONE;
    xSemaphoreGive(shared_mux);
  }
}

static void wifi_task(void *)
{
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    int reason = info.wifi_sta_disconnected.reason;
    if (reason != WIFI_REASON_STA_LEAVING && reason != WIFI_REASON_ASSOC_LEAVE) wifi_reason = reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  uint32_t relay_last = millis(), last_poll = 0, last_detail = 0, lost_at = 0;
  bool joined = false;
  for (;;) {
    bool relay = link_alive(LINK_BLE) || link_alive(LINK_USB);
    if (relay) relay_last = millis();
    bool wanted = settings.wifi && strlen(WIFI_PASS) && !relay && millis() - relay_last > FALLBACK_AFTER_MS;
    radio(wanted);
    if (!wanted) {
      joined = false;
      // Wi-Fi off is the user's choice, not the problem: the missing link is Bluetooth (user, 2026-11-14: «вимкнений
      // Wi-Fi, а ти показуєш … Wi-Fi off»).
      if (!relay) set_status(false, ble_connected() ? "waiting for the relay…" : "Bluetooth: looking for the Mac…");
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      drop_stale_commands();
      if (!lost_at) lost_at = millis();
      // The stack's own reconnect can stall; restarting sooner than 20 s collides with its attempt.
      if (millis() - lost_at > 20000) {
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        WiFi.begin(ssid.c_str(), WIFI_PASS);
        lost_at = millis();
      }
      set_status(false, wifi_problem() + (ssid != WIFI_SSID ? " · " + ssid : ""));
      joined = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    lost_at = 0;
    last_rssi = WiFi.RSSI();
    if (!joined) {
      joined = ever_joined = true;
      wifi_reason = 0;
      configTime(0, 0, "pool.ntp.org", "time.apple.com");
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      shared.link = LINK_WIFI;
      xSemaphoreGive(shared_mux);
    }
    Command c;
    if (pop_command(c)) {
      if (send_command(c)) last_poll = millis() - POLL_MS + 250;  // show where Orca went soon after
      else retry_later(c);
      continue;
    }
    String body, why;
    if (millis() - last_poll >= POLL_MS) {
      last_poll = millis();
      if (!get_body("/api/state", body, why)) poll_failed(why);
      else if (apply_state(body, "wifi " + String(last_rssi) + " dBm")) fails_in_row = 0;
      else poll_failed("bad json");
    }
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    String want = shared.want_detail;
    xSemaphoreGive(shared_mux);
    if (want.length() && millis() - last_detail >= POLL_MS) {
      last_detail = millis();
      String path = "/api/agent?id=" + want;
      path.replace("%", "%25");
      path.replace(" ", "%20");
      if (get_body(path, body, why)) apply_detail(body);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void wifi_link_start()
{
  WiFi.mode(WIFI_OFF);
  xTaskCreatePinnedToCoreWithCaps(wifi_task, "wifi", 16 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

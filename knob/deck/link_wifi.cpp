// Wi-Fi link: HTTP straight to the bridge, the fallback when BLE and USB are both gone for 10 s
// (Settings → Wi-Fi: off / fallback). Wi-Fi and BLE share one radio, so Wi-Fi is off while a relay link lives.
#include "deck.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <algorithm>
#include "secrets.h"

#define FALLBACK_AFTER_MS 10000
#define FAILS_TO_REPORT 3  // a weak signal breaks single polls; report only after this many in a row

static int fails_in_row = 0;
static volatile int wifi_reason = 0;
static bool ever_joined = false;
static int last_rssi = 0;
static String ssid, pass;
static std::vector<String> bridge_urls;
static size_t url_at = 0;  // the bridge address in use; a failed poll moves to the next
static bool radio_on = false;

static void poll_failed(const String &why)
{
  Serial.printf("wifi poll failed: %s\n", why.c_str());
  if (bridge_urls.size() > 1) url_at = (url_at + 1) % bridge_urls.size();  // the Mac may have another address here
  if (++fails_in_row >= FAILS_TO_REPORT) set_status(false, why);
}

// The Wi-Fi app's requests (LVGL task → this task, which alone drives the radio): a scan, or a trial join with a typed
// password. The trial disconnects a fallback link for a few seconds and rejoins it after.
static SemaphoreHandle_t req_mux;
static bool scan_wanted = false, try_wanted = false;
static std::vector<WifiScanItem> scan_items;
static uint32_t scan_version = 0;
static String try_ssid, try_pass, try_why;
static int try_state = 0;  // 0 idle / pending, 1 joined, -1 failed

void wifi_request_scan()
{
  xSemaphoreTake(req_mux, portMAX_DELAY);
  scan_wanted = true;
  xSemaphoreGive(req_mux);
}

uint32_t wifi_scan_take(std::vector<WifiScanItem> &out)
{
  xSemaphoreTake(req_mux, portMAX_DELAY);
  out = scan_items;
  uint32_t v = scan_version;
  xSemaphoreGive(req_mux);
  return v;
}

void wifi_request_try(const String &ssid_in, const String &pass_in)
{
  xSemaphoreTake(req_mux, portMAX_DELAY);
  try_ssid = ssid_in;
  try_pass = pass_in;
  try_state = 0;
  try_why = "";
  try_wanted = true;
  xSemaphoreGive(req_mux);
}

int wifi_try_result(String &why)
{
  xSemaphoreTake(req_mux, portMAX_DELAY);
  int s = try_wanted ? 0 : try_state;
  why = try_why;
  xSemaphoreGive(req_mux);
  return s;
}

static void serve_requests()
{
  xSemaphoreTake(req_mux, portMAX_DELAY);
  bool scan = scan_wanted, trial = try_wanted;
  String s = try_ssid, p = try_pass;
  xSemaphoreGive(req_mux);
  if (!scan && !trial) return;
  bool was_on = radio_on;
  if (!was_on) WiFi.mode(WIFI_STA);
  if (scan) {
    int n = WiFi.scanNetworks();
    std::vector<WifiScanItem> items;
    for (int i = 0; i < n; i++) {
      String name = WiFi.SSID(i);
      if (!name.length()) continue;  // hidden networks
      bool merged = false;
      for (WifiScanItem &it : items) {
        if (it.ssid == name) it.rssi = max(it.rssi, (int)WiFi.RSSI(i)), merged = true;
      }
      if (!merged) items.push_back({name, (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    }
    WiFi.scanDelete();
    std::sort(items.begin(), items.end(), [](const WifiScanItem &a, const WifiScanItem &b) { return a.rssi > b.rssi; });
    xSemaphoreTake(req_mux, portMAX_DELAY);
    scan_items = items;
    scan_version++;
    scan_wanted = false;
    xSemaphoreGive(req_mux);
  }
  if (trial) {
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_reason = 0;
    WiFi.begin(s.c_str(), p.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      int r = wifi_reason;
      if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT) break;
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    bool ok = WiFi.status() == WL_CONNECTED;
    int r = wifi_reason;
    String why = ok ? "" : r == WIFI_REASON_NO_AP_FOUND ? "not found on 2.4 GHz"
                 : r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ? "wrong password?"
                 : r ? "refused (reason " + String(r) + ")" : "no answer from the network";
    Serial.printf("wifi trial %s: %s\n", s.c_str(), ok ? "joined" : why.c_str());  // the SSID only
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_reason = 0;
    if (was_on) WiFi.begin(ssid.c_str(), pass.c_str());  // the fallback link, back on its own network
    xSemaphoreTake(req_mux, portMAX_DELAY);
    try_state = ok ? 1 : -1;
    try_why = why;
    try_wanted = false;
    xSemaphoreGive(req_mux);
  }
  if (!was_on) WiFi.mode(WIFI_OFF);
}

static String wifi_problem()
{
  if (ever_joined && wifi_reason) return L("wifi: weak signal (", "wifi: слабкий сигнал (") + String(last_rssi) + L(" dBm) · rejoining", " dBm) · перепідключаюсь");
  switch (wifi_reason) {
    case 0: return "wifi…";
    case WIFI_REASON_NO_AP_FOUND: return L("wifi: network not found on 2.4 GHz", "wifi: мережу 2.4 ГГц не знайдено");
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return L("wifi: wrong password?", "wifi: хибний пароль?");
    default: return L("wifi: reason ", "wifi: помилка ") + String(wifi_reason);
  }
}

// The strongest network on the air that the store knows (by name or by its 2.4 GHz twin); none seen: the first stored.
static void pick_network()
{
  std::vector<WifiNet> known = wifi_store_list();
  int n = WiFi.scanNetworks(), best_rssi = -1000;
  ssid = known.empty() ? "" : known[0].ssid;
  pass = known.empty() ? "" : known[0].pass;
  for (int i = 0; i < n; i++) {
    for (const WifiNet &k : known) {
      String twin = wifi_band_twin(k.ssid);
      if ((WiFi.SSID(i) == k.ssid || (twin.length() && WiFi.SSID(i) == twin)) && WiFi.RSSI(i) > best_rssi) {
        best_rssi = WiFi.RSSI(i);
        ssid = WiFi.SSID(i);
        pass = k.pass;
      }
    }
  }
  WiFi.scanDelete();
  Serial.printf("wifi: %d on the air, %d known, joining %s (%d dBm)\n", n, (int)known.size(), ssid.c_str(), best_rssi);
}

static HTTPClient *open_request(HTTPClient &http, const String &path, int timeout)
{
  http.useHTTP10(true);  // no chunked encoding: the body length is known up front
  http.setTimeout(timeout);
  http.begin((bridge_urls.empty() ? String(BRIDGE_URL) : bridge_urls[url_at % bridge_urls.size()]) + path);
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
  set_status(code == 200, ok ? c.kind + L(" ok", " ок") : c.kind + L(" failed", " не вдалось") + (code != 200 ? " HTTP " + String(code) : "") +
                              (note.length() ? ": " + note : ""));
  return true;
}

static void retry_later(const Command &c)
{
  if (millis() - c.queued_at > RETRY_FOR_MS) {
    set_status(false, c.kind + L(" dropped: bridge unreachable", " скасовано: бридж недоступний"));
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
    pick_network();
    bridge_urls = wifi_store_urls();
    url_at = 0;
    WiFi.begin(ssid.c_str(), pass.c_str());
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
    serve_requests();
    bool relay = link_alive(LINK_BLE) || link_alive(LINK_USB);
    if (relay) relay_last = millis();
    bool wanted = settings.wifi && !wifi_store_list().empty() && !relay && millis() - relay_last > FALLBACK_AFTER_MS;
    radio(wanted);
    if (!wanted) {
      joined = false;
      // Wi-Fi off is the user's choice, not the problem: the missing link is Bluetooth (user, 2026-11-14: «вимкнений
      // Wi-Fi, а ти показуєш … Wi-Fi off»).
      if (!relay) set_status(false, ble_connected() ? L("waiting for the relay…", "чекаю на релей…") : L("Bluetooth: looking for the Mac…", "Bluetooth: шукаю Мак…"));
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
        pick_network();  // maybe another known network is in reach now
        WiFi.begin(ssid.c_str(), pass.c_str());
        lost_at = millis();
      }
      set_status(false, wifi_problem() + " · " + ssid);
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
  req_mux = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_OFF);
  xTaskCreatePinnedToCoreWithCaps(wifi_task, "wifi", 16 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

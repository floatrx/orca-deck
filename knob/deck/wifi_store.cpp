// The knob's Wi-Fi networks, kept in NVS so a new place needs no reflash (.work/plans/knob-wifi.md, user, 2026-11-15).
// Up to WIFI_MAX networks: `mac` ones come from the Mac (~/.config/orca-deck/wifi.json via the relay's W line) and are
// replaced as a set; `knob` ones were typed on the knob. secrets.h's network is the seed while the store is empty.
// The bridge's LAN URLs come with the Mac's list, so its address is learnt, not compiled.
// NVS writes happen only in wifi_store_persist(), from the LVGL task: a flash write from a PSRAM-stack task (BLE,
// Wi-Fi) asserted in cache_utils.c on 2026-11-14. Passwords never go to a log line or a report.
#include "deck.h"
#include <Preferences.h>
#include "secrets.h"

// The ESP32-S3 sees 2.4 GHz only. A dual-band router often names its 5 GHz network "<name>_5G" (or -5G,
// " 5GHz"); the 2.4 GHz twin is then "<name>" with the same password.
String wifi_band_twin(const String &name)
{
  String low = name;
  low.toLowerCase();
  for (const char *tail : {"_5ghz", "-5ghz", " 5ghz", "5ghz", "_5g", "-5g", " 5g", "5g"}) {
    if (low.endsWith(tail)) return name.substring(0, name.length() - strlen(tail));
  }
  return "";
}

static SemaphoreHandle_t mux;
static std::vector<WifiNet> nets;
static std::vector<String> urls;
static volatile bool dirty = false;

static void lock() { xSemaphoreTake(mux, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mux); }

void wifi_store_load()
{
  if (!mux) mux = xSemaphoreCreateMutex();
  Preferences prefs;
  prefs.begin("wifi", true);
  String raw = prefs.getString("nets", "[]"), lan = prefs.getString("lan", "[]");
  prefs.end();
  JsonDocument doc;
  lock();
  nets.clear();
  urls.clear();
  if (!deserializeJson(doc, raw)) {
    for (JsonArray n : doc.as<JsonArray>()) nets.push_back({n[0] | "", n[1] | "", (n[2] | 0) == 1});
  }
  JsonDocument u;
  if (!deserializeJson(u, lan)) {
    for (JsonVariant v : u.as<JsonArray>()) urls.push_back(v.as<String>());
  }
  unlock();
}

void wifi_store_persist()
{
  if (!dirty) return;
  dirty = false;
  JsonDocument doc, u;
  lock();
  for (const WifiNet &n : nets) {
    JsonArray a = doc.add<JsonArray>();
    a.add(n.ssid);
    a.add(n.pass);
    a.add(n.from_mac ? 1 : 0);
  }
  for (const String &url : urls) u.add(url);
  unlock();
  String raw, lan;
  serializeJson(doc, raw);
  serializeJson(u, lan);
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("nets", raw);
  prefs.putString("lan", lan);
  prefs.end();
  Serial.printf("wifi store: %d networks saved\n", (int)nets.size());
}

// Every network to try: the stored ones, then the compiled seed (when not stored already).
std::vector<WifiNet> wifi_store_list()
{
  lock();
  std::vector<WifiNet> out = nets;
  unlock();
  if (strlen(WIFI_SSID)) {
    bool have = false;
    for (const WifiNet &n : out) have |= n.ssid == WIFI_SSID;
    if (!have) out.push_back({WIFI_SSID, WIFI_PASS, false});
  }
  return out;
}

int wifi_store_count()
{
  lock();
  int n = nets.size();
  unlock();
  return n;
}

// The bridge's addresses to try, the learnt ones first, then secrets.h's BRIDGE_URL.
std::vector<String> wifi_store_urls()
{
  lock();
  std::vector<String> out = urls;
  unlock();
  bool have = false;
  for (const String &u : out) have |= u == BRIDGE_URL;
  if (!have && strlen(BRIDGE_URL)) out.push_back(BRIDGE_URL);
  return out;
}

// The Mac's list (any task): its networks replace the old `mac` ones; the knob's own stay.
void wifi_store_set_mac(const std::vector<WifiNet> &mac, const std::vector<String> &lan)
{
  lock();
  std::vector<WifiNet> next;
  for (const WifiNet &n : mac) {
    if (n.ssid.length() && (int)next.size() < WIFI_MAX) next.push_back({n.ssid, n.pass, true});
  }
  for (const WifiNet &n : nets) {
    bool shadowed = false;
    for (const WifiNet &m : next) shadowed |= m.ssid == n.ssid;
    if (!n.from_mac && !shadowed && (int)next.size() < WIFI_MAX) next.push_back(n);
  }
  bool changed = next.size() != nets.size() || (lan.size() && lan != urls);
  for (size_t i = 0; !changed && i < next.size(); i++) {
    changed = next[i].ssid != nets[i].ssid || next[i].pass != nets[i].pass || next[i].from_mac != nets[i].from_mac;
  }
  if (changed) {
    nets = next;
    if (lan.size()) urls = lan;
    dirty = true;
  }
  unlock();
}

// A network typed on the knob, after it joined (LVGL task).
void wifi_store_add(const String &ssid, const String &pass)
{
  lock();
  for (auto it = nets.begin(); it != nets.end();) it = it->ssid == ssid ? nets.erase(it) : it + 1;
  nets.insert(nets.begin(), {ssid, pass, false});
  if ((int)nets.size() > WIFI_MAX) nets.resize(WIFI_MAX);
  dirty = true;
  unlock();
}

void wifi_store_forget(const String &ssid)
{
  lock();
  for (auto it = nets.begin(); it != nets.end();) it = it->ssid == ssid ? nets.erase(it) : it + 1;
  dirty = true;
  unlock();
}

String wifi_store_report()  // SSIDs, where they came from, and password lengths: never a password
{
  String out = "wifi store:";
  for (const WifiNet &n : wifi_store_list()) out += " " + n.ssid + (n.from_mac ? "(mac," : "(") + String(n.pass.length()) + ")";
  out += " · urls " + String((int)wifi_store_urls().size());
  return out;
}

// Wi-Fi, an app in the hub (.work/plans/knob-wifi.md phase 4, user 2026-11-15: «апку … клавіатуру … реєстрацію
// SSID»): add a network with no Mac at hand.
//   list: the networks on the air, strongest first (saved ones marked) · crown: the network · tap: type its password
//         (a saved one: nothing to do) · hold: forget a network saved on the knob · the last row scans again
//   keyboard: a wheel of characters — crown: the character · tap: type it · hold: delete the last one ·
//             swipe left / right: abc · ABC · 123 · !?& · the wheel's first item, JOIN, tries the password
// Only a network that joined is saved (wifi_store_add). The password shows as typed — it is the user's own desk —
// but never goes to a log or a report.
#include "deck.h"

enum Stage { LIST, KEYS, JOINING };
static const char *const SETS[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789",
                                   "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~ "};
static const char *const SET_NAMES[] = {"abc", "ABC", "123", "!?&"};  // no '#': it is the recolour mark
static const int SET_COUNT = 4;
#define SLOTS 5     // list rows on screen
#define WHEEL 7     // characters drawn on the wheel

static lv_obj_t *page, *title, *hint, *spinner, *names[SLOTS], *values[SLOTS];
static lv_obj_t *field, *wheel[WHEEL], *wheel_box, *sets_label;
static Stage stage = LIST;
static std::vector<WifiScanItem> found;
static uint32_t seen_scan = 0;
static bool scanning = false;
static int focus = 0, set = 0, pos = 1;  // pos 0 is JOIN
static String target, typed, result;
static uint32_t result_until = 0;
static bool trial_only = false;  // T wifitry: join with a stored password, save nothing

static int set_len(int s) { return strlen(SETS[s]) + 1; }  // + JOIN

static String item(int s, int i)
{
  if (i == 0) return "JOIN";
  char c = SETS[s][i - 1];
  return c == ' ' ? "space" : String(c);
}

static bool saved(const String &ssid, bool *from_mac = nullptr)
{
  for (const WifiNet &n : wifi_store_list()) {
    if (n.ssid == ssid || wifi_band_twin(n.ssid) == ssid) {  // "home-5G" saved: "home" is it on 2.4 GHz
      if (from_mac) *from_mac = n.from_mac;
      return true;
    }
  }
  return false;
}

static String signal(int rssi) { return rssi > -60 ? "strong" : rssi > -72 ? "good" : "weak"; }

static void show_list(bool on)
{
  for (int i = 0; i < SLOTS; i++) {
    on ? lv_obj_clear_flag(names[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(names[i], LV_OBJ_FLAG_HIDDEN);
    on ? lv_obj_clear_flag(values[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(values[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void show_keys(bool on)
{
  for (lv_obj_t *o : {field, wheel_box, sets_label}) on ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < WHEEL; i++) on ? lv_obj_clear_flag(wheel[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(wheel[i], LV_OBJ_FLAG_HIDDEN);
}

static void paint()
{
  if (!page) return;
  bool fresh_result = millis() < result_until;
  show_list(stage == LIST);
  show_keys(stage == KEYS);
  (stage == JOINING || (stage == LIST && scanning && found.empty())) ? lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN)
                                                                    : lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  if (stage == LIST) {
    set_text(title, "WI-FI");
    int rows = found.size() + 1;  // + "Scan again"
    for (int slot = 0; slot < SLOTS; slot++) {
      int i = focus + slot - SLOTS / 2;
      bool mid = slot == SLOTS / 2;
      lv_obj_set_style_text_color(names[slot], lv_color_hex(mid ? C_INK : C_MUTED), 0);
      if (i < 0 || i >= rows) {
        set_text(names[slot], "");
        set_text(values[slot], "");
      } else if (i == (int)found.size()) {
        set_text(names[slot], scanning ? "scanning…" : "Scan again");
        set_text(values[slot], "");
      } else {
        const WifiScanItem &it = found[i];
        bool mac = false, have = saved(it.ssid, &mac);
        set_text(names[slot], it.ssid);
        set_text(values[slot], signal(it.rssi) + (it.open ? " · open" : "") + (have ? (mac ? " · saved (Mac)" : " · saved") : ""));
      }
    }
    set_text(hint, fresh_result ? result : found.empty() ? "" : "tap: password · hold: forget");
    lv_obj_set_style_text_color(hint, lv_color_hex(fresh_result ? C_DONE : C_MUTED), 0);
  } else if (stage == KEYS) {
    set_text(title, target);
    String shown = typed.length() > 16 ? "…" + typed.substring(typed.length() - 16) : typed;
    set_text(field, shown + "_");
    int n = set_len(set);
    for (int k = 0; k < WHEEL; k++) {
      int i = ((pos + k - WHEEL / 2) % n + n) % n;
      int d = abs(k - WHEEL / 2);
      String text = item(set, i);
      bool word = text.length() > 1;
      lv_obj_set_style_text_font(wheel[k], d == 0 ? (word ? &deck_bold_16 : &lv_font_montserrat_48)
                                                  : (word ? &deck_14 : d == 1 ? &lv_font_montserrat_28 : &lv_font_montserrat_20), 0);
      lv_obj_set_style_text_color(wheel[k], lv_color_hex(i == 0 ? C_DONE : d == 0 ? C_INK : C_DETAIL), 0);
      lv_obj_set_style_opa(wheel[k], d == 0 ? LV_OPA_COVER : d == 1 ? LV_OPA_70 : d == 2 ? LV_OPA_40 : LV_OPA_20, 0);
      set_text(wheel[k], text);
    }
    String sets;
    for (int s = 0; s < SET_COUNT; s++) sets += String(s ? "  " : "") + (s == set ? "#F1EFE9 " : "#55534F ") + SET_NAMES[s] + "#";
    set_text(sets_label, sets);
    set_text(hint, fresh_result ? result : "tap: type · hold: delete");
    lv_obj_set_style_text_color(hint, lv_color_hex(fresh_result ? C_ATTENTION : C_MUTED), 0);
  } else {
    set_text(title, target);
    set_text(hint, "joining…");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);
  }
}

static void scan()
{
  scanning = true;
  wifi_request_scan();
  paint();
}

static void turn(int step)
{
  if (stage == LIST) {
    int next = focus + step;
    if (next < 0 || next > (int)found.size()) return;
    focus = next;
  } else if (stage == KEYS) {
    int n = set_len(set);
    pos = ((pos + step) % n + n) % n;
  } else {
    return;
  }
  haptic_tick();
  paint();
}

static void tap()
{
  if (stage == LIST) {
    if (focus == (int)found.size()) {
      if (!scanning) scan();
      haptic_click();
      return;
    }
    const WifiScanItem &it = found[focus];
    if (saved(it.ssid)) {
      haptic_buzz(1);  // known already: nothing to type
      return;
    }
    target = it.ssid;
    typed = "";
    set = 0;
    pos = 1;
    if (it.open) {  // no password to type: try it straight away
      stage = JOINING;
      wifi_request_try(target, "");
    } else {
      stage = KEYS;
    }
    haptic_click();
    paint();
  } else if (stage == KEYS) {
    if (pos == 0) {  // JOIN
      if (typed.length() < 8) {
        result = "a password has 8 characters or more";
        result_until = millis() + 2500;
        haptic_buzz(1);
        paint();
        return;
      }
      stage = JOINING;
      wifi_request_try(target, typed);
      haptic_click(2);
    } else {
      if (typed.length() < 63) typed += SETS[set][pos - 1];
      haptic_click();
    }
    paint();
  }
}

static void hold()
{
  if (stage == KEYS) {
    if (typed.length()) typed.remove(typed.length() - 1);
    haptic_click();
    paint();
  } else if (stage == LIST && focus < (int)found.size()) {
    bool mac = false;
    if (!saved(found[focus].ssid, &mac)) return haptic_buzz(1);
    if (mac) {
      result = "a Mac network: remove it in wifi.json";
      result_until = millis() + 3000;
      haptic_buzz(1);
    } else {
      wifi_store_forget(found[focus].ssid);
      result = "forgotten";
      result_until = millis() + 2000;
      haptic_click(2);
    }
    paint();
  }
}

static void swipe(lv_dir_t dir)
{
  if (stage != KEYS || (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)) return;
  set = (set + (dir == LV_DIR_LEFT ? 1 : SET_COUNT - 1)) % SET_COUNT;
  pos = 1;
  haptic_click();
  paint();
}

void wifi_app_tick()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  std::vector<WifiScanItem> items;
  uint32_t v = wifi_scan_take(items);
  if (v != seen_scan) {
    seen_scan = v;
    found = items;
    scanning = false;
    focus = min(focus, (int)found.size());
    paint();
  }
  if (stage == JOINING) {
    String why;
    int r = wifi_try_result(why);
    if (r == 1) {
      if (!trial_only) wifi_store_add(target, typed);
      trial_only = false;
      result = target + " saved";
      result_until = millis() + 3000;
      stage = LIST;
      haptic_click(2);
      paint();
    } else if (r == -1) {
      trial_only = false;
      result = why;
      result_until = millis() + 3500;
      stage = typed.length() ? KEYS : LIST;
      haptic_buzz(1);
      paint();
    }
  }
  static uint32_t last = 0;
  if (result_until && millis() > result_until && millis() - last > 200) {
    last = millis();
    result_until = 0;
    paint();
  }
}

static String subtitle() { return String(wifi_store_list().size()) + " saved"; }

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  stage = LIST;
  focus = 0;
  scan();
}

static void hide()
{
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  typed = "";  // the password does not wait in memory for the next visit
}

// Tests: a trial join of a scanned network with the password the store has for it (no password on the T line).
void wifi_app_test_try(const String &ssid)
{
  for (const WifiNet &n : wifi_store_list()) {
    if (n.ssid == ssid || wifi_band_twin(n.ssid) == ssid) {
      target = ssid;
      typed = n.pass;
      trial_only = true;
      stage = JOINING;
      wifi_request_try(target, typed);
      paint();
      return;
    }
  }
  Serial.println("wifitry: not in the store");
}

String wifi_app_report()  // the SSID and how much is typed: never the password itself
{
  return String(stage == LIST ? "list" : stage == KEYS ? "keys" : "joining") + " found=" + String((int)found.size()) +
         " focus=" + String(focus) + " target=" + target + " typed=" + String(typed.length()) + " set=" + SET_NAMES[set] +
         " at=" + item(set, pos) + " | " + lv_label_get_text(hint);
}

void wifi_app_create()
{
  page = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page, 360, 360);
  lv_obj_center(page);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

  title = make_label(page, &deck_bold_16, C_MUTED, -122, 220);
  lv_obj_set_style_text_letter_space(title, 1, 0);
  for (int slot = 0; slot < SLOTS; slot++) {  // the list, as Settings: the focused row in the middle
    int y = (slot - SLOTS / 2) * 40 - 4;
    bool mid = slot == SLOTS / 2;
    names[slot] = make_label(page, mid ? &deck_bold_16 : &deck_14, C_MUTED, y - 9, 240);
    values[slot] = make_label(page, &deck_14, mid ? C_WORKING : 0x55534F, y + 10, 240);
  }

  field = make_label(page, &lv_font_montserrat_20, C_INK, -78, 250);  // what is typed, with a cursor
  wheel_box = lv_obj_create(page);  // the wheel's window: the character a tap types
  lv_obj_set_size(wheel_box, 74, 74);
  lv_obj_align(wheel_box, LV_ALIGN_CENTER, 0, 4);
  lv_obj_set_style_radius(wheel_box, 18, 0);
  lv_obj_set_style_bg_color(wheel_box, lv_color_hex(0x1C1D21), 0);
  lv_obj_set_style_border_width(wheel_box, 0, 0);
  lv_obj_clear_flag(wheel_box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  static const int X[WHEEL] = {-132, -96, -56, 0, 56, 96, 132};
  static const int Y[WHEEL] = {26, 14, 8, 4, 8, 14, 26};  // a slight smile: the wheel curves with the screen
  for (int k = 0; k < WHEEL; k++) {
    wheel[k] = make_label(page, &lv_font_montserrat_28, C_DETAIL, 4, 70);
    lv_obj_align(wheel[k], LV_ALIGN_CENTER, X[k], Y[k]);
  }
  sets_label = make_label(page, &deck_bold_16, C_MUTED, 66, 220);
  lv_label_set_recolor(sets_label, true);
  hint = make_label(page, &deck_14, C_MUTED, 112, 220);
  spinner = lv_spinner_create(page, 1000, 60);
  lv_obj_set_size(spinner, 34, 34);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 70);
  lv_obj_set_style_arc_width(spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
  show_keys(false);
  paint();
}

const App app_wifi = {"Wi-Fi", LV_SYMBOL_WIFI, 0x4CC3FF, show, hide, subtitle, turn, tap, hold, swipe};

// Settings: stored in NVS. Two pages share this code: the Settings app in the hub (the knob's own rows) and
// orca-deck's page (swipe right in it, as Apex): Hide inactive, Hide completed.
// The crown moves between rows (the focused one sits in the middle), a tap changes the value. Connection forgets
// the paired relay only on a 2 s hold: a double tap did it by accident (2026-11-14).
#include "deck.h"
#include <Preferences.h>
#include "lcd_bl_pwm_bsp.h"
#include "sound.h"

Settings settings;

static Preferences prefs;
static lv_obj_t *page, *title, *hint;
static const int VISIBLE = 5;
static lv_obj_t *names[VISIBLE], *values[VISIBLE];
static int focus = 0;               // index into `rows`
static uint32_t forget_hold_from = 0;  // when the hold on Connection began; 0: not holding

#define FORGET_HOLD_MS 2000
#define LONG_PRESS_MS 400  // LVGL's long press fires after this: the hold began that long before hold()

enum Row { ROW_LINK, ROW_WIFI, ROW_HIDE_INACTIVE, ROW_HIDE_COMPLETED, ROW_HAPTICS, ROW_SOUND, ROW_BRIGHTNESS, ROW_SAVER,
           ROW_WIFI_ADD };
static const std::vector<int> SYSTEM_ROWS = {ROW_LINK, ROW_WIFI, ROW_WIFI_ADD, ROW_SAVER, ROW_HAPTICS, ROW_SOUND, ROW_BRIGHTNESS};
static uint32_t wifi_asked_at = 0;  // "Add the Mac's Wi-Fi" was tapped: the password is asked on the Mac
static const uint8_t SAVER_STEPS[] = {0, 5, 10, 15, 20, 25, 30, 60};  // user, 2026-11-14
static const std::vector<int> DECK_ROWS = {ROW_HIDE_INACTIVE, ROW_HIDE_COMPLETED};
static const std::vector<int> *rows = &SYSTEM_ROWS;

void settings_load()
{
  prefs.begin("deck", true);
  settings.wifi = prefs.getUChar("wifi", settings.wifi);
  settings.hide_inactive = prefs.getBool("hideInactive", settings.hide_inactive);
  settings.hide_completed = prefs.getUChar("hideDone", settings.hide_completed);
  settings.haptics = prefs.getUChar("haptics", settings.haptics);
  settings.sound = prefs.getUChar("sound", settings.sound);
  settings.brightness = prefs.getUChar("bright", settings.brightness);
  settings.saver = prefs.getUChar("saver", settings.saver);
  prefs.end();
}

static void save();
static bool editing = false;  // the Brightness row is taking the crown

// Brightness in 5 % steps (1 % under 10 %) (user, 2026-11-14: «більше градацій»; it was 40 / 60 / 80 / 100). The crown sets
// it, live; the caller saves once when the editing ends (one NVS write, not one a detent).
// Below 10 % it goes a percent at a time, down to 1 % (user, 2026-11-15: «від десяти … 9, 8, 7 … 1»): the dark end
// is where a step is felt.
void settings_adjust_brightness(int step)
{
  int now = settings.brightness, next = now;
  for (int i = 0; i < abs(step); i++) {
    bool fine = step > 0 ? next < 10 : next <= 10;
    next += (step > 0 ? 1 : -1) * (fine ? 1 : BRIGHT_STEP);
  }
  next = max(BRIGHT_MIN, min(100, next));
  if (next == settings.brightness) return;
  settings.brightness = next;
  settings_apply();
  haptic_tick();
}

void settings_save() { save(); }

static void save()
{
  prefs.begin("deck", false);
  prefs.putUChar("wifi", settings.wifi);
  prefs.putBool("hideInactive", settings.hide_inactive);
  prefs.putUChar("hideDone", settings.hide_completed);
  prefs.putUChar("haptics", settings.haptics);
  prefs.putUChar("sound", settings.sound);
  prefs.putUChar("bright", settings.brightness);
  prefs.putUChar("saver", settings.saver);
  prefs.end();
}

void settings_set_haptics(int level)
{
  settings.haptics = (uint8_t)max(0, min(3, level));
  save();
  haptic_sample();
}

void settings_apply()
{
  setUpdutySubdivide(255 * settings.brightness / 100);
  sound_set_level(settings.sound);
}

static String row_name(int row)
{
  static const char *NAMES[] = {"Connection", "Wi-Fi", "Hide inactive", "Hide completed", "Haptics", "Sound",
                                "Brightness", "Screensaver", "Add the Mac's Wi-Fi"};
  return NAMES[row];
}

static String row_value(int row)
{
  switch (row) {
    case ROW_LINK: {
      if (forget_hold_from) return "keep holding…";
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      LinkKind link = shared.link;
      xSemaphoreGive(shared_mux);
      return String(link_name(link)) + (ble_connected() ? " · paired" : "");
    }
    case ROW_WIFI: return settings.wifi ? "fallback" : "off";
    case ROW_HIDE_INACTIVE: return settings.hide_inactive ? "on" : "off";
    case ROW_HIDE_COMPLETED: return settings.hide_completed == 0 ? "off" : settings.hide_completed == 1 ? "1 h" : "1 day";
    case ROW_HAPTICS: return settings.haptics ? String(settings.haptics) + "/3" : "off";
    case ROW_SOUND: return settings.sound == 0 ? "off" : settings.sound == 1 ? "quiet" : "on";
    case ROW_WIFI_ADD:
      if (wifi_asked_at && millis() - wifi_asked_at < 30000) return "type the password on the Mac";
      return String(wifi_store_count()) + " saved · tap to add";
    case ROW_BRIGHTNESS: return editing ? "- " + String(settings.brightness) + "% +" : String(settings.brightness) + "%";
    case ROW_SAVER: return settings.saver == 0 ? "off" : settings.saver >= 60 ? "after 1 min" : "after " + String(settings.saver) + " s";
  }
  return "";
}

void settings_page_refresh()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  for (int slot = 0; slot < VISIBLE; slot++) {
    int i = focus + slot - VISIBLE / 2;
    bool used = i >= 0 && i < (int)rows->size();
    set_text(names[slot], used ? row_name((*rows)[i]) : "");
    set_text(values[slot], used ? row_value((*rows)[i]) : "");
  }
  bool deck = rows == &DECK_ROWS;
  if (editing) {
    set_text(hint, "crown: brightness · tap: done");
    return;
  }
  set_text(hint, (*rows)[focus] == ROW_LINK ? "hold 2 s: forget the paired relay"
                 : deck                     ? "tap: change · swipe left: back"
                                            : "tap: change · swipe down: hub");
}

void settings_page_turn(int step)
{
  if (editing) {
    settings_adjust_brightness(step);
    settings_page_refresh();
    return;
  }
  int next = focus + step;
  if (next < 0 || next >= (int)rows->size()) return;
  focus = next;
  forget_hold_from = 0;
  haptic_tick();
  settings_page_refresh();
}

void settings_page_tap()
{
  switch ((*rows)[focus]) {
    case ROW_LINK: return;  // forgetting takes a hold, never a tap
    case ROW_WIFI: settings.wifi = !settings.wifi; break;
    case ROW_HIDE_INACTIVE: settings.hide_inactive = !settings.hide_inactive; break;
    case ROW_HIDE_COMPLETED: settings.hide_completed = (settings.hide_completed + 1) % 3; break;
    case ROW_HAPTICS: settings.haptics = (settings.haptics + 1) % 4; break;
    case ROW_SOUND: settings.sound = (settings.sound + 1) % 3; break;
    case ROW_WIFI_ADD: {  // the bridge asks the password in a dialog on the Mac; the W line brings the network here
      JsonDocument cmd;
      cmd["cmd"] = "wifi_add";
      push_command(cmd);
      wifi_asked_at = millis();
      haptic_click();
      settings_page_refresh();
      return;
    }
    case ROW_BRIGHTNESS:  // tap: the crown sets it; tap again: done (saved)
      editing = !editing;
      if (!editing) save();
      haptic_click();
      settings_page_refresh();
      return;
    case ROW_SAVER: {
      size_t i = 0, n = sizeof(SAVER_STEPS);
      while (i < n && SAVER_STEPS[i] != settings.saver) i++;
      settings.saver = SAVER_STEPS[(i + 1) % n];
      buddy_input();  // the new wait starts from now
      break;
    }
  }
  save();
  settings_apply();
  haptic_sample();  // the new strength / sound level, felt at once
  settings_page_refresh();
}

void settings_page_hold()
{
  if (!settings_page_open() || (*rows)[focus] != ROW_LINK) return;
  forget_hold_from = millis() - LONG_PRESS_MS;
  haptic_click();
  settings_page_refresh();
}

void settings_tick()
{
  if (!forget_hold_from) return;
  if (!hub_pressing() || !settings_page_open()) {
    forget_hold_from = 0;  // let go early: nothing happens
    settings_page_refresh();
  } else if (millis() - forget_hold_from >= FORGET_HOLD_MS) {
    forget_hold_from = 0;
    ble_forget();
    haptic_buzz(1);
    settings_page_refresh();
  }
}

void settings_page_create()
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

  title = make_label(page, &deck_bold_16, C_MUTED, -122, 200);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  set_text(title, "SETTINGS");
  for (int slot = 0; slot < VISIBLE; slot++) {
    int y = (slot - VISIBLE / 2) * 40 - 4;
    bool mid = slot == VISIBLE / 2;
    names[slot] = make_label(page, mid ? &deck_bold_16 : &deck_14, mid ? C_INK : C_MUTED, y - 9, 240);
    values[slot] = make_label(page, mid ? &deck_16 : &deck_14, mid ? C_WORKING : 0x55534F, y + 10, 240);
  }
  hint = make_label(page, &deck_14, C_MUTED, 122, 244);  // ~250 px of screen at this height
}

void settings_page_show(bool show, bool deck_rows)
{
  if (show) {
    rows = deck_rows ? &DECK_ROWS : &SYSTEM_ROWS;
    focus = 0;
    set_text(title, deck_rows ? "CLAUDE" : "SETTINGS");
    lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page);
    forget_hold_from = 0;
    settings_page_refresh();
  } else {
    if (editing) save();  // left while setting it: keep what was set
    editing = false;
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  }
}

String settings_report()
{
  int row = (*rows)[focus];
  return row_name(row) + "=" + row_value(row) + (editing ? " EDITING" : "") + " brightness=" + String(settings.brightness);
}

bool settings_page_open() { return page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN); }

// The Settings app in the hub.
static void app_show() { settings_page_show(true); }
static void app_hide() { settings_page_show(false); }
static String app_subtitle()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  LinkKind link = shared.link;
  xSemaphoreGive(shared_mux);
  return String(link_name(link)) + (ble_connected() ? " · paired" : "");
}

const App app_settings = {"Settings", LV_SYMBOL_SETTINGS, 0x9A9890, app_show, app_hide, app_subtitle,
                          settings_page_turn, settings_page_tap, settings_page_hold, nullptr};

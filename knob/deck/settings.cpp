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
           ROW_WIFI_ADD, ROW_ABOUT, ROW_VACATION, ROW_LANG, ROW_HUB };
static const std::vector<int> SYSTEM_ROWS = {ROW_LINK, ROW_WIFI, ROW_WIFI_ADD, ROW_HUB, ROW_SAVER, ROW_HAPTICS, ROW_SOUND,
                                              ROW_BRIGHTNESS, ROW_LANG, ROW_VACATION, ROW_ABOUT};
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
  settings.lang = prefs.getUChar("lang", settings.lang);
  settings.hub_grid = prefs.getBool("hubGrid", settings.hub_grid);
  prefs.end();
}

static void save();
// A row with several values or a stepper (user, 2026-11-15): a tap gives it the crown, the crown sets the value
// (live), a second tap saves and gives the crown back to the rows. On / off rows still flip on one tap.
static bool editing = false;

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
  prefs.putUChar("lang", settings.lang);
  prefs.putBool("hubGrid", settings.hub_grid);
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
  const char *NAMES[] = {L("Connection", "Зв’язок"), "Wi-Fi", L("Hide inactive", "Ховати неактивні"), L("Hide completed", "Ховати завершені"), L("Haptics", "Вібро"), L("Sound", "Звук"),
                                L("Brightness", "Яскравість"), L("Screensaver", "Заставка"), L("Add the Mac's Wi-Fi", "Додати Wi-Fi Мака"), L("About", "Про пристрій"), L("Clawd vacation", "Відпустка Clawd"),
                        "Мова / Language", L("Hub layout", "Вигляд хабу")};
  return NAMES[row];
}

static String row_value(int row)
{
  switch (row) {
    case ROW_LINK: {
      if (forget_hold_from) return L("keep holding…", "тримай далі…");
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      LinkKind link = shared.link;
      xSemaphoreGive(shared_mux);
      return String(link_name(link)) + (ble_connected() ? L(" · paired", " · спарено") : "");
    }
    case ROW_WIFI: return settings.wifi ? L("fallback", "запасний") : L("off", "вимк");
    case ROW_HIDE_INACTIVE: return settings.hide_inactive ? L("on", "увімк") : L("off", "вимк");
    case ROW_HIDE_COMPLETED: return settings.hide_completed == 0 ? L("off", "вимк") : settings.hide_completed == 1 ? L("1 h", "1 год") : L("1 day", "1 день");
    case ROW_HAPTICS: return settings.haptics ? String(settings.haptics) + "/3" : L("off", "вимк");
    case ROW_SOUND: return settings.sound == 0 ? L("off", "вимк") : settings.sound == 1 ? L("quiet", "тихо") : L("on", "увімк");
    case ROW_WIFI_ADD:
      if (wifi_asked_at && millis() - wifi_asked_at < 30000) return L("type the password on the Mac", "введи пароль на Маку");
      return String(wifi_store_count()) + L(" saved · tap to add", " збережено · тап: додати");
    case ROW_BRIGHTNESS: return String(settings.brightness) + "%";
    case ROW_LANG: return settings.lang ? "English" : "Українська";
    case ROW_HUB: return settings.hub_grid ? L("grid", "сітка") : L("list", "список");
    case ROW_VACATION: return pet_vacation() ? L("time stands still", "час стоїть") : L("off", "вимкнена");  // the pet's clock frozen (pet.cpp)
    case ROW_ABOUT: return String("Clawd · ") + String(__DATE__).substring(0, 6);  // the device is Clawd (user, 2026-11-15)
    case ROW_SAVER: return settings.saver == 0 ? L("off", "вимк") : settings.saver >= 60 ? L("after 1 min", "через 1 хв") : L("after ", "через ") + String(settings.saver) + L(" s", " с");
  }
  return "";
}

static bool stepped(int row)  // the rows that take the crown
{
  return row == ROW_HIDE_COMPLETED || row == ROW_HAPTICS || row == ROW_SOUND || row == ROW_BRIGHTNESS || row == ROW_SAVER;
}

// One crown detent on a row being set: the next / previous value, clamped (no wrap: the end is felt).
static void step_row(int row, int dir)
{
  int before;
  switch (row) {
    case ROW_HIDE_COMPLETED:
      before = settings.hide_completed;
      settings.hide_completed = max(0, min(2, before + dir));
      if (settings.hide_completed != before) haptic_tick();
      return;
    case ROW_HAPTICS:
      before = settings.haptics;
      settings.haptics = max(0, min(3, before + dir));
      if (settings.haptics != before) haptic_sample();  // the new strength, felt at once
      return;
    case ROW_SOUND:
      before = settings.sound;
      settings.sound = max(0, min(2, before + dir));
      if (settings.sound != before) settings_apply(), haptic_sample();
      return;
    case ROW_BRIGHTNESS: settings_adjust_brightness(dir); return;
    case ROW_SAVER: {
      int i = 0, n = sizeof(SAVER_STEPS);
      while (i < n && SAVER_STEPS[i] != settings.saver) i++;
      int next = max(0, min(n - 1, i + dir));
      if (next != i) settings.saver = SAVER_STEPS[next], haptic_tick();
      return;
    }
  }
}

void settings_page_refresh()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  for (int slot = 0; slot < VISIBLE; slot++) {
    int i = focus + slot - VISIBLE / 2;
    bool used = i >= 0 && i < (int)rows->size();
    set_text(names[slot], used ? row_name((*rows)[i]) : "");
    String value = used ? row_value((*rows)[i]) : "";
    if (used && editing && i == focus) value = "- " + value + " +";  // the row that has the crown
    set_text(values[slot], value);
  }
  bool deck = rows == &DECK_ROWS;
  if (editing) {
    set_text(hint, L("crown: change · tap: save", "колесо: змінити · тап: зберегти"));
    return;
  }
  set_text(hint, (*rows)[focus] == ROW_LINK  ? L("hold 2 s: forget the paired relay", "утримай 2 с: забути релей")
                 : (*rows)[focus] == ROW_LANG  ? L("tap: switch · the knob restarts", "тап: змінити (з перезапуском)")
                 : (*rows)[focus] == ROW_ABOUT ? L("BLE name: orca-deck-knob", L("BLE name: orca-deck-knob", "BLE-ім’я: orca-deck-knob"))
                 : deck                     ? L("tap: change · swipe left: back", "тап: змінити · свайп вліво: назад")
                                            : L("tap: change · swipe down: hub", "тап: змінити · свайп вниз: хаб"));
}

void settings_page_turn(int step)
{
  if (editing) {
    for (int i = 0; i < abs(step); i++) step_row((*rows)[focus], step > 0 ? 1 : -1);
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
  int row = (*rows)[focus];
  if (stepped(row)) {  // tap: the crown is the row's; tap again: saved, the crown picks rows again
    editing = !editing;
    if (!editing) {
      save();
      settings_apply();
      if (row == ROW_SAVER) buddy_input();  // the new wait starts from now
    }
    haptic_click();
    settings_page_refresh();
    return;
  }
  switch (row) {
    case ROW_LINK: return;  // forgetting takes a hold, never a tap
    case ROW_ABOUT: return;
    case ROW_WIFI: settings.wifi = !settings.wifi; break;
    case ROW_HIDE_INACTIVE: settings.hide_inactive = !settings.hide_inactive; break;
    case ROW_VACATION: pet_action(PET_VACATION); haptic_click(); settings_page_refresh(); return;
    case ROW_HUB:  // two values: one tap flips them, like a switch (user, 2026-11-16: the Apple Watch-like grid)
      settings.hub_grid = !settings.hub_grid;
      save();
      haptic_click();
      settings_page_refresh();
      return;
    case ROW_LANG: {  // saved, then a restart: every page is built again in the new language (titles set once included)
      settings.lang = !settings.lang;
      save();
      haptic_click(2);
      settings_page_refresh();
      lv_timer_create([](lv_timer_t *) { ESP.restart(); }, 700, NULL);
      return;
    }
    case ROW_WIFI_ADD: {  // the bridge asks the password in a dialog on the Mac; the W line brings the network here
      JsonDocument cmd;
      cmd["cmd"] = "wifi_add";
      push_command(cmd);
      wifi_asked_at = millis();
      haptic_click();
      settings_page_refresh();
      return;
    }
    default: return;
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
  set_text(title, L("SETTINGS", "НАЛАШТУВАННЯ"));
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
    set_text(title, deck_rows ? "CLAUDE" : L("SETTINGS", "НАЛАШТУВАННЯ"));
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
  return String(link_name(link)) + (ble_connected() ? L(" · paired", " · спарено") : "");
}

const App app_settings = {"Settings", LV_SYMBOL_SETTINGS, 0x9A9890, app_show, app_hide, app_subtitle,
                          settings_page_turn, settings_page_tap, settings_page_hold, nullptr};

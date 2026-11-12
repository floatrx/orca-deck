// Settings: stored in NVS, shown on the left page (swipe right from the main page), as in Apex.
// The crown moves between rows (the focused one sits in the middle), a tap changes the value or runs the action.
#include "deck.h"
#include <Preferences.h>
#include "lcd_bl_pwm_bsp.h"
#include "sound.h"

Settings settings;

static Preferences prefs;
static lv_obj_t *page, *title, *hint;
static const int VISIBLE = 5;
static lv_obj_t *names[VISIBLE], *values[VISIBLE];
static int focus = 0;
static uint32_t forget_armed_until = 0;  // "Forget relay" needs a second tap within 3 s

enum Row { ROW_LINK, ROW_WIFI, ROW_HIDE_INACTIVE, ROW_HIDE_COMPLETED, ROW_HAPTICS, ROW_SOUND, ROW_BRIGHTNESS, ROWS };

void settings_load()
{
  prefs.begin("deck", true);
  settings.wifi = prefs.getUChar("wifi", settings.wifi);
  settings.hide_inactive = prefs.getBool("hideInactive", settings.hide_inactive);
  settings.hide_completed = prefs.getUChar("hideDone", settings.hide_completed);
  settings.haptics = prefs.getUChar("haptics", settings.haptics);
  settings.sound = prefs.getUChar("sound", settings.sound);
  settings.brightness = prefs.getUChar("bright", settings.brightness);
  prefs.end();
}

static void save()
{
  prefs.begin("deck", false);
  prefs.putUChar("wifi", settings.wifi);
  prefs.putBool("hideInactive", settings.hide_inactive);
  prefs.putUChar("hideDone", settings.hide_completed);
  prefs.putUChar("haptics", settings.haptics);
  prefs.putUChar("sound", settings.sound);
  prefs.putUChar("bright", settings.brightness);
  prefs.end();
}

void settings_apply()
{
  setUpdutySubdivide(255 * settings.brightness / 100);
  sound_set_level(settings.sound);
}

static String row_name(int row)
{
  static const char *NAMES[] = {"Connection", "Wi-Fi", "Hide inactive", "Hide completed", "Haptics", "Sound",
                                "Brightness"};
  return NAMES[row];
}

static String row_value(int row)
{
  switch (row) {
    case ROW_LINK: {
      if (forget_armed_until && millis() < forget_armed_until) return "tap: forget";
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
    case ROW_BRIGHTNESS: return String(settings.brightness) + "%";
  }
  return "";
}

void settings_page_refresh()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  for (int slot = 0; slot < VISIBLE; slot++) {
    int row = focus + slot - VISIBLE / 2;
    bool used = row >= 0 && row < ROWS;
    set_text(names[slot], used ? row_name(row) : "");
    set_text(values[slot], used ? row_value(row) : "");
  }
  set_text(hint, focus == ROW_LINK ? "tap twice: forget the paired relay" : "tap to change · swipe ← back");
}

void settings_page_turn(int step)
{
  int next = focus + step;
  if (next < 0 || next >= ROWS) return;
  focus = next;
  forget_armed_until = 0;
  haptic_tick();
  settings_page_refresh();
}

void settings_page_tap()
{
  switch (focus) {
    case ROW_LINK:
      if (forget_armed_until && millis() < forget_armed_until) {
        forget_armed_until = 0;
        ble_forget();
        haptic_buzz(1);
      } else {
        forget_armed_until = millis() + 3000;
        haptic_click();
      }
      settings_page_refresh();
      return;
    case ROW_WIFI: settings.wifi = !settings.wifi; break;
    case ROW_HIDE_INACTIVE: settings.hide_inactive = !settings.hide_inactive; break;
    case ROW_HIDE_COMPLETED: settings.hide_completed = (settings.hide_completed + 1) % 3; break;
    case ROW_HAPTICS: settings.haptics = (settings.haptics + 1) % 4; break;
    case ROW_SOUND: settings.sound = (settings.sound + 1) % 3; break;
    case ROW_BRIGHTNESS: settings.brightness = settings.brightness >= 100 ? 40 : settings.brightness + 20; break;
  }
  save();
  settings_apply();
  haptic_sample();  // the new strength / sound level, felt at once
  settings_page_refresh();
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
  hint = make_label(page, &deck_14, C_MUTED, 122, 210);
}

void settings_page_show(bool show)
{
  if (show) {
    lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page);
    forget_armed_until = 0;
    settings_page_refresh();
  } else {
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  }
}

bool settings_page_open() { return page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN); }

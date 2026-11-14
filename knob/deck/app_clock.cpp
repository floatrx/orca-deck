// Clock, an app in the hub: the Mac's local time (the knob keeps UTC; the offset comes with the relay's M line and
// stays in NVS), the date, a seconds sweep round the rim, and the Mac's battery and CPU while the link is alive.
// Works with no link: the time alone.
#include "deck.h"
#include <time.h>

#define STALE_MS 10000  // Mac numbers older than this are shown as stale

static lv_obj_t *page, *sweep, *date_label, *time_label, *battery_icon, *mac_label;

static bool local_time(struct tm &out)
{
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // the clock is not set yet
  now += tz_offset_min() * 60;
  gmtime_r(&now, &out);
  return true;
}

static const char *battery_symbol(int pct, bool charging)
{
  if (charging) return LV_SYMBOL_CHARGE;
  if (pct >= 85) return LV_SYMBOL_BATTERY_FULL;
  if (pct >= 60) return LV_SYMBOL_BATTERY_3;
  if (pct >= 35) return LV_SYMBOL_BATTERY_2;
  if (pct >= 12) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

static String ago(uint32_t ms)
{
  uint32_t s = ms / 1000;
  if (s < 60) return String(s) + "s ago";
  if (s < 3600) return String(s / 60) + "m ago";
  return String(s / 3600) + "h ago";
}

static void paint_text()
{
  struct tm t;
  if (local_time(t)) {
    char hm[8], date[32];
    strftime(hm, sizeof(hm), "%H:%M", &t);
    strftime(date, sizeof(date), "%A %e %b", &t);
    set_text(time_label, hm);
    set_text(date_label, date);
  } else {
    set_text(time_label, "--:--");
    set_text(date_label, "waiting for the Mac's time");
  }

  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  if (!mac.at || mac.battery < 0) {
    set_text(battery_icon, "");
    set_text(mac_label, "");
    return;
  }
  uint32_t age = millis() - mac.at;
  bool stale = age > STALE_MS;
  String line = String(mac.battery) + "%";
  if (mac.cpu >= 0 && !stale) line += "    CPU " + String(mac.cpu) + "%";
  if (stale) line += " · " + ago(age);
  set_text(battery_icon, battery_symbol(mac.battery, mac.charging));
  set_text(mac_label, line);
  uint32_t c = stale ? C_MUTED : mac.charging ? C_DONE : mac.battery < 20 ? C_ATTENTION : C_DETAIL;
  lv_obj_set_style_text_color(battery_icon, lv_color_hex(c), 0);
  lv_obj_set_style_text_color(mac_label, lv_color_hex(stale ? C_MUTED : C_DETAIL), 0);
}

// Every frame while shown: the sweep moves smoothly; the text changes at most once a second.
void clock_tick()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  static uint32_t last_frame = 0, last_text = 0;
  uint32_t now = millis();
  if (now - last_frame < 50) return;
  last_frame = now;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  lv_arc_set_value(sweep, (int)((tv.tv_sec % 60) * 10 + tv.tv_usec / 100000));
  if (now - last_text >= 1000) {
    last_text = now;
    paint_text();
  }
}

static String subtitle()
{
  struct tm t;
  if (!local_time(t)) return "--:--";
  char hm[8];
  strftime(hm, sizeof(hm), "%H:%M", &t);
  String out = hm;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  if (mac.at && mac.battery >= 0 && millis() - mac.at < STALE_MS) out += " · Mac " + String(mac.battery) + "%";
  return out;
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint_text();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String clock_report()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  return String(lv_label_get_text(time_label)) + " tz=" + String(tz_offset_min()) + " mac: battery=" + String(mac.battery) +
         (mac.charging ? "+" : "") + " cpu=" + String(mac.cpu) + " volume=" + String(mac.volume) +
         (mac.muted ? " muted" : "") + " age=" + String(mac.at ? (millis() - mac.at) / 1000 : -1) + "s";
}

void clock_create()
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

  // The seconds sweep: a thin arc that fills the rim once a minute.
  sweep = lv_arc_create(page);
  lv_obj_set_size(sweep, 344, 344);
  lv_obj_center(sweep);
  lv_arc_set_rotation(sweep, 270);
  lv_arc_set_bg_angles(sweep, 0, 360);
  lv_arc_set_range(sweep, 0, 600);
  lv_obj_remove_style(sweep, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(sweep, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(sweep, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(sweep, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sweep, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sweep, lv_color_hex(C_WORKING), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(sweep, true, LV_PART_INDICATOR);

  date_label = make_label(page, &deck_16, C_MUTED, -56, 240);
  time_label = make_label(page, &lv_font_montserrat_48, C_INK, -8, 260);
  battery_icon = lv_label_create(page);
  lv_obj_set_style_text_font(battery_icon, &lv_font_montserrat_20, 0);
  lv_label_set_text(battery_icon, "");
  lv_obj_align(battery_icon, LV_ALIGN_CENTER, -70, 52);
  mac_label = make_label(page, &deck_16, C_DETAIL, 52, 200);
  lv_obj_set_style_text_align(mac_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(mac_label, LV_ALIGN_CENTER, 50, 52);
}

const App app_clock = {"Clock", LV_SYMBOL_REFRESH, C_WORKING, show, hide, subtitle, nullptr, nullptr, nullptr, nullptr};

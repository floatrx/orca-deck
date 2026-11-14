// Jog Wheel, an app in the hub (idea 4 in .work/notes/knob-app-ideas.md): the crown as a precise wheel for whatever
// app is in front on the Mac.
//   swipe left / right: mode (Scroll, Undo, Tabs, Zoom, Frame) · crown: act · tap: reset the count (Zoom: 100 %)
// A ruler of ticks round the rim turns with the crown; the centre counts how far you went ("−14 undos"). Steps go
// to the bridge in one `jog` every 80 ms (the bridge presses the keys or scrolls).
#include "deck.h"
#include <math.h>

#define SEND_MS 80
#define TICKS 48
#define RULER_R 160
#define COUNT_RESET_MS 2500  // the count starts over after the crown rests this long

struct Mode {
  const char *id, *name, *icon, *forward, *back;  // forward / back: what a + / − count reads as
  uint32_t color;
};
static const Mode MODES[] = {
    {"scroll", "Scroll", LV_SYMBOL_DOWN, "lines down", "lines up", 0x7FD1B9},
    {"undo", "Undo", LV_SYMBOL_LOOP, "redos", "undos", 0xFFB347},
    {"tab", "Tabs", LV_SYMBOL_LIST, "tabs right", "tabs left", 0x4CC3FF},
    {"zoom", "Zoom", LV_SYMBOL_PLUS, "zoom in", "zoom out", 0xB18CFF},
    {"frame", "Frame", LV_SYMBOL_VIDEO, "frames on", "frames back", 0xFF7A9A},
};
static const int MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

static lv_obj_t *page, *ticks[TICKS], *icon_label, *name_label, *count_label, *unit_label, *hint_label, *dots[MODE_COUNT];
static int mode = 1;  // Undo first: the new trick
static int pending = 0, count = 0;
static uint32_t send_at = 0, last_turn = 0;
static float angle = 0;  // the ruler's rotation, degrees

static void send_jog(int value)
{
  JsonDocument cmd;
  cmd["cmd"] = "jog";
  cmd["mode"] = MODES[mode].id;
  if (value) cmd["value"] = value;
  else cmd["reset"] = true;
  push_command(cmd);
}

static void place_ruler()
{
  for (int i = 0; i < TICKS; i++) {
    float a = (angle + i * 360.0f / TICKS - 90) * M_PI / 180;
    int s = lv_obj_get_width(ticks[i]);
    lv_obj_set_pos(ticks[i], 180 + RULER_R * cosf(a) - s / 2, 180 + RULER_R * sinf(a) - s / 2);
  }
}

static void paint()
{
  const Mode &m = MODES[mode];
  set_text(icon_label, m.icon);
  set_text(name_label, m.name);
  lv_obj_set_style_text_color(icon_label, lv_color_hex(m.color), 0);
  for (int i = 0; i < TICKS; i++) lv_obj_set_style_bg_color(ticks[i], lv_color_hex(i % 6 ? 0x3A3B40 : m.color), 0);
  for (int i = 0; i < MODE_COUNT; i++) {
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == mode ? m.color : 0x3A3B40), 0);
    lv_obj_set_size(dots[i], i == mode ? 10 : 6, i == mode ? 10 : 6);
  }
  if (count) {
    char n[12];
    snprintf(n, sizeof(n), "%+d", count);  // ASCII: Montserrat 48 has no U+2212 minus
    set_text(count_label, n);
    set_text(unit_label, count > 0 ? m.forward : m.back);
  } else {
    set_text(count_label, "0");
    set_text(unit_label, "turn the crown");
  }
  set_text(hint_label, !strcmp(m.id, "zoom") ? "swipe: mode · tap: 100 %" : "swipe: mode · tap: reset");
}

static void turn(int step)
{
  pending += step;
  count += step;
  last_turn = millis();
  if (!send_at) send_at = millis() + SEND_MS;
  angle += step * 360.0f / TICKS;  // one tick per detent: the ruler moves with the finger
  place_ruler();
  haptic_tick();
  paint();
}

static void tap()
{
  if (!strcmp(MODES[mode].id, "zoom")) send_jog(0);
  count = 0;
  haptic_click();
  paint();
}

static void swipe(lv_dir_t dir)
{
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
  mode = (mode + (dir == LV_DIR_LEFT ? 1 : MODE_COUNT - 1)) % MODE_COUNT;
  count = pending = 0;
  send_at = 0;
  haptic_click();
  paint();
}

// Every frame, shown or not: the batched steps go out; a rested crown starts the count over.
void jog_tick()
{
  uint32_t now = millis();
  if (send_at && now >= send_at) {
    send_at = 0;
    while (pending) {  // the bridge takes up to 20 a command
      int v = max(-20, min(20, pending));
      send_jog(v);
      pending -= v;
    }
  }
  if (count && now - last_turn > COUNT_RESET_MS && page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) {
    count = 0;
    paint();
  }
}

static String subtitle() { return MODES[mode].name; }

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String jog_report() { return String("mode=") + MODES[mode].id + " count=" + String(count) + " pending=" + String(pending); }

void jog_create()
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

  for (int i = 0; i < TICKS; i++) {  // the ruler: every sixth tick bigger, in the mode's colour
    lv_obj_t *t = ticks[i] = lv_obj_create(page);
    int s = i % 6 ? 5 : 9;
    lv_obj_set_size(t, s, s);
    lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_clear_flag(t, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  place_ruler();

  icon_label = lv_label_create(page);
  lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);
  lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -92);
  name_label = make_label(page, &deck_bold_16, C_INK, -60, 200);
  lv_obj_set_style_text_letter_space(name_label, 2, 0);
  count_label = make_label(page, &lv_font_montserrat_48, C_INK, -6, 200);
  unit_label = make_label(page, &deck_16, C_MUTED, 36, 220);
  hint_label = make_label(page, &deck_14, C_MUTED, 100, 200);
  for (int i = 0; i < MODE_COUNT; i++) {
    lv_obj_t *d = dots[i] = lv_obj_create(page);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(d, LV_ALIGN_CENTER, (i - MODE_COUNT / 2) * 16, 70);
  }
  paint();
}

const App app_jog = {"Jog", LV_SYMBOL_REFRESH, 0x7FD1B9, show, hide, subtitle, turn, tap, nullptr, swipe};

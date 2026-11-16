// Mic Mute, an app in the hub (idea 3 in .work/notes/knob-app-ideas.md): the Mac's microphone under a thumb, for
// calls in any app.
//   tap: mute / unmute · hold: the other way while the finger stays (push-to-talk when muted, a cough button when
//   live) · crown: the input level ±2 a detent
// The ring says it from across the desk: red and still while muted, green and breathing while live. The knob shows
// its own choice at once; the Mac's (M line: mi / mm / mv) takes over again LOCAL_MS after the last change.
#include "deck.h"
#include <math.h>

#define STEP 2
#define SEND_MS 120
#define LOCAL_MS 3000
#define C_MUTED_RED 0xFF4B4B

static lv_obj_t *page, *ring, *level_arc, *glyph, *state_label, *name_label, *level_label, *hint_label;
static bool muted = false, known = false, holding = false;
static int level = -1, pending = 0;
static uint32_t send_at = 0, local_until = 0;

// ---- the microphone glyph (no font here has one): a capsule, a cradle, a stem and a foot; a slash when muted ----

lv_obj_t *mic_glyph(lv_obj_t *parent, int size)
{
  lv_obj_t *g = lv_obj_create(parent);
  lv_obj_set_size(g, size, size);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g, 0, 0);
  lv_obj_set_style_pad_all(g, 0, 0);
  lv_obj_clear_flag(g, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  int line = max(2, size / 14);

  lv_obj_t *capsule = lv_obj_create(g);
  lv_obj_set_size(capsule, size * 30 / 100, size * 46 / 100);
  lv_obj_align(capsule, LV_ALIGN_TOP_MID, 0, size * 8 / 100);
  lv_obj_set_style_radius(capsule, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(capsule, 0, 0);
  lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  lv_obj_t *cradle = lv_arc_create(g);  // the U round the capsule's lower half
  int c = size * 52 / 100;
  lv_obj_set_size(cradle, c, c);
  lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, size * 22 / 100);
  lv_arc_set_bg_angles(cradle, 0, 180);
  lv_obj_remove_style(cradle, NULL, LV_PART_KNOB);
  lv_obj_remove_style(cradle, NULL, LV_PART_INDICATOR);
  lv_obj_clear_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(cradle, line, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(cradle, true, LV_PART_MAIN);
  lv_obj_set_style_pad_all(cradle, 0, 0);

  lv_obj_t *stem = lv_obj_create(g);
  lv_obj_set_size(stem, line, size * 14 / 100);
  lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 22 / 100 + c - 1);
  lv_obj_t *foot = lv_obj_create(g);
  lv_obj_set_size(foot, size * 28 / 100, line);
  lv_obj_align(foot, LV_ALIGN_TOP_MID, 0, size * 22 / 100 + c + size * 14 / 100 - 2);
  lv_obj_t *slash = lv_line_create(g);  // the mute bar, corner to corner
  lv_point_t *p = new lv_point_t[2];    // lv_line keeps the pointer: one pair per glyph, made once
  p[0] = {(lv_coord_t)(size * 18 / 100), (lv_coord_t)(size * 10 / 100)};
  p[1] = {(lv_coord_t)(size * 82 / 100), (lv_coord_t)(size * 90 / 100)};
  lv_line_set_points(slash, p, 2);
  lv_obj_set_style_line_width(slash, line + 1, 0);
  lv_obj_set_style_line_rounded(slash, true, 0);
  for (lv_obj_t *o : {stem, foot}) {
    lv_obj_set_style_radius(o, line / 2, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  return g;
}

void mic_glyph_set(lv_obj_t *g, uint32_t color, bool slashed)
{
  lv_color_t c = lv_color_hex(color);
  lv_obj_set_style_bg_color(lv_obj_get_child(g, 0), c, 0);
  lv_obj_set_style_arc_color(lv_obj_get_child(g, 1), c, LV_PART_MAIN);
  lv_obj_set_style_bg_color(lv_obj_get_child(g, 2), c, 0);
  lv_obj_set_style_bg_color(lv_obj_get_child(g, 3), c, 0);
  lv_obj_t *slash = lv_obj_get_child(g, 4);
  lv_obj_set_style_line_color(slash, c, 0);
  slashed ? lv_obj_clear_flag(slash, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(slash, LV_OBJ_FLAG_HIDDEN);
}

// ---- the app ----

static void send_mute(bool want)
{
  JsonDocument cmd;
  cmd["cmd"] = "mac";
  cmd["action"] = "inmute";
  cmd["value"] = want;
  push_command(cmd);
  muted = want;
  known = true;
  local_until = millis() + LOCAL_MS;
}

static void paint()
{
  if (!page) return;
  uint32_t c = !known ? C_IDLE : muted ? C_MUTED_RED : C_DONE;
  lv_obj_set_style_arc_color(ring, lv_color_hex(c), LV_PART_INDICATOR);
  mic_glyph_set(glyph, muted ? C_MUTED_RED : C_INK, muted);
  set_text(state_label, !known ? L("MIC", "МІКРОФОН") : holding ? (muted ? L("HUSHED", "ТИХО") : L("TALKING", "ГОВОРЮ")) : muted ? L("MUTED", "ВИМКНЕНО") : L("LIVE", "УВІМКНЕНО"));
  lv_obj_set_style_text_color(state_label, lv_color_hex(c), 0);
  lv_arc_set_value(level_arc, max(0, level));
  set_text(level_label, level >= 0 ? L("level ", "рівень ") + String(level) + "%" : "");
}

static void take_mac()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  set_text(name_label, mac.at ? (mac.mic.length() ? mac.mic : L("no microphone", "немає мікрофона")) : L("waiting for the Mac…", "чекаю на Мак…"));
  if (millis() < local_until || holding || !mac.at || !mac.mic.length()) return;
  known = true;
  muted = mac.mic_muted;
  if (!pending) level = mac.mic_volume;
}

static void tap()
{
  if (!known) return;
  send_mute(!muted);
  haptic_click(muted ? 2 : 1);  // two for muted: the safe side is the stronger click
  paint();
}

bool mic_is_muted() { return known && muted; }

// Control Center's Mic tile: the same toggle as a tap here, from the Mac's latest state.
void mic_toggle()
{
  take_mac();
  tap();
}

static void hold()
{
  if (!known || holding) return;
  holding = true;
  send_mute(!muted);
  haptic_click();
  paint();
}

static void turn(int step)
{
  if (level < 0) return;
  int next = max(0, min(100, level + step * STEP));
  if (next == level) return;
  pending += next - level;
  level = next;
  local_until = millis() + LOCAL_MS;
  haptic_tick();
  paint();
}

static float breath = 0;

void micmute_tick()
{
  uint32_t now = millis();
  if (holding && !hub_pressing()) {  // the finger is up: back the way it was
    holding = false;
    send_mute(!muted);
    haptic_click();
    if (page) paint();
  }
  if (pending && now - send_at >= SEND_MS) {
    JsonDocument cmd;
    cmd["cmd"] = "mac";
    cmd["action"] = "involume";
    cmd["value"] = pending;
    push_command(cmd);
    pending = 0;
    send_at = now;
  }
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  static uint32_t last_mac = 0, last_frame = 0;
  if (now - last_mac > 500) {
    last_mac = now;
    take_mac();
    paint();
  }
  if (now - last_frame < 33) return;  // the breath, 30 fps: live breathes, muted holds still
  last_frame = now;
  breath = known && !muted ? 0.5f + 0.5f * sinf(now / 2400.0f * 2 * M_PI) : 1.0f;
  lv_obj_set_style_arc_opa(ring, (lv_opa_t)(110 + 145 * breath), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ring, known && !muted ? 10 + (int)(6 * breath) : 14, LV_PART_INDICATOR);
}

static String subtitle()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool m = shared.mac.mic_muted, has = shared.mac.at && shared.mac.mic.length();
  xSemaphoreGive(shared_mux);
  return !has ? L("the Mac's microphone", "мікрофон Мака") : m ? L("muted", "вимкнено") : L("live", "увімкнено");
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  take_mac();
  paint();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String micmute_report()
{
  return String("known=") + known + " muted=" + muted + " holding=" + holding + " level=" + level + " | " +
         lv_label_get_text(state_label) + " | " + lv_label_get_text(name_label);
}

void micmute_create()
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

  ring = lv_arc_create(page);
  lv_obj_set_size(ring, 336, 336);
  lv_obj_center(ring);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 100);
  lv_arc_set_value(ring, 100);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 14, LV_PART_INDICATOR);

  level_arc = lv_arc_create(page);  // the input level: a thin dial inside the ring, open at the bottom
  lv_obj_set_size(level_arc, 300, 300);
  lv_obj_center(level_arc);
  lv_arc_set_rotation(level_arc, 135);
  lv_arc_set_bg_angles(level_arc, 0, 270);
  lv_arc_set_range(level_arc, 0, 100);
  lv_obj_remove_style(level_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(level_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(level_arc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(level_arc, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_width(level_arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(level_arc, lv_color_hex(C_DETAIL), LV_PART_INDICATOR);

  glyph = mic_glyph(page, 110);
  lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -42);
  state_label = make_label(page, &deck_bold_30, C_IDLE, 38, 220);
  name_label = make_label(page, &deck_14, C_MUTED, 70, 220);
  level_label = make_label(page, &deck_14, C_DETAIL, 92, 200);
  hint_label = make_label(page, &deck_14, C_MUTED, 128, 170);
  set_text(hint_label, L("tap: mute · hold: talk", "тап: вимкнути · утримай: говорити"));
  paint();
}

const App app_mic = {"Mic", LV_SYMBOL_AUDIO, C_MUTED_RED, show, hide, subtitle, turn, tap, hold, nullptr};

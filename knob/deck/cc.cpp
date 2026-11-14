// Control Center (user, 2026-11-14: the free swipe up): the switches used most, without opening their apps.
// Swipe up in the hub or any app but Claude (its swipe up is the next agent); swipe down closes.
//   tiles: Mic (mute) · Focus (Mac Focus + Pomodoro) · Sound (the Mac's mute) · Brightness (40 → 100 %)
//   crown: the tile · tap: the tile under the finger (or the focused one) · top: Claude limits · bottom: the Mac
// It slides up from the bottom edge, as on the watch.
#include "deck.h"

#define TILE_W 112
#define TILE_H 84
#define GAP 10
#define LOCAL_MS 3000  // a tile shows its own choice this long, then the Mac's state again

enum { T_MIC, T_FOCUS, T_SOUND, T_BRIGHT, TILES };
static const char *const NAMES[TILES] = {"Mic", "Focus", "Sound", "Bright"};

static lv_obj_t *page, *tiles[TILES], *icons[TILES], *labels[TILES], *mic_icon, *top_label, *bottom_label;
static int focus = 0;
static bool shown = false;
static bool sound_muted = false;
static uint32_t sound_until = 0;

bool cc_is_open() { return shown; }

static void slide(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }
static void hide_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

static uint32_t lit_ink(bool live, bool muted) { return live || muted ? C_BG : C_INK; }  // dark on a lit tile

static void paint()
{
  if (!page) return;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  if (millis() >= sound_until) sound_muted = mac.muted;
  bool mic_muted = mic_is_muted();
  bool on[TILES] = {mac.at && !mic_muted, focus_is_on(), !sound_muted, true};
  uint32_t colors[TILES] = {mic_muted ? 0xFF4B4B : C_DONE, 0x8E7CFF, C_WORKING, 0xFFD166};
  String text[TILES] = {
      !mac.at ? "Mic" : mic_muted ? "Mic off" : "Mic live",
      focus_is_on() ? "Focus on" : "Focus",
      mac.volume < 0 ? "Sound" : sound_muted ? "Muted" : String(mac.volume) + "%",
      String(settings.brightness) + "%",
  };
  for (int i = 0; i < TILES; i++) {
    bool lit = on[i] || (i == T_MIC && mic_muted);  // a muted mic is news: its tile is red, not dark
    lv_obj_set_style_bg_color(tiles[i], lv_color_hex(lit ? colors[i] : 0x1C1D21), 0);
    lv_obj_set_style_bg_opa(tiles[i], lit ? (i == T_BRIGHT ? LV_OPA_30 : LV_OPA_COVER) : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tiles[i], lv_color_hex(i == focus ? C_INK : 0x2C2D31), 0);
    uint32_t ink = lit && i != T_BRIGHT ? C_BG : C_INK;
    lv_obj_set_style_text_color(labels[i], lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(icons[i], lv_color_hex(ink), 0);
    set_text(labels[i], text[i]);
  }
  mic_glyph_set(mic_icon, lit_ink(mac.at && !mic_muted, mic_muted), mic_muted);
  set_text(icons[T_SOUND], sound_muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
  String limits = claude_limits(true);
  set_text(top_label, limits.length() ? "Claude " + limits : "");
  String bottom;
  if (mac.battery >= 0) bottom = "battery " + String(mac.battery) + "%" + (mac.charging ? " " LV_SYMBOL_CHARGE : "");
  if (mac.cpu >= 0) bottom += String(bottom.length() ? " · " : "") + "CPU " + String(mac.cpu) + "%";
  set_text(bottom_label, mac.at ? bottom : "waiting for the Mac…");
}

void cc_open()
{
  if (shown || !page) return;
  shown = true;
  focus = 0;
  paint();
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  lv_anim_set_values(&a, 360, 0);
  lv_anim_set_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, slide);
  lv_anim_start(&a);
  haptic_click();
}

void cc_close(bool quiet)
{
  if (!shown) return;
  shown = false;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  lv_anim_set_values(&a, lv_obj_get_y(page), 360);
  lv_anim_set_time(&a, 200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&a, slide);
  lv_anim_set_ready_cb(&a, hide_done);
  lv_anim_start(&a);
  if (!quiet) haptic_home();
}

void cc_turn(int step)
{
  int next = focus + step;
  if (next < 0 || next >= TILES) return;
  focus = next;
  haptic_tick();
  paint();
}

static void act(int i)
{
  focus = i;
  if (i == T_MIC) {
    mic_toggle();
  } else if (i == T_FOCUS) {
    focus_toggle();
  } else if (i == T_SOUND) {
    JsonDocument cmd;
    cmd["cmd"] = "mac";
    cmd["action"] = "mute";
    push_command(cmd);
    sound_muted = !sound_muted;
    sound_until = millis() + LOCAL_MS;
    haptic_click();
  } else {
    settings_step_brightness();
    haptic_click();
  }
  paint();
}

void cc_tap()
{
  // The tile under the finger; the focused one when the tap came without a point (the T test channel).
  lv_indev_t *indev = lv_indev_get_act();
  if (indev) {
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    for (int i = 0; i < TILES; i++) {
      lv_area_t r;
      lv_obj_get_coords(tiles[i], &r);
      if (p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2) return act(i);
    }
    return;  // between the tiles: nothing (the hub ticks)
  }
  act(focus);
}

void cc_tick()
{
  static uint32_t last = 0;
  if (!shown || millis() - last < 500) return;
  last = millis();
  paint();
}

String cc_report()
{
  String out = String("cc focus=") + NAMES[focus];
  for (int i = 0; i < TILES; i++) out += String(" | ") + lv_label_get_text(labels[i]);
  return out + " | " + lv_label_get_text(top_label) + " | " + lv_label_get_text(bottom_label);
}

void cc_create()
{
  page = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page, 360, 360);
  lv_obj_set_pos(page, 0, 360);
  lv_obj_set_style_bg_color(page, lv_color_hex(0x0B0C0E), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *grabber = lv_obj_create(page);  // the sheet's handle: it came up, it goes down
  lv_obj_set_size(grabber, 40, 5);
  lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_set_style_radius(grabber, 3, 0);
  lv_obj_set_style_border_width(grabber, 0, 0);
  lv_obj_set_style_bg_color(grabber, lv_color_hex(0x3A3B40), 0);

  for (int i = 0; i < TILES; i++) {
    lv_obj_t *t = tiles[i] = lv_obj_create(page);
    lv_obj_set_size(t, TILE_W, TILE_H);
    int col = i % 2, row = i / 2;
    lv_obj_align(t, LV_ALIGN_CENTER, (col ? 1 : -1) * (TILE_W + GAP) / 2, (row ? 1 : -1) * (TILE_H + GAP) / 2);
    lv_obj_set_style_radius(t, 22, 0);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_clear_flag(t, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    icons[i] = lv_label_create(t);
    lv_obj_set_style_text_font(icons[i], &lv_font_montserrat_28, 0);
    lv_obj_align(icons[i], LV_ALIGN_CENTER, 0, -12);
    labels[i] = lv_label_create(t);
    lv_obj_set_style_text_font(labels[i], &deck_14, 0);
    lv_obj_align(labels[i], LV_ALIGN_BOTTOM_MID, 0, -8);
  }
  lv_label_set_text(icons[T_MIC], "");
  mic_icon = mic_glyph(tiles[T_MIC], 34);
  lv_obj_align(mic_icon, LV_ALIGN_CENTER, 0, -12);
  lv_label_set_text(icons[T_FOCUS], LV_SYMBOL_EYE_CLOSE);
  lv_label_set_text(icons[T_SOUND], LV_SYMBOL_VOLUME_MAX);
  lv_label_set_text(icons[T_BRIGHT], LV_SYMBOL_IMAGE);

  top_label = make_label(page, &deck_14, C_DETAIL, -118, 220);
  bottom_label = make_label(page, &deck_14, C_MUTED, 118, 200);
}

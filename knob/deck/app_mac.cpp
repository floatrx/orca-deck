// Mac remote, an app in the hub: the Mac's output volume on the crown, media keys on taps and swipes.
//   crown: volume ±2 per detent · tap: play / pause · swipe left: next track · swipe right: previous · hold: mute
// The volume moves on the knob at once; the crown's steps go to the bridge in one `mac volume` every 120 ms, and
// the Mac's own number (M line) takes over again 1.5 s after the last turn. What plays: macOS 15+ closed
// MediaRemote to third parties, so the bridge asks the players: Music, Spotify, a YouTube tab (the M line's
// source / title).
#include "deck.h"

#define STEP 2
#define SEND_MS 120
#define SETTLE_MS 1500

static lv_obj_t *page, *gauge, *value_label, *unit_label, *icon_label, *output_label, *flash_label, *hint_label, *source_label,
    *title_label;
static String now_playing;  // "YouTube: Lynnic - Gravity" for the hub's subtitle
static int volume = -1;  // shown volume; -1: unknown yet
static bool muted = false;
static int pending = 0;  // crown steps not sent yet, in percent points
static uint32_t send_at = 0, local_until = 0, flash_until = 0;

static void send(const char *action, int value = 0)
{
  JsonDocument cmd;
  cmd["cmd"] = "mac";
  cmd["action"] = action;
  if (!strcmp(action, "volume")) cmd["value"] = value;
  push_command(cmd);
}

static void paint()
{
  if (!page) return;
  bool known = volume >= 0;
  set_text(value_label, known ? String(volume) : "--");
  lv_arc_set_value(gauge, known ? volume : 0);
  uint32_t c = muted ? C_IDLE : C_WORKING;
  lv_obj_set_style_arc_color(gauge, lv_color_hex(c), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(value_label, lv_color_hex(muted ? C_MUTED : C_INK), 0);
  set_text(icon_label, muted ? LV_SYMBOL_MUTE : volume == 0 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
  lv_obj_set_style_text_color(icon_label, lv_color_hex(c), 0);
}

// A big symbol that fades out: the media key went (there is no play state to show).
static void flash(const char *symbol)
{
  set_text(flash_label, symbol);
  lv_obj_set_style_opa(flash_label, LV_OPA_COVER, 0);
  lv_obj_clear_flag(flash_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(value_label, LV_OBJ_FLAG_HIDDEN);
  flash_until = millis() + 700;
}

static void turn(int step)
{
  if (volume < 0) return;
  int next = max(0, min(100, volume + step * STEP));
  if (next == volume) return;
  pending += next - volume;
  volume = next;
  if (muted && step > 0) muted = false;  // the bridge unmutes on a raise too
  if (!send_at) send_at = millis() + SEND_MS;
  local_until = millis() + SETTLE_MS;
  haptic_tick();
  paint();
}

static void tap()
{
  send("play");
  haptic_click();
  flash(LV_SYMBOL_PLAY " " LV_SYMBOL_PAUSE);
}

static void hold()
{
  send("mute");
  muted = !muted;
  local_until = millis() + SETTLE_MS;
  haptic_click(2);
  paint();
}

static void swipe(lv_dir_t dir)
{
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
  send(dir == LV_DIR_LEFT ? "next" : "prev");
  haptic_click();
  flash(dir == LV_DIR_LEFT ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV);
}

// Every frame, shown or not: the batched volume goes out; the Mac's numbers come in when the crown rests.
void mac_tick()
{
  uint32_t now = millis();
  if (send_at && now >= send_at) {
    send_at = 0;
    if (pending) send("volume", pending);
    pending = 0;
  }
  if (flash_until && now >= flash_until) {
    flash_until = 0;
    lv_obj_add_flag(flash_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(value_label, LV_OBJ_FLAG_HIDDEN);
  } else if (flash_until) {
    lv_obj_set_style_opa(flash_label, (lv_opa_t)min<uint32_t>(255, (flash_until - now) * 255 / 400), 0);
  }
  static uint32_t last = 0;
  if (now - last < 250 || !shared_mux) return;
  last = now;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  if (mac.at && now >= local_until && !pending) {
    if (mac.volume != volume || mac.muted != muted) {
      volume = mac.volume;
      muted = mac.muted;
      paint();
    }
  }
  now_playing = mac.title.length() ? mac.source + ": " + mac.title : "";
  if (page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) {
    set_text(output_label, mac.output);
    set_text(source_label, mac.title.length() ? mac.source : "");
    set_text(title_label, mac.title);
  }
}

static String subtitle()
{
  if (volume < 0) return "no Mac yet";
  if (now_playing.length()) return now_playing;
  return muted ? "muted" : "volume " + String(volume) + "%";
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String mac_report()
{
  return "volume=" + String(volume) + (muted ? " muted" : "") + " pending=" + String(pending);
}

void mac_create()
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

  // The gauge: a 270° arc open at the bottom, as a hi-fi volume dial.
  gauge = lv_arc_create(page);
  lv_obj_set_size(gauge, 320, 320);
  lv_obj_center(gauge);
  lv_arc_set_rotation(gauge, 135);
  lv_arc_set_bg_angles(gauge, 0, 270);
  lv_arc_set_range(gauge, 0, 100);
  lv_obj_remove_style(gauge, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(gauge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(gauge, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_color(gauge, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_width(gauge, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(gauge, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(gauge, true, LV_PART_MAIN);

  icon_label = lv_label_create(page);
  lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);
  lv_label_set_text(icon_label, "");
  lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -64);
  value_label = make_label(page, &lv_font_montserrat_48, C_INK, -8, 160);
  unit_label = make_label(page, &deck_16, C_MUTED, 30, 100);
  set_text(unit_label, "volume");
  flash_label = make_label(page, &lv_font_montserrat_48, C_WORKING, -8, 220);
  lv_obj_add_flag(flash_label, LV_OBJ_FLAG_HIDDEN);
  output_label = make_label(page, &deck_14, C_MUTED, 30, 160);
  lv_obj_add_flag(unit_label, LV_OBJ_FLAG_HIDDEN);  // the output device's name sits there now
  // What plays: the source small, the title scrolling round, inside the dial's open bottom.
  source_label = make_label(page, &deck_bold_16, 0xB18CFF, 70, 180);
  lv_obj_set_style_text_letter_space(source_label, 1, 0);
  title_label = make_label(page, &deck_16, C_INK, 94, 230);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  hint_label = make_label(page, &deck_14, C_MUTED, 134, 170);
  set_text(hint_label, "tap play · swipe track");
  paint();
}

const App app_mac = {"Mac", LV_SYMBOL_VOLUME_MAX, 0xB18CFF, show, hide, subtitle, turn, tap, hold, swipe};

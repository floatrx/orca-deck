// Focus Mode, an app in the hub (idea 13 in .work/notes/knob-app-ideas.md): one tap into deep work.
//   tap: on — the Mac's Focus (a Shortcut the user made: "Knob Focus On"), the Timer's Pomodoro (25 min focus, then a
//   5 min break), and quiet banners (a fresh done neither buzzes nor banners; needs-you still does) · tap again: off.
// When the Pomodoro's break ends, Focus turns itself off. Entering closes an iris on the screen; while on, a calm
// ring shows the time left.
#include "deck.h"

#define FOCUS_COLOR 0x8E7CFF

static lv_obj_t *page, *iris, *state_label, *time_label, *hint_label, *error_label;
static bool on = false;
static uint32_t on_since = 0;

bool focus_is_on() { return on; }

static void mac_focus(bool want)
{
  JsonDocument cmd;
  cmd["cmd"] = "focus";
  cmd["on"] = want;
  push_command(cmd);
}

static void set_width(void *obj, int32_t w) { lv_obj_set_style_arc_width((lv_obj_t *)obj, w, LV_PART_INDICATOR); }

static void paint()
{
  if (!page) return;
  uint32_t left = timer_left_ms(), total = max(timer_total_ms(), (uint32_t)1);
  bool brk = timer_on_break();
  lv_arc_set_value(iris, on ? (int)(1000ULL * left / total) : 1000);
  lv_obj_set_style_arc_color(iris, lv_color_hex(on ? (brk ? C_DONE : FOCUS_COLOR) : 0x2C2D31), LV_PART_INDICATOR);
  set_text(state_label, on ? (brk ? "BREAK" : "FOCUS") : "FOCUS");
  lv_obj_set_style_text_color(state_label, lv_color_hex(on ? (brk ? C_DONE : FOCUS_COLOR) : C_MUTED), 0);
  uint32_t s = (left + 999) / 1000;
  char t[12];
  snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  set_text(time_label, on ? t : "25:00");
  lv_obj_set_style_text_color(time_label, lv_color_hex(on ? C_INK : C_MUTED), 0);
  set_text(hint_label, on ? "tap: stop" : "tap: Mac Focus + Pomodoro");
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  String error = shared.mac.focus_error;
  xSemaphoreGive(shared_mux);
  set_text(error_label, error);
}

static void turn_on()
{
  on = true;
  on_since = millis();
  mac_focus(true);
  timer_focus(true);
  haptic_click(2);
  // The iris closes: the ring thickens from a hairline to a band in 600 ms, then settles.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, iris);
  lv_anim_set_values(&a, 2, 34);
  lv_anim_set_time(&a, 600);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, set_width);
  lv_anim_set_playback_time(&a, 500);  // and opens back to a calm 10 px ring
  lv_anim_start(&a);
  paint();
}

static void turn_off(bool by_timer)
{
  on = false;
  mac_focus(false);
  if (!by_timer) timer_focus(false);
  lv_obj_set_style_arc_width(iris, 10, LV_PART_INDICATOR);
  if (by_timer) banner_show_app(&app_focus, "round over · Focus off");
  haptic_click();
  paint();
}

static void tap() { on ? turn_off(false) : turn_on(); }
void focus_toggle() { tap(); }  // Control Center's Focus tile

// Every frame: the ring and time while shown; Focus ends with the Pomodoro.
void focus_tick()
{
  if (on && !timer_running() && millis() - on_since > 2000) turn_off(true);  // the break is over
  static uint32_t last = 0;
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN) || millis() - last < 250) return;
  last = millis();
  paint();
}

static String subtitle()
{
  if (!on) return "Mac Focus + Pomodoro";
  uint32_t s = (timer_left_ms() + 999) / 1000;
  return String(timer_on_break() ? "break · " : "on · ") + String(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + String(s % 60);
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String focus_report() { return String("focus=") + (on ? "on" : "off") + " timer " + timer_report(); }

void focus_create()
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

  iris = lv_arc_create(page);
  lv_obj_set_size(iris, 300, 300);
  lv_obj_center(iris);
  lv_arc_set_rotation(iris, 270);
  lv_arc_set_bg_angles(iris, 0, 360);
  lv_arc_set_range(iris, 0, 1000);
  lv_obj_remove_style(iris, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(iris, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(iris, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_color(iris, lv_color_hex(0x15161A), LV_PART_MAIN);
  lv_obj_set_style_arc_width(iris, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(iris, true, LV_PART_INDICATOR);

  state_label = make_label(page, &deck_bold_16, C_MUTED, -56, 200);
  lv_obj_set_style_text_letter_space(state_label, 3, 0);
  time_label = make_label(page, &lv_font_montserrat_48, C_MUTED, -6, 220);
  hint_label = make_label(page, &deck_14, C_MUTED, 44, 220);
  error_label = make_label(page, &deck_14, C_ATTENTION, 70, 220);
  lv_label_set_long_mode(error_label, LV_LABEL_LONG_WRAP);
  paint();
}

const App app_focus = {"Focus", LV_SYMBOL_EYE_CLOSE, FOCUS_COLOR, show, hide, subtitle, nullptr, tap, nullptr, nullptr};

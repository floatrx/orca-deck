// Timer / Pomodoro, an app in the hub. It lives on the knob alone (no Mac needed) and keeps counting in the
// background: the end buzzes and shows a banner wherever the user is.
//   crown: minutes (1–90) while stopped · tap: start / pause · hold: reset · tap twice: Pomodoro on / off
// Pomodoro: 25 min focus, then a 5 min break starts by itself, then it stops, ready for the next round.
#include "deck.h"

#define TOMATO 0xFF5A4E
#define FOCUS_MIN 25
#define BREAK_MIN 5
#define DOUBLE_TAP_MS 260

enum Phase { STOPPED, RUNNING, PAUSED };

static lv_obj_t *page, *ring, *mode_label, *time_label, *hint_label, *rounds_label;
static Phase phase = STOPPED;
static bool pomodoro = false, on_break = false;
static int minutes = 25;          // the set length while stopped (plain timer)
static uint32_t total_ms = 0;     // length of the running stretch
static uint32_t end_at = 0;       // millis() when it runs out (RUNNING)
static uint32_t left_ms = 0;      // what is left (PAUSED)
static uint32_t single_tap_at = 0;
static int rounds = 0;            // finished focus stretches today (since boot)

static uint32_t remaining()
{
  if (phase == RUNNING) return end_at > millis() ? end_at - millis() : 0;
  if (phase == PAUSED) return left_ms;
  return (pomodoro ? FOCUS_MIN : minutes) * 60000UL;
}

static String clock_text(uint32_t ms)
{
  uint32_t s = (ms + 999) / 1000;
  char out[12];
  snprintf(out, sizeof(out), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return out;
}

static uint32_t color()
{
  if (on_break) return C_DONE;
  return phase == STOPPED ? C_MUTED : TOMATO;
}

static void paint()
{
  if (!page) return;
  uint32_t left = remaining();
  set_text(time_label, clock_text(left));
  set_text(mode_label, on_break ? "BREAK" : pomodoro ? "FOCUS" : "TIMER");
  lv_obj_set_style_text_color(mode_label, lv_color_hex(color()), 0);
  lv_obj_set_style_text_color(time_label, lv_color_hex(phase == PAUSED ? C_MUTED : C_INK), 0);
  lv_obj_set_style_arc_color(ring, lv_color_hex(color()), LV_PART_INDICATOR);
  // The ring empties as the time runs out; while stopped it shows the set length out of 90 min.
  int value = phase == STOPPED ? (pomodoro ? FOCUS_MIN : minutes) * 1000 / 90 : (int)(1000ULL * left / max(total_ms, (uint32_t)1));
  lv_arc_set_value(ring, value);
  set_text(hint_label, phase == STOPPED  ? (pomodoro ? "tap: focus · tap twice: plain timer" : "crown: minutes · tap: start")
                       : phase == PAUSED ? "tap: go on · hold: reset"
                                         : "tap: pause · hold: reset");
  set_text(rounds_label, pomodoro && rounds ? String(rounds) + (rounds == 1 ? " round" : " rounds") : "");
}

static void start(uint32_t ms)
{
  total_ms = ms;
  end_at = millis() + ms;
  phase = RUNNING;
}

static void reset()
{
  phase = STOPPED;
  on_break = false;
}

static void finished()
{
  haptic_buzz(3);
  if (pomodoro && !on_break) {
    rounds++;
    on_break = true;
    start(BREAK_MIN * 60000UL);
    banner_show_app(&app_timer, "focus done · break");
  } else {
    bool was_break = on_break;
    reset();
    banner_show_app(&app_timer, was_break ? "break over" : "time's up");
  }
  paint();
}

static void single_tap()
{
  if (phase == STOPPED) start(remaining());
  else if (phase == RUNNING) left_ms = remaining(), phase = PAUSED;
  else {
    uint32_t whole = total_ms;  // the ring keeps measuring against the whole stretch
    start(left_ms);
    total_ms = whole;
  }
  haptic_click();
  paint();
}

static void tap()
{
  if (single_tap_at) {  // the second tap of a double tap: Pomodoro on / off (only while stopped)
    single_tap_at = 0;
    if (phase != STOPPED) return;
    pomodoro = !pomodoro;
    haptic_click(2);
    paint();
    return;
  }
  single_tap_at = millis() + DOUBLE_TAP_MS;
}

static void hold()
{
  single_tap_at = 0;
  reset();
  haptic_click(2);
  paint();
}

static void turn(int step)
{
  if (phase != STOPPED || pomodoro) return;
  int next = max(1, min(90, minutes + step));
  if (next == minutes) return;
  minutes = next;
  haptic_tick();
  paint();
}

// Every frame, shown or not: the time runs out in the background too.
void timer_tick()
{
  uint32_t now = millis();
  if (single_tap_at && now >= single_tap_at) {
    single_tap_at = 0;
    single_tap();
  }
  if (phase == RUNNING && now >= end_at) finished();
  static uint32_t last = 0;
  if (page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN) && now - last >= 250) {
    last = now;
    paint();
  }
}

static String subtitle()
{
  if (phase == RUNNING) return clock_text(remaining()) + (on_break ? " break" : " left");
  if (phase == PAUSED) return "paused · " + clock_text(left_ms);
  return pomodoro ? "pomodoro 25 / 5" : String(minutes) + " min";
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint();
}

static void hide()
{
  single_tap_at = 0;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

void timer_focus(bool on)
{
  single_tap_at = 0;
  if (on) {
    pomodoro = true;
    on_break = false;
    start(FOCUS_MIN * 60000UL);
  } else {
    reset();
  }
  paint();
}

uint32_t timer_left_ms() { return remaining(); }
uint32_t timer_total_ms() { return total_ms; }
bool timer_running() { return phase == RUNNING; }
bool timer_on_break() { return on_break; }

String timer_report()
{
  return String(phase == RUNNING ? "running" : phase == PAUSED ? "paused" : "stopped") + " " + clock_text(remaining()) +
         (pomodoro ? (on_break ? " break" : " focus") : "") + " rounds=" + String(rounds);
}

void timer_create()
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
  lv_obj_set_size(ring, 340, 340);
  lv_obj_center(ring);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 1000);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);

  mode_label = make_label(page, &deck_bold_16, C_MUTED, -64, 200);
  lv_obj_set_style_text_letter_space(mode_label, 3, 0);
  time_label = make_label(page, &lv_font_montserrat_48, C_INK, -10, 260);
  rounds_label = make_label(page, &deck_14, C_MUTED, 34, 200);
  hint_label = make_label(page, &deck_14, C_MUTED, 92, 230);
  paint();
}

const App app_timer = {"Timer", LV_SYMBOL_BELL, TOMATO, show, hide, subtitle, turn, tap, hold, nullptr};

// The hub: the knob's home screen, a watchOS-style list of apps, and the input router. The knob boots into it;
// the crown moves the list (the centre row is big, the rows toward the rim shrink and fade), a tap opens the app.
// Swipe down in any app comes back here, to the same row. .work/plans/knob-hub.md
// Behind the list the Earth turns (globe.cpp).
#include "deck.h"

static const App *const APPS[] = {&app_deck, &app_timer, &app_clock, &app_settings};
static const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

static const int SLOTS = 5;  // rows on screen: the centre one and two on each side
static lv_obj_t *page, *link_label;
static lv_obj_t *rows[SLOTS], *badges[SLOTS], *icons[SLOTS], *names[SLOTS], *subs[SLOTS];
static int focus = 0;
static const App *current = nullptr;  // nullptr: the hub is on screen
static bool pressed = false;
static bool gestured = false;  // this touch was a swipe: its release must not count as a tap
static uint32_t test_press_until = 0;

// Per slot, from the centre outward: row height offset, badge size, opacity.
static const int SLOT_Y[SLOTS] = {-126, -70, 0, 70, 126};
static const int BADGE[SLOTS] = {30, 40, 60, 40, 30};
static const lv_opa_t OPA[SLOTS] = {LV_OPA_30, LV_OPA_60, LV_OPA_COVER, LV_OPA_60, LV_OPA_30};

static void refresh()
{
  // shared_mux comes with link_init(), after the UI is built: the first refresh waits for hub_tick.
  if (!page || !shared_mux || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  for (int slot = 0; slot < SLOTS; slot++) {
    int i = focus + slot - SLOTS / 2;
    if (i < 0 || i >= APP_COUNT) {
      lv_obj_add_flag(rows[slot], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const App *app = APPS[i];
    lv_obj_clear_flag(rows[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(badges[slot], lv_color_hex(app->color), 0);
    set_text(icons[slot], app->icon);
    set_text(names[slot], app->name);
    set_text(subs[slot], slot == SLOTS / 2 && app->subtitle ? app->subtitle() : "");
  }
  String problem = status_problem();
  set_text(link_label, problem.length() ? "• " + problem : "");
  lv_obj_set_style_text_color(link_label, lv_color_hex(C_ATTENTION), 0);
}

static void open_app(int i);

void hub_open(const App *app)
{
  for (int i = 0; i < APP_COUNT; i++) {
    if (APPS[i] != app) continue;
    if (current == app) return;
    if (current) current->hide();
    current = nullptr;
    open_app(i);
  }
}

static void open_app(int i)
{
  if (i < 0 || i >= APP_COUNT) return;
  focus = i;
  current = APPS[i];
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  haptic_click();
  current->show();
}

void hub_home()
{
  if (current) {
    current->hide();
    current = nullptr;
  }
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  refresh();
}

bool hub_is_open() { return current == nullptr; }
const App *hub_current() { return current; }
bool hub_pressing() { return pressed || millis() < test_press_until; }

// Any input while the screensaver is up only wakes the knob, into the hub.
static bool wake()
{
  buddy_input();
  if (!buddy_is_shown()) return false;
  buddy_hide();
  haptic_click();
  hub_home();
  return true;
}

void hub_turn(int step)
{
  if (wake()) return;
  if (current) {
    if (current->turn) current->turn(step);
    return;
  }
  int next = focus + step;
  if (next < 0 || next >= APP_COUNT) return;
  focus = next;
  haptic_tick();
  refresh();
}

static void tap()
{
  if (wake()) return;
  if (banner_take()) return;
  if (!current) return open_app(focus);
  if (current->tap) current->tap();
}

static void hold()
{
  if (wake()) return;
  if (current && current->hold) current->hold();
}

static void swipe(lv_dir_t dir)
{
  if (wake()) return;
  if (dir == LV_DIR_BOTTOM) {
    if (current) {
      haptic_click();
      hub_home();
    }
    return;
  }
  if (current && current->swipe) current->swipe(dir);
}

// LVGL sends a click on release even after a swipe: a quick swipe down went home, then the click reopened the app.
static void on_click(lv_event_t *)
{
  if (!gestured) tap();
}
static void on_long_press(lv_event_t *)
{
  if (!gestured) hold();
}
static void on_gesture(lv_event_t *)
{
  gestured = true;
  swipe(lv_indev_get_gesture_dir(lv_indev_get_act()));
}
static void on_pressed(lv_event_t *)
{
  pressed = true;
  gestured = false;
  buddy_input();
}
static void on_released(lv_event_t *) { pressed = false; }

void hub_tick()
{
  static uint32_t last = 0;
  uint32_t now = millis();
  buddy_tick(now);
  if (!current && !buddy_is_shown()) globe_tick(now);
  if (now - last < 1000) return;  // subtitles and the link line: once a second is enough
  last = now;
  refresh();
}

String hub_report()
{
  if (buddy_is_shown()) return buddy_report();
  if (current == &app_timer) return "Timer " + timer_report();
  if (current == &app_clock) return "Clock " + clock_report();
  return current ? String(current->name) : "hub" + globe_report();
}

void hub_test(const String &verb, const String &arg)
{
  if (verb == "open" || verb == "home") {
    buddy_input();
    buddy_hide();
  }
  if (verb == "tap" && arg == "banner") banner_take(true);
  else if (verb == "tap") tap();
  else if (verb == "hold") {
    // "hold 2500": a finger held that long (Settings → Connection needs 2 s); plain "hold" is a short one.
    test_press_until = millis() + (arg.length() ? arg.toInt() : 500);
    hold();
  } else if (verb == "swipe") {
    swipe(arg == "left" ? LV_DIR_LEFT : arg == "right" ? LV_DIR_RIGHT : arg == "up" ? LV_DIR_TOP : LV_DIR_BOTTOM);
  } else if (verb == "saver") {  // "saver off|on|now"
    if (arg == "now") buddy_show_now();
    else buddy_enable(arg != "off");
  } else if (verb == "home") {
    hub_home();
  } else if (verb == "open") {
    for (int i = 0; i < APP_COUNT; i++) {
      if (arg == APPS[i]->name) {
        if (current) hub_home();
        open_app(i);
      }
    }
  }
}

void hub_create()
{
  lv_obj_t *screen = lv_scr_act();
  lv_obj_add_event_cb(screen, on_click, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(screen, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
  lv_obj_add_event_cb(screen, on_pressed, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(screen, on_released, LV_EVENT_RELEASED, NULL);

  page = lv_obj_create(screen);
  lv_obj_set_size(page, 360, 360);
  lv_obj_center(page);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  globe_create(page);

  for (int slot = 0; slot < SLOTS; slot++) {
    bool mid = slot == SLOTS / 2;
    // The rim is round: rows away from the centre are narrower.
    int width = mid ? 290 : abs(slot - SLOTS / 2) == 1 ? 260 : 200;
    lv_obj_t *row = rows[slot] = lv_obj_create(page);
    lv_obj_set_size(row, width, mid ? 76 : 50);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, SLOT_Y[slot]);
    lv_obj_set_style_bg_opa(row, mid ? LV_OPA_80 : LV_OPA_TRANSP, 0);  // the globe shows through, faintly
    lv_obj_set_style_bg_color(row, lv_color_hex(0x141518), 0);
    lv_obj_set_style_radius(row, 38, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_opa(row, OPA[slot], 0);
    lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *badge = badges[slot] = lv_obj_create(row);
    lv_obj_set_size(badge, BADGE[slot], BADGE[slot]);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, mid ? 8 : 14, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *icon = icons[slot] = lv_label_create(badge);
    lv_obj_set_style_text_font(icon, mid ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(C_BG), 0);
    lv_obj_center(icon);

    int text_x = BADGE[slot] + (mid ? 22 : 26);
    lv_obj_t *name = names[slot] = lv_label_create(row);
    lv_obj_set_style_text_font(name, mid ? &deck_bold_30 : &deck_16, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(C_INK), 0);
    lv_obj_set_width(name, width - text_x - 18);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, text_x, mid ? -10 : 0);

    lv_obj_t *sub = subs[slot] = lv_label_create(row);
    lv_obj_set_style_text_font(sub, &deck_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_MUTED), 0);
    lv_obj_set_width(sub, width - text_x - 18);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_label_set_text(sub, "");
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, text_x, 20);
    if (!mid) lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);
  }
  link_label = make_label(page, &deck_14, C_MUTED, 160, 120);
  hub_home();
}

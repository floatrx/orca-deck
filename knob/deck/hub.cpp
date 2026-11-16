// The hub: the knob's home screen, a watchOS-style list of apps, and the input router. The knob boots into it;
// the crown moves the list (the centre row is big, the rows toward the rim shrink and fade), a tap opens the app.
// Swipe down in any app comes back here, to the same row. .work/plans/knob-hub.md
// Behind the list the Earth turns (globe.cpp).
#include "deck.h"
#include <math.h>
#include <time.h>

// The developer feed apps (app_feed.cpp) live in one folder row, «Розробка» (user, 2026-11-16: the hub had 31 rows).
static void dev_show() {}
static void dev_hide() {}
static String dev_subtitle() { return L("15 apps · Queue, Git, Diff…", "15 апок · черга, гілки, дифф…"); }
const App app_dev = {"Dev", LV_SYMBOL_DIRECTORY, 0x7AA2F7, dev_show, dev_hide, dev_subtitle, nullptr, nullptr, nullptr, nullptr};

static const App *const MAIN_APPS[] = {&app_deck, &app_pet, &app_usage, &app_prompts,  // Claude first, Clawd second (user, 2026-11-16)
                                       &app_prs, &app_dev, &app_day, &app_focus, &app_mic, &app_timer,
                                       &app_mac, &app_jog, &app_wheel, &app_snake, &app_clock, &app_wifi, &app_settings};
static const App *const DEV_APPS[] = {&app_queue, &app_turns, &app_git, &app_context, &app_launch, &app_digest, &app_wrap,
                                      &app_hands, &app_logs, &app_diff, &app_deploy, &app_ask, &app_shutter, &app_toggl,
                                      &app_macros};
static const int MAIN_COUNT = sizeof(MAIN_APPS) / sizeof(MAIN_APPS[0]);
static const int DEV_COUNT = sizeof(DEV_APPS) / sizeof(DEV_APPS[0]);
static bool in_dev = false;  // the hub shows the folder's list
static int main_focus = 0;   // the hub's row while the folder is open: swipe down comes back to it
#define APPS (in_dev ? DEV_APPS : MAIN_APPS)
#define APP_COUNT (in_dev ? DEV_COUNT : MAIN_COUNT)
static int focus = 0;  // the centre row's app

static const int SLOTS = 5;  // rows on screen: the centre one and two on each side
static lv_obj_t *page, *link_label, *folder_label;
static lv_obj_t *rows[SLOTS], *badges[SLOTS], *icons[SLOTS], *names[SLOTS], *subs[SLOTS];
// The Clock's badge is a live analog face (LVGL's symbols have no clock): an hour and a minute hand per slot.
static lv_obj_t *hands[SLOTS][2];
// The Mic's badge is a drawn microphone (no font has one), slashed while the Mac's mic is muted.
static lv_obj_t *mic_badge[SLOTS];
// The Claude row's badge is Clawd himself on black, acting out the loudest agent state, as on the screensaver
// (user, 2026-11-14). One small canvas per slot; drawn ~12 fps while the hub is on screen.
static lv_obj_t *clawd_canvas[SLOTS];
static lv_color_t *clawd_buf[SLOTS];

static void clawd_badge(int slot, bool on)
{
  if (!clawd_canvas[slot]) return;
  on ? lv_obj_clear_flag(clawd_canvas[slot], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(clawd_canvas[slot], LV_OBJ_FLAG_HIDDEN);
}

static void draw_clawds(uint32_t now)
{
  static uint32_t last = 0;
  if (now - last < 80) return;
  last = now;
  String mood = claude_mood();
  for (int slot = 0; slot < SLOTS; slot++) {
    if (!clawd_canvas[slot] || lv_obj_has_flag(clawd_canvas[slot], LV_OBJ_FLAG_HIDDEN)) continue;
    int size = lv_obj_get_width(badges[slot]);
    int pw = size >= 56 ? 3 : size >= 40 ? 2 : 1, ph = size >= 56 ? 5 : size >= 40 ? 4 : 2;
    // The Clawd row shows the pet as he is; the Claude row keeps the standard Clawd acting out the agents
    // (user, 2026-11-16: «апка claude повинна мати свою анімовану стандартну іконку не залежно від стану улюбленця»).
    int i = focus + slot - SLOTS / 2;
    bool is_pet = i >= 0 && i < APP_COUNT && APPS[i] == &app_pet;
    PetLook look = pet_look();
    clawd_draw(clawd_buf[slot], size, size, pw, ph, (size - 5 * ph) / 2 + ph / 2, mood, now, false, lv_color_hex(0x000000),
               false, is_pet && pet_born() ? &look : nullptr);
    lv_obj_invalidate(clawd_canvas[slot]);
  }
}
static lv_point_t hand_pts[SLOTS][2][2];

static void clock_face(int slot, bool on)
{
  for (int h = 0; h < 2; h++) on ? lv_obj_clear_flag(hands[slot][h], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(hands[slot][h], LV_OBJ_FLAG_HIDDEN);
  if (!on) return;
  time_t now = time(nullptr) + tz_offset_min() * 60;
  struct tm t;
  gmtime_r(&now, &t);
  int size = lv_obj_get_width(badges[slot]), c = size / 2;
  float minute = (t.tm_min + t.tm_sec / 60.0f) / 60 * 2 * M_PI, hour = (t.tm_hour % 12 + t.tm_min / 60.0f) / 12 * 2 * M_PI;
  float len[2] = {size * 0.24f, size * 0.36f};  // hour, minute
  float ang[2] = {hour, minute};
  for (int h = 0; h < 2; h++) {
    hand_pts[slot][h][0] = {(lv_coord_t)c, (lv_coord_t)c};
    hand_pts[slot][h][1] = {(lv_coord_t)(c + len[h] * sinf(ang[h])), (lv_coord_t)(c - len[h] * cosf(ang[h]))};
    lv_line_set_points(hands[slot][h], hand_pts[slot][h], 2);
  }
}
static const App *current = nullptr;  // nullptr: the hub is on screen

// The folder's name over its list, so it never reads as the hub itself (wayfinding: where am I).
static void folder_title()
{
  if (!folder_label) return;
  set_text(folder_label, settings.lang ? "DEV" : "РОЗРОБКА");
  in_dev ? lv_obj_clear_flag(folder_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(folder_label, LV_OBJ_FLAG_HIDDEN);
}
static bool pressed = false;
// Boot (user, 2026-11-14): the Earth turns with a spinner in the middle until the Mac answers, then the list
// fades in. 5 s at most: the knob works offline too.
static lv_obj_t *spinner, *boot_label, *boot_name, *boot_clawd;
static lv_color_t *boot_buf;
#define BOOT_CLAWD 64  // Clawd inside the boot spinner (user, 2026-11-15: the device is Clawd)
static bool booting = true;
static uint32_t boot_until = 0, linked_at = 0;
static bool gestured = false;  // this touch was a swipe: its release must not count as a tap
static uint32_t test_press_until = 0;

// Per slot, from the centre outward: row height offset, badge size, opacity.
static const int SLOT_Y[SLOTS] = {-126, -70, 0, 70, 126};
static const int BADGE[SLOTS] = {30, 40, 60, 40, 30};
static const lv_opa_t OPA[SLOTS] = {LV_OPA_30, LV_OPA_60, LV_OPA_COVER, LV_OPA_60, LV_OPA_30};

// The hub shows the apps' names in Ukrainian (user, 2026-11-16: «це ж моя OS»); `App::name` stays English — the
// test verbs ("open Clock") and the code know the apps by it.
const char *hub_shown_name(const App *app)
{
  static const struct { const App *app; const char *name; } UA[] = {
      {&app_usage, "Ліміти"}, {&app_prompts, "Промпти"}, {&app_prs, "PR-радар"}, {&app_day, "День"}, {&app_focus, "Фокус"},
      {&app_mic, "Мікрофон"}, {&app_timer, "Таймер"}, {&app_mac, "Мак"}, {&app_jog, "Джог"}, {&app_wheel, "Колесо"},
      {&app_snake, "Змійка"}, {&app_clock, "Годинник"}, {&app_settings, "Налаштування"}};
  if (app == &app_dev) return settings.lang ? "Dev" : "Розробка";
  if (const char *feed = feed_name(app)) return feed;
  if (settings.lang) return app->name;  // English: the apps' own names
  for (const auto &e : UA)
    if (e.app == app) return e.name;
  return app->name;  // Clawd, Claude, Wi-Fi
}

static void refresh()
{
  if (!page || booting || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  // shared_mux comes with link_init(), after the UI is built: until then the rows go without subtitles (LVGL's
  // default "Text" showed on every label at boot when the whole refresh waited).
  bool linked = shared_mux != nullptr;
  for (int slot = 0; slot < SLOTS; slot++) {
    int i = focus + slot - SLOTS / 2;
    if (i < 0 || i >= APP_COUNT) {
      lv_obj_add_flag(rows[slot], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const App *app = APPS[i];
    lv_obj_clear_flag(rows[slot], LV_OBJ_FLAG_HIDDEN);
    bool claude = app == &app_deck || app == &app_pet;  // both badges are Clawd himself (the pet's look)
    lv_obj_set_style_bg_color(badges[slot], lv_color_hex(claude ? 0x000000 : app->color), 0);
    clawd_badge(slot, claude);
    bool mic = app == &app_mic;
    set_text(icons[slot], app == &app_clock || claude || mic ? "" : app->icon);
    clock_face(slot, app == &app_clock);
    if (mic && linked) {
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      bool muted = shared.mac.mic_muted;
      xSemaphoreGive(shared_mux);
      mic_glyph_set(mic_badge[slot], C_BG, muted);
    }
    mic ? lv_obj_clear_flag(mic_badge[slot], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(mic_badge[slot], LV_OBJ_FLAG_HIDDEN);
    set_text(names[slot], hub_shown_name(app));
    set_text(subs[slot], linked && slot == SLOTS / 2 && app->subtitle ? app->subtitle() : "");
  }
  if (!linked) return;
  String problem = status_problem();
  set_text(link_label, problem.length() ? "• " + problem : "");
  lv_obj_set_style_text_color(link_label, lv_color_hex(C_ATTENTION), 0);
}

static void open_app(int i);

static bool in_list(const App *const *list, int count, const App *app)
{
  for (int i = 0; i < count; i++)
    if (list[i] == app) return true;
  return false;
}

// Points the hub at the list that holds `app` (a banner may open an app in the folder from anywhere).
static void list_for(const App *app)
{
  bool dev = in_list(DEV_APPS, DEV_COUNT, app);
  if (dev && !in_dev) {
    for (int i = 0; i < MAIN_COUNT; i++)
      if (MAIN_APPS[i] == &app_dev) main_focus = i;
  }
  if (dev != in_dev) {
    in_dev = dev;
    focus = 0;
    folder_title();
  }
}

void hub_open(const App *app)
{
  list_for(app);
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
  if (APPS[i] == &app_dev) {  // the folder: its list replaces the hub's, the crown starts at its top
    main_focus = i;
    in_dev = true;
    focus = 0;
    haptic_click();
    folder_title();
    refresh();
    return;
  }
  focus = i;
  current = APPS[i];
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  haptic_click();
  current->show();
}

void hub_home()
{
  cc_close(true);  // the sheet never stays under the hub, catching its input
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
static void end_boot();

static bool wake()
{
  if (booting) {  // a touch during boot skips it
    end_boot();
    return true;
  }
  buddy_input();
  if (!buddy_is_shown()) return false;
  buddy_hide();
  haptic_home();
  hub_home();
  return true;
}

void hub_turn(int step)
{
  if (wake()) return;
  if (cc_is_open()) return cc_turn(step);
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
  if (cc_is_open()) return cc_tap();
  if (banner_take()) return;
  if (!current) return open_app(focus);
  if (current->tap) current->tap();
}

static void hold()
{
  if (wake()) return;
  if (cc_is_open()) return cc_hold();
  if (current && current->hold) current->hold();
}

static void swipe(lv_dir_t dir)
{
  if (wake()) return;
  if (cc_is_open()) {  // the sheet goes back down; nothing else reaches the app under it
    if (dir == LV_DIR_BOTTOM) cc_close();
    return;
  }
  if (dir == LV_DIR_TOP && current != &app_deck) {  // Claude keeps its own swipe up: the next agent
    cc_open();
    return;
  }
  if (dir == LV_DIR_BOTTOM) {
    if (current) {
      haptic_home();
      hub_home();  // an app in the folder comes back to the folder
    } else if (in_dev) {
      in_dev = false;
      focus = main_focus;
      haptic_home();
      folder_title();
      refresh();
    }
    return;
  }
  if (current && current->swipe) current->swipe(dir);
}

// LVGL sends a click on release even after a swipe: a quick swipe down went home, then the click reopened the app.
// Every gesture answers with a buzz (user, 2026-11-14: «тапи, свайпи … не мають віддачі»). An app that gave none
// — a gesture with nothing to do, or one whose action comes later — gets a light tick here.
static void answered(void (*gesture)())
{
  uint32_t before = haptic_plays();
  gesture();
  if (haptic_plays() == before) haptic_tick();
}

static lv_dir_t swipe_dir;
static void swipe_now() { swipe(swipe_dir); }

static void on_click(lv_event_t *)
{
  if (!gestured) answered(tap);
}
static void on_long_press(lv_event_t *)
{
  if (!gestured) answered(hold);
}
static void on_gesture(lv_event_t *)
{
  gestured = true;
  swipe_dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  answered(swipe_now);
}
static void on_pressed(lv_event_t *)
{
  pressed = true;
  gestured = false;
  buddy_input();
}
static void on_released(lv_event_t *) { pressed = false; }

static void fade_row(void *row, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)row, v, 0); }

static void end_boot()
{
  booting = false;
  lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(boot_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(boot_name, LV_OBJ_FLAG_HIDDEN);
  if (boot_clawd) lv_obj_add_flag(boot_clawd, LV_OBJ_FLAG_HIDDEN);
  refresh();
  for (int slot = 0; slot < SLOTS; slot++) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rows[slot]);
    lv_anim_set_values(&a, LV_OPA_TRANSP, OPA[slot]);
    lv_anim_set_time(&a, 300);
    lv_anim_set_delay(&a, abs(slot - SLOTS / 2) * 60);  // the centre row first, then outward
    lv_anim_set_exec_cb(&a, fade_row);
    lv_anim_start(&a);
  }
  buddy_input();  // the screensaver's 12 s start now, not at power-on
}

static void boot_tick(uint32_t now)
{
  static uint32_t drawn = 0;
  if (boot_clawd && now - drawn >= 80) {  // the same Clawd as the saver's, small, waking up with the knob
    drawn = now;
    clawd_draw(boot_buf, BOOT_CLAWD, BOOT_CLAWD, 3, 5, (BOOT_CLAWD - 5 * 5) / 2 + 2, linked_at ? "done" : "idle", now, false,
               lv_color_hex(0x000000));
    lv_obj_invalidate(boot_clawd);
  }
  bool online = false;
  if (shared_mux) {
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    online = shared.online;
    xSemaphoreGive(shared_mux);
  }
  if (online && !linked_at) {
    linked_at = now;
    set_text(boot_label, L("connected", "на зв’язку"));
  }
  if ((linked_at && now - linked_at >= 600) || now >= boot_until) end_boot();
}

bool hub_booting() { return booting; }

void hub_tick()
{
  static uint32_t last = 0;
  uint32_t now = millis();
  if (booting) {
    globe_tick(now);
    boot_tick(now);
    return;
  }
  buddy_tick(now);
  if (!current && !buddy_is_shown()) {
    globe_tick(now);
    draw_clawds(now);
  }
  if (now - last < 1000) return;  // subtitles and the link line: once a second is enough
  last = now;
  refresh();
}

String hub_report()
{
  if (buddy_is_shown()) return buddy_report();
  if (cc_is_open()) return cc_report();
  if (current == &app_timer) return "Timer " + timer_report();
  if (current == &app_settings) return "Settings " + settings_report();
  if (current == &app_clock) return "Clock " + clock_report();
  if (current == &app_mac) return "Mac " + mac_report();
  if (current == &app_jog) return "Jog " + jog_report();
  if (current == &app_wheel) return "Wheel " + wheel_report();
  if (current == &app_snake) return "Snake " + snake_report();
  if (current == &app_pet) return "Clawd " + pet_app_report();
  if (current == &app_focus) return "Focus " + focus_report();
  if (current == &app_usage) return "Usage " + usage_report();
  if (current == &app_day) return "Day " + day_report();
  if (current == &app_prompts) return "Prompts " + prompts_report();
  if (current == &app_mic) return "Mic " + micmute_report();
  if (current == &app_prs) return "PRs " + prs_report();
  if (current == &app_wifi) return "Wi-Fi " + wifi_app_report();
  return current ? String(current->name) : (booting ? "hub(boot)" : in_dev ? "hub/dev" : "hub") + globe_report();
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
    const App *found = nullptr;
    for (int i = 0; i < MAIN_COUNT; i++)
      if (arg == MAIN_APPS[i]->name) found = MAIN_APPS[i];
    for (int i = 0; i < DEV_COUNT; i++)
      if (arg == DEV_APPS[i]->name) found = DEV_APPS[i];
    if (found) {
      if (current) hub_home();
      list_for(found);
      for (int i = 0; i < APP_COUNT; i++)
        if (APPS[i] == found) open_app(i);
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
    lv_obj_set_style_clip_corner(badge, true, 0);  // the Claude badge's square canvas stays inside the circle
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    clawd_buf[slot] = (lv_color_t *)ps_malloc(BADGE[slot] * BADGE[slot] * sizeof(lv_color_t));
    if (clawd_buf[slot]) {
      clawd_canvas[slot] = lv_canvas_create(badge);
      lv_canvas_set_buffer(clawd_canvas[slot], clawd_buf[slot], BADGE[slot], BADGE[slot], LV_IMG_CF_TRUE_COLOR);
      lv_obj_center(clawd_canvas[slot]);
      lv_obj_clear_flag(clawd_canvas[slot], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(clawd_canvas[slot], LV_OBJ_FLAG_HIDDEN);
    }
    for (int h = 0; h < 2; h++) {
      lv_obj_t *hand = hands[slot][h] = lv_line_create(badge);
      lv_obj_set_pos(hand, 0, 0);
      lv_obj_set_style_line_width(hand, h ? (mid ? 3 : 2) : (mid ? 4 : 3), 0);
      lv_obj_set_style_line_color(hand, lv_color_hex(C_BG), 0);
      lv_obj_set_style_line_rounded(hand, true, 0);
      lv_obj_add_flag(hand, LV_OBJ_FLAG_HIDDEN);
    }
    mic_badge[slot] = mic_glyph(badge, BADGE[slot] * 7 / 10);
    lv_obj_center(mic_badge[slot]);
    mic_glyph_set(mic_badge[slot], C_BG, false);
    lv_obj_add_flag(mic_badge[slot], LV_OBJ_FLAG_HIDDEN);
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
  folder_label = make_label(page, &deck_bold_16, 0x7AA2F7, -158, 140);
  lv_obj_set_style_text_letter_space(folder_label, 2, 0);
  lv_obj_add_flag(folder_label, LV_OBJ_FLAG_HIDDEN);

  spinner = lv_spinner_create(page, 1100, 70);
  lv_obj_set_size(spinner, 96, 96);
  lv_obj_center(spinner);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(spinner, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(C_ATTENTION), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(spinner, true, LV_PART_INDICATOR);
  boot_buf = (lv_color_t *)ps_malloc(BOOT_CLAWD * BOOT_CLAWD * sizeof(lv_color_t));
  if (boot_buf) {
    boot_clawd = lv_canvas_create(page);
    lv_canvas_set_buffer(boot_clawd, boot_buf, BOOT_CLAWD, BOOT_CLAWD, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(boot_clawd);
    lv_obj_clear_flag(boot_clawd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(boot_clawd, LV_RADIUS_CIRCLE, 0);  // round, inside the spinner: no black square on the Earth
    lv_obj_set_style_clip_corner(boot_clawd, true, 0);
  }
  boot_name = make_label(page, &deck_bold_30, C_INK, -84, 220);
  set_text(boot_name, "Clawd");
  boot_label = make_label(page, &deck_bold_16, C_INK, 76, 220);
  set_text(boot_label, L("connecting…", "підключаюсь…"));
  for (int slot = 0; slot < SLOTS; slot++) lv_obj_set_style_opa(rows[slot], LV_OPA_TRANSP, 0);
  boot_until = millis() + 5000;
  hub_home();
}

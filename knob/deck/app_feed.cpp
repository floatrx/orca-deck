// The developer feed apps (.work/plans/knob-dev-apps.md): fifteen apps, one template. The bridge builds each app's
// feed (bridge/feeds.lua) — a header, pages of items, what a tap and a hold do — and the relay brings it as a G line.
// This file draws any feed:
//   crown: the drum turns, one item a detent · tap: the item's action · hold: dictate (the words go to the app) or
//   the item's second action · swipe left / right: the next / previous page
// The selected item sits in the band, bold, with its badge and detail under it; the rows toward the rim fade. The
// bottom line names where a send goes ("→ orca-deck"), and for a few seconds what the last action did (the note).
#include "deck.h"
#include <math.h>

#define ROWS 7
#define DRUM_R 96
#define ROW_ANGLE 0.40f
#define DRUM_Y -36
#define GAP 74            // the room under the selected row for its badge and detail
#define ROWS_BOTTOM 90     // the lowest row centre: the target line sits at 108
#define EASE 16.0f
#define NOTE_MS 4500
#define OPEN_EVERY_MS 60000  // the bridge refreshes the open app's sources faster while it hears this
#define LANG_EVERY_MS 300000

struct FeedDef {
  const App *app;
  const char *id;
  const char *ua;  // the Ukrainian name (the hub's and the header's)
};

static const FeedDef DEFS[] = {
    {&app_queue, "queue", "Черга"},       {&app_turns, "turns", "Довгий хід"}, {&app_git, "git", "Гілки"},
    {&app_context, "context", "Контекст"}, {&app_launch, "launch", "Запуск"},  {&app_digest, "digest", "Дайджест"},
    {&app_wrap, "wrap", "Кінець дня"},    {&app_hands, "hands", "Руки геть"},  {&app_logs, "logs", "Лог"},
    {&app_diff, "diff", "Дифф"},          {&app_deploy, "deploy", "Деплой"},   {&app_ask, "ask", "Питання"},
    {&app_shutter, "shutter", "Затвор"},  {&app_toggl, "toggl", "Час"},        {&app_macros, "macros", "Макроси"},
    {&app_notif, "notif", "Сповіщення"}};  // local: banner.cpp builds it, its taps never go to the bridge
static const int DEF_COUNT = sizeof(DEFS) / sizeof(DEFS[0]);

struct FeedState {
  int page = 0, sel = 0;
  bool opened_once = false;
  uint32_t alert_seen = 0;  // the alert seq already shown (or found at boot: old news)
  bool alert_known = false;
};

static lv_obj_t *rim;
static Accel accel;
static lv_obj_t *page_obj, *band, *header_label, *page_label, *rows[ROWS], *badge_label, *detail_label, *empty_label,
    *to_label, *hint_label, *note_label;
static FeedState states[DEF_COUNT];
static int cur = -1;  // the open feed app's index in DEFS
static Feed feed;
static uint32_t seen_version = 0, seen_note = 0, note_at = 0, last_frame = 0, open_told = 0, lang_told = 0;
static float pos = 0;

const char *feed_id(const App *app)
{
  for (const FeedDef &d : DEFS)
    if (d.app == app) return d.id;
  return "";
}

const char *feed_name(const App *app)
{
  for (const FeedDef &d : DEFS)
    if (d.app == app) return settings.lang ? app->name : d.ua;
  return nullptr;
}

static bool local() { return cur >= 0 && !strcmp(DEFS[cur].id, "notif"); }

static const FeedPage *page_now()
{
  if (feed.pages.empty()) return nullptr;
  FeedState &s = states[cur];
  if (s.page >= (int)feed.pages.size()) s.page = 0;
  return &feed.pages[s.page];
}

static void command(const char *action, const String &key = "", const String &text = "")
{
  JsonDocument cmd;
  cmd["cmd"] = "app";
  cmd["app"] = DEFS[cur].id;
  cmd["action"] = action;
  if (key.length()) cmd["key"] = key;
  cmd["page"] = states[cur].page;
  if (text.length()) cmd["text"] = text;
  std::vector<DeckTarget> targets = deck_chat_targets();
  if (!targets.empty()) cmd["agent"] = targets[0].agent;  // the agent on screen in Claude
  push_command(cmd);
}

static void layout()
{
  const FeedPage *p = page_now();
  int n = p ? p->items.size() : 0;
  int base = (int)floorf(pos + 0.5f);
  for (int k = 0; k < ROWS; k++) {
    int item = base + k - ROWS / 2;
    lv_obj_t *row = rows[k];
    if (item < 0 || item >= n) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    float d = item - pos, angle = d * ROW_ANGLE, light = cosf(angle);
    if (light <= 0.05f) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    bool front = item == base;
    const FeedItem &it = p->items[item];
    lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
    set_text(row, it.title);
    lv_obj_set_style_text_font(row, front ? &deck_bold_16 : &deck_16, 0);
    // The rows away from the band wear their item's colour: the state reads at a glance down the drum.
    uint32_t color = front ? C_INK : (it.color == C_IDLE ? C_DETAIL : it.color);
    lv_obj_set_style_text_color(row, lv_color_hex(color), 0);
    lv_label_set_long_mode(row, front ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(row, front ? 250 : (int)(170 + 60 * light));
    lv_obj_set_style_opa(row, (lv_opa_t)(255 * light * light), 0);
    float below = fminf(1.0f, fmaxf(0.0f, d));  // the rows under the band make room for its detail
    int y = DRUM_Y + (int)(DRUM_R * sinf(angle)) + (int)(GAP * below);
    if (y > ROWS_BOTTOM) {  // the target and hint lines own the bottom: a row there would sit on them
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    if (y > ROWS_BOTTOM - 24) lv_obj_set_style_opa(row, (lv_opa_t)(255 * light * light * (ROWS_BOTTOM - y) / 24), 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y);
  }
  if (n && base >= 0 && base < n) {
    const FeedItem &it = p->items[base];
    lv_obj_set_style_border_color(band, lv_color_hex(it.color), 0);
    set_text(badge_label, it.badge);
    lv_obj_set_style_text_color(badge_label, lv_color_hex(it.color == C_IDLE ? C_MUTED : it.color), 0);
    set_text(detail_label, it.detail);
    // the detail sits under the badge, or in its place when there is none
    lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, DRUM_Y + (it.badge.length() ? 66 : 48));
    lv_obj_clear_flag(band, LV_OBJ_FLAG_HIDDEN);
  } else {
    set_text(badge_label, "");
    set_text(detail_label, "");
    lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
  }
  rim_set(rim, states[cur].sel, n, 4);
}

static void paint()
{
  if (cur < 0) return;
  const FeedPage *p = page_now();
  FeedState &s = states[cur];
  set_text(header_label, feed.header.length() ? feed.header : String(feed_name(DEFS[cur].app)));
  String pages = feed.pages.size() > 1 ? "   " + String(s.page + 1) + "/" + String(feed.pages.size()) : "";
  set_text(page_label, p ? p->name + pages : L("waiting for the Mac…", "чекаю на Мак…"));
  int n = p ? p->items.size() : 0;
  if (s.sel >= n) s.sel = max(0, n - 1);
  bool empty = n == 0;
  set_text(empty_label, empty ? (p ? p->empty : String(L("waiting for the Mac…", "чекаю на Мак…"))) : "");
  empty ? lv_obj_clear_flag(empty_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
  bool noting = note_at && millis() - note_at < NOTE_MS;
  set_text(to_label, p ? p->to : "");
  noting || !p || !p->to.length() ? lv_obj_add_flag(to_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(to_label, LV_OBJ_FLAG_HIDDEN);
  set_text(hint_label, p ? p->hint : "");
  layout();
}

static void show_note()
{
  set_text(note_label, feed.note);
  bool bad = feed.note.startsWith("✗");
  lv_obj_set_style_text_color(note_label, lv_color_hex(bad ? C_ATTENTION : C_DONE), 0);
  lv_obj_clear_flag(note_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(to_label, LV_OBJ_FLAG_HIDDEN);
  note_at = millis();
}

// The newest feed for the open app; a note the app has not shown yet plays for NOTE_MS.
static void take(bool first)
{
  Feed fresh;
  if (!feed_take(DEFS[cur].id, fresh)) {
    if (first) feed = Feed(), paint();
    return;
  }
  if (!first && fresh.version == seen_version) return;
  seen_version = fresh.version;
  feed = fresh;
  if (first) {
    seen_note = feed.note_seq;  // a note from before the app opened is old news
    FeedState &s = states[cur];
    const FeedPage *p = page_now();
    if (p && p->last && !s.opened_once) s.sel = max(0, (int)p->items.size() - 1);
    s.opened_once = true;
    pos = s.sel;
  } else if (feed.note_seq != seen_note) {
    seen_note = feed.note_seq;
    if (feed.note.length()) show_note();
  }
  paint();
}

static void show_index(int i)
{
  cur = i;
  lv_obj_clear_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page_obj);
  lv_obj_add_flag(note_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(header_label, lv_color_hex(DEFS[i].app->color), 0);
  note_at = 0;
  take(true);
  if (!local()) command("open");
  open_told = millis();
}

static void hide()
{
  if (dictate_is_open()) dictate_close();
  lv_obj_add_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
  cur = -1;
}

static void turn(int step)
{
  if (cur < 0 || dictate_is_open()) return;
  const FeedPage *p = page_now();
  if (!p || p->items.empty()) return;
  FeedState &s = states[cur];
  int next = max(0, min((int)p->items.size() - 1, s.sel + accel_step(accel, step)));
  if (next == s.sel) return;  // the drum's end: no tick
  s.sel = next;
  last_frame = millis();
  haptic_tick();
}

static const FeedItem *selected()
{
  const FeedPage *p = page_now();
  int sel = states[cur].sel;
  return p && sel < (int)p->items.size() ? &p->items[sel] : nullptr;
}

static void tap()
{
  if (cur < 0) return;
  if (dictate_is_open()) return dictate_tap();
  const FeedPage *p = page_now();
  const FeedItem *it = selected();
  if (!p || !p->tap || !it) return;  // the router ticks
  if (local()) {
    haptic_click();
    return banner_history_open(it->key);
  }
  command("tap", it->key);
  if (p->to.length()) pet_event(PET_SENT);
  haptic_click();
}

static void hold()
{
  if (cur < 0 || dictate_is_open()) return;
  const FeedPage *p = page_now();
  if (!p) return;
  if (p->hold == 1) {  // dictate: the words go to the app's bridge side as action "dictate"
    JsonDocument cmd;
    cmd["cmd"] = "app";
    cmd["app"] = DEFS[cur].id;
    cmd["action"] = "dictate";
    const FeedItem *it = selected();
    if (it) cmd["key"] = it->key;
    cmd["page"] = states[cur].page;
    std::vector<DeckTarget> targets = deck_chat_targets();
    if (!targets.empty()) cmd["agent"] = targets[0].agent;
    String json;
    serializeJson(cmd, json);
    dictate_begin_cmd(feed_name(DEFS[cur].app), json);
  } else if (p->hold == 2) {
    const FeedItem *it = selected();
    if (!it) return;
    if (local()) {
      banner_history_clear();
      haptic_click(2);
      return;
    }
    command("hold", it->key);
    haptic_click(2);
  }
}

static void swipe(lv_dir_t dir)
{
  if (cur < 0) return;
  if (dictate_is_open()) return dictate_swipe();
  if (feed.pages.size() < 2 || (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)) return;
  FeedState &s = states[cur];
  s.page = (s.page + (dir == LV_DIR_LEFT ? 1 : feed.pages.size() - 1)) % feed.pages.size();
  const FeedPage *p = page_now();
  s.sel = p && p->last ? max(0, (int)p->items.size() - 1) : 0;
  pos = s.sel;
  haptic_click();
  paint();
}

void feed_tick()
{
  uint32_t now = millis();
  // The bridge writes its texts in the knob's language: tell it when the link is up, and now and then after.
  if (shared_mux && (!lang_told || now - lang_told > LANG_EVERY_MS)) {
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    bool online = shared.online;
    xSemaphoreGive(shared_mux);
    if (online) {
      lang_told = now;
      JsonDocument cmd;
      cmd["cmd"] = "app";
      cmd["app"] = "*";
      cmd["action"] = "lang";
      cmd["text"] = settings.lang ? "en" : "ua";
      push_command(cmd);
    }
  }
  static uint32_t alerts_at = 0;
  if (shared_mux && now - alerts_at > 1000) {  // a feed's alert: a banner and a buzz, also while the app is closed
    alerts_at = now;
    for (int i = 0; i < DEF_COUNT; i++) {
      Feed f;
      if (!feed_take(DEFS[i].id, f)) continue;  // a feed with no alert yet still counts as seen: its first one shows
      FeedState &s = states[i];
      bool quiet = focus_is_on() && DEFS[i].app != &app_hands && DEFS[i].app != &app_deploy;  // Focus: only the loud ones
      if (s.alert_known && f.alert_seq != s.alert_seen && f.alert.length() && !quiet) {
        banner_show_app(DEFS[i].app, f.alert);
        haptic_buzz(2);
      }
      s.alert_seen = f.alert_seq;
      s.alert_known = true;
    }
  }
  if (cur < 0 || lv_obj_has_flag(page_obj, LV_OBJ_FLAG_HIDDEN)) return;
  take(false);
  if (now - open_told > OPEN_EVERY_MS && !local()) open_told = now, command("open");
  if (note_at && now - note_at > NOTE_MS) {
    note_at = 0;
    lv_obj_add_flag(note_label, LV_OBJ_FLAG_HIDDEN);
    paint();
  }
  FeedState &s = states[cur];
  if (fabsf(pos - s.sel) < 0.002f) return;
  float dt = min(0.1f, (now - last_frame) / 1000.0f);
  last_frame = now;
  pos += (s.sel - pos) * fminf(1.0f, dt * EASE);
  if (fabsf(pos - s.sel) < 0.002f) pos = s.sel;
  layout();
}

String feed_report()
{
  if (cur < 0) return "feed: none open";
  const FeedPage *p = page_now();
  const FeedItem *it = selected();
  return String("feed ") + DEFS[cur].id + " v" + seen_version + " pages=" + feed.pages.size() + " page=" + states[cur].page +
         " items=" + (p ? p->items.size() : 0) + " sel=" + states[cur].sel + " item=" + (it ? it->title + " [" + it->key + "]" : "-") +
         " note=\"" + feed.note + "\"";
}

void feed_create()
{
  page_obj = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page_obj, 360, 360);
  lv_obj_center(page_obj);
  lv_obj_set_style_bg_color(page_obj, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page_obj, 0, 0);
  lv_obj_set_style_radius(page_obj, 0, 0);
  lv_obj_set_style_pad_all(page_obj, 0, 0);
  lv_obj_clear_flag(page_obj, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page_obj, LV_OBJ_FLAG_HIDDEN);

  band = lv_obj_create(page_obj);  // the selected row's window; its border wears the item's colour
  lv_obj_set_size(band, 290, 60);
  lv_obj_align(band, LV_ALIGN_CENTER, 0, DRUM_Y);
  lv_obj_set_style_radius(band, 30, 0);
  lv_obj_set_style_bg_color(band, lv_color_hex(0x15161A), 0);
  lv_obj_set_style_border_width(band, 2, 0);
  lv_obj_clear_flag(band, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  for (int k = 0; k < ROWS; k++) rows[k] = make_label(page_obj, &deck_16, C_DETAIL, DRUM_Y, 240);
  header_label = make_label(page_obj, &deck_bold_16, C_INK, -146, 200);
  lv_obj_set_style_text_letter_space(header_label, 1, 0);
  page_label = make_label(page_obj, &deck_14, C_MUTED, -124, 230);
  badge_label = make_label(page_obj, &deck_bold_16, C_MUTED, DRUM_Y + 44, 200);  // just under the band
  detail_label = make_label(page_obj, &deck_14, C_DETAIL, DRUM_Y + 56, 250);
  lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_max_height(detail_label, 36, 0);  // two lines: the drum's next row sits under it
  empty_label = make_label(page_obj, &deck_16, C_MUTED, -6, 250);
  lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
  to_label = make_label(page_obj, &deck_bold_16, C_MUTED, 108, 230);
  note_label = make_label(page_obj, &deck_bold_16, C_DONE, 108, 240);
  lv_label_set_long_mode(note_label, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(note_label, LV_OBJ_FLAG_HIDDEN);
  rim = rim_create(page_obj);
  hint_label = make_label(page_obj, &deck_14, C_MUTED, 132, 200);  // ~200 px of screen at this height
  lv_label_set_long_mode(hint_label, LV_LABEL_LONG_SCROLL_CIRCULAR);  // a long hint runs by instead of being cut
}

// One App per feed: the hub's struct carries plain function pointers, so each app gets thin wrappers.
static String sub_of(int i)
{
  Feed f;
  return feed_take(DEFS[i].id, f) ? f.sub : String(L("waiting for the Mac", "чекаю на Мак"));
}

#define FEED_APP(var, index, name, icon, color)                         \
  static void var##_show() { show_index(index); }                        \
  static String var##_sub() { return sub_of(index); }                    \
  const App var = {name, icon, color, var##_show, hide, var##_sub, turn, tap, hold, swipe};

FEED_APP(app_queue, 0, "Queue", LV_SYMBOL_LIST, 0x3DDC84)
FEED_APP(app_turns, 1, "Turns", LV_SYMBOL_REFRESH, 0x4CC3FF)
FEED_APP(app_git, 2, "Git Pulse", LV_SYMBOL_SHUFFLE, 0xF05033)
FEED_APP(app_context, 3, "Context", LV_SYMBOL_BATTERY_2, 0xFFD166)
FEED_APP(app_launch, 4, "Launcher", LV_SYMBOL_PLAY, 0xB48CFF)
FEED_APP(app_digest, 5, "Digest", LV_SYMBOL_EYE_OPEN, 0xFFB86B)
FEED_APP(app_wrap, 6, "Wrap-up", LV_SYMBOL_OK, 0x7AA2F7)
FEED_APP(app_hands, 7, "Hands Off", LV_SYMBOL_WARNING, 0xE5484D)
FEED_APP(app_logs, 8, "Log Tail", LV_SYMBOL_FILE, 0x9AA0A6)
FEED_APP(app_diff, 9, "Diff", LV_SYMBOL_EDIT, 0x5EEAD4)
FEED_APP(app_deploy, 10, "Deploy", LV_SYMBOL_UPLOAD, 0xF1EFE9)
FEED_APP(app_ask, 11, "Quick Ask", LV_SYMBOL_AUDIO, 0xFF7A9A)
FEED_APP(app_shutter, 12, "Shutter", LV_SYMBOL_IMAGE, 0x4CC3FF)
FEED_APP(app_toggl, 13, "Toggl", LV_SYMBOL_PLAY, 0xE57CD8)
FEED_APP(app_macros, 14, "Macros", LV_SYMBOL_KEYBOARD, 0xFFA94D)
FEED_APP(app_notif, 15, "Notifications", LV_SYMBOL_BELL, 0xFF7A1A)

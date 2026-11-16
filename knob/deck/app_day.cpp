// Agent Day Timeline, an app in the hub (idea 7 in .work/notes/knob-app-ideas.md): today on a 24 h dial — midnight at
// the top, one ring per worktree (the busiest outside), blue while its agent worked, orange while it waited on you;
// a white hand is now. The centre sums the day: how long agents waited on you, and how long they worked.
//   crown: a cursor round the clock — the centre tells the time and who did what then (5 s at rest: back to totals)
// Data: the bridge keeps the spans (GET /api/day), the relay sends them as a Y line once a minute.
#include "deck.h"
#include <math.h>

#define SIZE 360
#define RING0 166     // the outermost ring's radius
#define RING_STEP 13
#define RING_W 8
#define MAX_RINGS 6
#define CURSOR_REST_MS 5000

static lv_obj_t *page, *canvas, *big_label, *unit_label, *line1_label, *line2_label;
static lv_color_t *buf;
static DayData day;
static uint32_t seen_version = 0;
static int cursor = -1;  // minute of the day under the crown's cursor; -1: hidden
static uint32_t cursor_at = 0;
static bool dirty = true;

static inline void dot(int x, int y, lv_color_t c)
{
  for (int j = y - 1; j <= y + 1; j++) {
    if (j < 0 || j >= SIZE) continue;
    for (int i = x - 1; i <= x + 1; i++) {
      if (i >= 0 && i < SIZE) buf[j * SIZE + i] = c;
    }
  }
}

static void radial(int minute, int r_from, int r_to, lv_color_t c)
{
  float a = minute / 1440.0f * 2 * M_PI - M_PI / 2;
  float ca = cosf(a), sa = sinf(a);
  for (int r = r_from; r <= r_to; r += 2) dot(SIZE / 2 + (int)(r * ca), SIZE / 2 + (int)(r * sa), c);
}

static String hm(int minute)
{
  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", minute / 60 % 24, minute % 60);
  return t;
}

static String span_text(int minutes)
{
  if (minutes >= 60) return String(minutes / 60) + L("h ", " год ") + String(minutes % 60) + L("m", " хв");
  return String(minutes) + " min";
}

static void draw()
{
  dirty = false;
  memset(buf, 0, SIZE * SIZE * sizeof(lv_color_t));
  lv_color_t track = lv_color_hex(0x17181C), work = lv_color_hex(C_WORKING), wait = lv_color_hex(C_ATTENTION);
  int rings = min((int)day.rows.size(), MAX_RINGS);
  for (int i = 0; i < rings; i++) {
    int r = RING0 - i * RING_STEP;
    for (int m = 0; m < 1440; m += 3) radial(m, r - RING_W / 2 + 1, r + RING_W / 2 - 1, track);  // the empty ring
    for (const DaySpan &s : day.rows[i].spans) {
      for (int m = s.from; m <= s.to; m++) radial(m, r - RING_W / 2 + 1, r + RING_W / 2 - 1, s.waiting ? wait : work);
    }
  }
  int inner = RING0 - rings * RING_STEP;
  for (int h = 0; h < 24; h++) radial(h * 60, h % 6 ? 176 : 172, 178, lv_color_hex(h % 6 ? 0x3A3B40 : C_MUTED));  // hours
  if (day.now >= 0) radial(day.now, max(inner - 4, 60), 178, lv_color_hex(0xFFFFFF));  // now
  if (cursor >= 0) radial(cursor, max(inner - 4, 60), 178, lv_color_hex(0xFFD166));
  lv_obj_invalidate(canvas);

  if (cursor >= 0) {  // the time under the cursor, and who was busy then
    set_text(big_label, hm(cursor));
    String who[2];
    int n = 0;
    for (int i = 0; i < rings && n < 2; i++) {
      for (const DaySpan &s : day.rows[i].spans) {
        if (cursor >= s.from && cursor <= s.to) {
          who[n++] = day.rows[i].name + (s.waiting ? " · waited" : " · worked");
          break;
        }
      }
    }
    set_text(unit_label, n ? "" : L("nothing ran", "нічого не працювало"));
    set_text(line1_label, who[0]);
    set_text(line2_label, who[1]);
  } else {  // the day's totals
    set_text(big_label, span_text(day.waited));
    set_text(unit_label, L("waited on you", "чекали на тебе"));
    set_text(line1_label, day.now >= 0 ? L("worked ", "працювали ") + span_text(day.worked) : L("no data yet", "ще немає даних"));
    String names;
    for (int i = 0; i < min(rings, 3); i++) names += String(i ? " · " : "") + day.rows[i].name;
    set_text(line2_label, names);  // the rings from the outside in
  }
}

static void turn(int step)
{
  if (cursor < 0) cursor = day.now >= 0 ? day.now : 720;
  cursor = (cursor + step * 15 + 1440) % 1440;  // 15 min a detent: the whole day in 96
  cursor_at = millis();
  haptic_tick();
  dirty = true;
}

void day_tick()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool fresh = shared.day_version != seen_version;
  if (fresh) {
    seen_version = shared.day_version;
    day = shared.day;
  }
  xSemaphoreGive(shared_mux);
  if (fresh) dirty = true;
  if (cursor >= 0 && millis() - cursor_at > CURSOR_REST_MS) {
    cursor = -1;
    dirty = true;
  }
  if (dirty && page && buf && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) draw();
}

static String subtitle()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  int waited = shared.day.waited, now = shared.day.now;
  xSemaphoreGive(shared_mux);
  return now < 0 ? L("today", "сьогодні") : L("waited ", "чекали ") + span_text(waited);
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  dirty = true;
}

static void hide()
{
  cursor = -1;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

String day_report()
{
  return "now=" + String(day.now) + " waited=" + String(day.waited) + " worked=" + String(day.worked) + " rows=" +
         String((int)day.rows.size()) + " cursor=" + String(cursor) + " | " + lv_label_get_text(big_label) + " | " +
         lv_label_get_text(line1_label) + " | " + lv_label_get_text(line2_label);
}

void day_create()
{
  page = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page, SIZE, SIZE);
  lv_obj_center(page);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

  buf = (lv_color_t *)ps_malloc(SIZE * SIZE * sizeof(lv_color_t));
  if (buf) {
    memset(buf, 0, SIZE * SIZE * sizeof(lv_color_t));
    canvas = lv_canvas_create(page);
    lv_canvas_set_buffer(canvas, buf, SIZE, SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  }
  big_label = make_label(page, &deck_bold_30, C_INK, -18, 150);
  unit_label = make_label(page, &deck_14, C_ATTENTION, 10, 150);
  line1_label = make_label(page, &deck_14, C_MUTED, 30, 140);
  line2_label = make_label(page, &deck_14, C_MUTED, 48, 130);
}

const App app_day = {"Day", LV_SYMBOL_LOOP, 0x4CC3FF, show, hide, subtitle, turn, nullptr, nullptr, nullptr};

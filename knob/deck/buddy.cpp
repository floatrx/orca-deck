// Buddy: the screensaver. Clawd from the Claude Code banner (` ▐▛███▜▌ / ▝▜█████▛▘ / ▘▘ ▝▝`) as pixel art, acting
// out the loudest agent state. It starts after a few seconds without input in the hub or Settings (not over
// orca-deck: that page is the one glanced at); a tap or the crown goes back to the hub (user, 2026-11-14).
//
// One terminal cell is about twice as tall as it is wide, so a sprite pixel is 14 × 26 px: square pixels made
// Clawd look squashed (user caught it in the mock).
#include "deck.h"
#include <math.h>

#define PW 14
#define PH 26
#define COLS 18
#define ROWS 5
#define CW (COLS * PW)          // canvas: the sprite plus room above for a hop and the "!"
#define CH (ROWS * PH + 5 * PH)
#define BASE_Y (4 * PH)         // the sprite's top row inside the canvas at rest
#define FRAME_MS 80             // pixel art moves in steps: ~12 fps is enough

static const char *const SPRITE[ROWS] = {
    "...############...",
    "...##.######.##...",
    ".################.",
    "...############...",
    "....#.#....#.#....",
};

static lv_obj_t *page, *canvas, *caption, *zz;
static lv_color_t *buf;
static bool shown = false, enabled = true;
static uint32_t last_input = 0;
static String mood = "idle";

static const uint32_t ORANGE = 0xD97757, YELLOW = 0xFFD166;

static void fill(int x, int y, int w, int h, lv_color_t c)
{
  for (int j = max(0, y); j < min(CH, y + h); j++) {
    for (int i = max(0, x); i < min(CW, x + w); i++) buf[j * CW + i] = c;
  }
}

static void draw(uint32_t now)
{
  char m[ROWS][COLS + 1];
  for (int r = 0; r < ROWS; r++) memcpy(m[r], SPRITE[r], COLS + 1);
  auto set = [&](int r, int c, char v) { if (r >= 0 && r < ROWS && c >= 0 && c < COLS) m[r][c] = v; };
  float t = now / 1000.0f;
  int dy = 0;  // px; negative = up
  bool bang = false, dim = false, line_eyes = false;

  if (mood == "working") {
    bool step = (now / 160) % 2;  // legs run in place
    for (int c : {4, 6, 11, 13}) set(4, c, '.');
    for (int c : step ? std::initializer_list<int>{4, 6, 11, 13} : std::initializer_list<int>{3, 5, 12, 14}) set(4, c, '#');
    bool left = (now / 1250) % 2;  // eyes glance left and right
    set(1, 5, '#'), set(1, 12, '#');
    set(1, left ? 4 : 6, '.'), set(1, left ? 11 : 13, '.');
  } else if (mood == "attention") {
    float ph = fmodf(t * 1.4f, 1.0f);
    dy = -(int)(fmaxf(0, sinf(ph * M_PI)) * 34);
    bang = true;
  } else if (mood == "done") {
    bool up = (now / 330) % 2;  // arms up and down
    set(2, 1, '.'), set(2, 16, '.');
    set(1, 1, '#'), set(1, 16, '#');
    if (up) set(0, 1, '#'), set(0, 16, '#'), dy = -10;
  } else if (mood == "sleep") {
    dim = true;
    line_eyes = true;
    set(1, 5, '#'), set(1, 12, '#');
    dy = (int)(sinf(t) * 3);
  } else {  // idle: a slow bob and a blink every ~4 s
    dy = (int)(sinf(t * 2) * 4);
    if (fmodf(t, 4.0f) > 3.85f) set(1, 5, '#'), set(1, 12, '#');
  }

  lv_color_t bg = lv_color_hex(C_BG);
  lv_color_t body = dim ? lv_color_mix(lv_color_hex(ORANGE), bg, 110) : lv_color_hex(ORANGE);
  fill(0, 0, CW, CH, bg);
  int y0 = BASE_Y + dy;
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      if (m[r][c] == '#') fill(c * PW, y0 + r * PH, PW, PH, body);
    }
  }
  if (line_eyes) {
    lv_color_t eye = lv_color_hex(0x1A1210);
    for (int c : {4, 11}) fill(c * PW, y0 + PH + PH / 2, 2 * PW, 4, eye);
  }
  if (bang) {  // "!" above the head: a bar and a dot
    lv_color_t y = lv_color_hex(YELLOW);
    fill(8 * PW, y0 - 3 * PH + 6, 2 * PW, PH + 10, y);
    fill(8 * PW, y0 - PH - 2, 2 * PW, 14, y);
  }
  lv_obj_invalidate(canvas);

  if (mood == "sleep") {
    float ph = fmodf(t * 0.5f, 1.0f);
    lv_obj_clear_flag(zz, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(zz, LV_ALIGN_CENTER, 70 + (int)(ph * 12), -70 - (int)(ph * 30));
    lv_obj_set_style_opa(zz, (lv_opa_t)(255 * sinf(ph * M_PI)), 0);
  } else {
    lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  }
}

// The loudest state among the agents; "sleep" when no relay link is alive.
static String loudest()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool online = shared.online;
  xSemaphoreGive(shared_mux);
  if (!online) return "sleep";
  String out = "idle";
  for (const Agent &a : deck_all_agents()) {
    if (a.bucket == "attention") out = "attention";
    else if (a.bucket == "working" && out != "attention") out = "working";
    else if (a.bucket == "done" && out == "idle") out = "done";
  }
  return out;
}

static void show()
{
  shown = true;
  mood = loudest();
  set_text(caption, app_deck.subtitle());
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  banner_hide();
}

void buddy_hide()
{
  shown = false;
  if (page) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

bool buddy_is_shown() { return shown; }

void buddy_input() { last_input = millis(); }

void buddy_enable(bool on)
{
  enabled = on;
  buddy_input();
}

void buddy_show_now() { show(); }

void buddy_tick(uint32_t now)
{
  if (!page) return;
  if (!shown) {
    const App *app = hub_current();
    bool eligible = enabled && settings.saver && (app == nullptr || app == &app_settings);
    if (eligible && now - last_input >= settings.saver * 1000UL) show();
    return;
  }
  static uint32_t last = 0, last_mood = 0;
  if (now - last < FRAME_MS) return;
  last = now;
  if (now - last_mood >= 1000) {  // the agents change slowly: once a second
    last_mood = now;
    mood = loudest();
    set_text(caption, app_deck.subtitle());
  }
  draw(now);
}

String buddy_report() { return shown ? "buddy(" + mood + ")" : ""; }

void buddy_create()
{
  buf = (lv_color_t *)ps_malloc(CW * CH * sizeof(lv_color_t));
  if (!buf) return;
  page = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page, 360, 360);
  lv_obj_center(page);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

  canvas = lv_canvas_create(page);
  lv_canvas_set_buffer(canvas, buf, CW, CH, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -52);
  lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  fill(0, 0, CW, CH, lv_color_hex(C_BG));

  caption = make_label(page, &deck_14, C_MUTED, 110, 220);
  zz = lv_label_create(page);
  lv_obj_set_style_text_font(zz, &deck_bold_30, 0);
  lv_obj_set_style_text_color(zz, lv_color_hex(C_MUTED), 0);
  lv_label_set_text(zz, "z");
  lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  last_input = millis();
}

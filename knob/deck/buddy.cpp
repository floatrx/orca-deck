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
// Claude's plan limits round the rim (user, 2026-11-14: «на заставці ліміти гарно показати»): the outer arc is the
// week, the inner one the 5 h window; open at the bottom, where the numbers sit.
static lv_obj_t *week_arc, *session_arc, *limits_label;
static lv_color_t *buf;
static bool shown = false, enabled = true;
static uint32_t last_input = 0;
static String mood = "idle";

static const uint32_t ORANGE = 0xD97757, YELLOW = 0xFFD166;

static void fill(lv_color_t *b, int bw, int bh, int x, int y, int w, int h, lv_color_t c)
{
  for (int j = max(0, y); j < min(bh, y + h); j++) {
    for (int i = max(0, x); i < min(bw, x + w); i++) b[j * bw + i] = c;
  }
}

// Clawd acting out `mood`, into any RGB565 buffer: the screensaver (14 × 26 px pixels) and the hub's Claude badge
// (a few px per pixel). Sizes of the hop, the arms and the eyes scale with the pixel height. `bang`: room above the
// head for the needs-you "!". bg: the buffer's background.
void clawd_draw(lv_color_t *b, int bw, int bh, int pw, int ph, int base_y, const String &mood, uint32_t now, bool bang_ok,
                lv_color_t bg)
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
    float phase = fmodf(t * 1.4f, 1.0f);
    dy = -(int)(fmaxf(0, sinf(phase * M_PI)) * ph * 1.3f);
    bang = bang_ok;
  } else if (mood == "done") {
    bool up = (now / 330) % 2;  // arms up and down
    set(2, 1, '.'), set(2, 16, '.');
    set(1, 1, '#'), set(1, 16, '#');
    if (up) set(0, 1, '#'), set(0, 16, '#'), dy = -max(1, (int)(ph * 0.4f));
  } else if (mood == "sleep") {
    dim = true;
    line_eyes = true;
    set(1, 5, '#'), set(1, 12, '#');
    dy = (int)(sinf(t) * ph * 0.12f);
  } else {  // idle: a slow bob and a blink every ~4 s
    dy = (int)(sinf(t * 2) * ph * 0.15f);
    if (fmodf(t, 4.0f) > 3.85f) set(1, 5, '#'), set(1, 12, '#');
  }

  lv_color_t body = dim ? lv_color_mix(lv_color_hex(ORANGE), bg, 110) : lv_color_hex(ORANGE);
  fill(b, bw, bh, 0, 0, bw, bh, bg);
  int x0 = (bw - COLS * pw) / 2, y0 = base_y + dy;
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      if (m[r][c] == '#') fill(b, bw, bh, x0 + c * pw, y0 + r * ph, pw, ph, body);
    }
  }
  if (line_eyes) {
    lv_color_t eye = lv_color_hex(0x1A1210);
    for (int c : {4, 11}) fill(b, bw, bh, x0 + c * pw, y0 + ph + ph / 2, 2 * pw, max(1, ph / 6), eye);
  }
  if (bang) {  // "!" above the head: a bar and a dot
    lv_color_t y = lv_color_hex(YELLOW);
    fill(b, bw, bh, x0 + 8 * pw, y0 - 3 * ph + ph / 4, 2 * pw, ph + ph * 2 / 5, y);
    fill(b, bw, bh, x0 + 8 * pw, y0 - ph - ph / 12, 2 * pw, ph / 2, y);
  }
}

static void draw(uint32_t now)
{
  clawd_draw(buf, CW, CH, PW, PH, BASE_Y, mood, now, true, lv_color_hex(C_BG));
  lv_obj_invalidate(canvas);
  float t = now / 1000.0f;
  if (mood == "sleep") {
    float phase = fmodf(t * 0.5f, 1.0f);
    lv_obj_clear_flag(zz, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(zz, LV_ALIGN_CENTER, 70 + (int)(phase * 12), -70 - (int)(phase * 30));
    lv_obj_set_style_opa(zz, (lv_opa_t)(255 * sinf(phase * M_PI)), 0);
  } else {
    lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  }
}

static void paint_limits()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  lv_arc_set_value(week_arc, max(0, mac.claude_week));
  lv_arc_set_value(session_arc, max(0, mac.claude_session));
  mac.claude_week >= 0 ? lv_obj_clear_flag(week_arc, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(week_arc, LV_OBJ_FLAG_HIDDEN);
  mac.claude_session >= 0 ? lv_obj_clear_flag(session_arc, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(session_arc, LV_OBJ_FLAG_HIDDEN);
  // The week turns orange past 80 %: close to the wall.
  lv_obj_set_style_arc_color(week_arc, lv_color_hex(mac.claude_week >= 80 ? C_ATTENTION : C_WORKING), LV_PART_INDICATOR);
  set_text(limits_label, claude_limits(true));
}

// The loudest state among the agents; "sleep" when no relay link is alive.
String claude_mood()
{
  if (!shared_mux) return "sleep";
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
  mood = claude_mood();
  set_text(caption, app_deck.subtitle());
  paint_limits();
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
    mood = claude_mood();
    set_text(caption, app_deck.subtitle());
    paint_limits();
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
  fill(buf, CW, CH, 0, 0, CW, CH, lv_color_hex(C_BG));

  for (int k = 0; k < 2; k++) {
    lv_obj_t *arc = lv_arc_create(page);
    int size = k ? 326 : 348;
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1C1D21), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(k ? ORANGE : C_WORKING), LV_PART_INDICATOR);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    (k ? session_arc : week_arc) = arc;
  }
  caption = make_label(page, &deck_14, C_MUTED, 104, 220);
  limits_label = make_label(page, &deck_14, C_DETAIL, 128, 200);
  zz = lv_label_create(page);
  lv_obj_set_style_text_font(zz, &deck_bold_30, 0);
  lv_obj_set_style_text_color(zz, lv_color_hex(C_MUTED), 0);
  lv_label_set_text(zz, "z");
  lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  last_input = millis();
}

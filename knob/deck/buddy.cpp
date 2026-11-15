// Buddy: the screensaver. Clawd from the Claude Code banner (` ▐▛███▜▌ / ▝▜█████▛▘ / ▘▘ ▝▝`) as pixel art, acting
// out the loudest agent state. It starts after a few seconds without input in the hub or Settings (not over
// orca-deck: that page is the one glanced at); a tap or the crown goes back to the hub (user, 2026-11-14).
//
// One terminal cell is about twice as tall as it is wide, so a sprite pixel is 10 × 18 px: square pixels made
// Clawd look squashed (user caught it in the mock). Smaller since 2026-11-15 (user: «зменшити баді-клода … заставка
// трішки більш інформаційна»): under him the Mac's time, battery and CPU; his body has a light top-to-bottom
// gradient and a soft shadow on the floor that shrinks while he hops.
#include "deck.h"
#include <math.h>

#define PW 10
#define PH 18
#define COLS 18
#define ROWS 5
#define CW (COLS * PW)          // canvas: the sprite plus room above for a hop and the "!"
#define CH (9 * PH)             // the sprite, room above for a hop and the "!", and the floor shadow below
#define BASE_Y (3 * PH)         // the sprite's top row inside the canvas at rest
#define FRAME_MS 80             // pixel art moves in steps: ~12 fps is enough

static const char *const SPRITE[ROWS] = {
    "...############...",
    "...##.######.##...",
    ".################.",
    "...############...",
    "....#.#....#.#....",
};

static lv_obj_t *page, *canvas, *zz;
// The screensaver tells the state at a glance (user, 2026-11-15: «ліміти клода тижневі і денні … скільки працює
// агентів … гарненько подумай над UI»):
//   rim: the week's limit on the left half, the 5 h window on the right — both fill from the bottom up
//   under Clawd: three counts (needs you · working · done); then the Mac: time · battery · CPU
//   bottom: each limit's % and time to reset, under its own half of the rim
static lv_obj_t *week_arc, *session_arc, *count_value[3], *count_name[3], *mac_label, *power_label, *limit_value[2],
    *limit_name[2];
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
                lv_color_t bg, bool floor_shadow)
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

  // The body: a light gradient, warm peach at the top to a deeper terracotta at the feet.
  lv_color_t top = lv_color_hex(0xF0A07C), bottom = lv_color_hex(0xB8573A);
  if (dim) top = lv_color_mix(top, bg, 110), bottom = lv_color_mix(bottom, bg, 110);
  fill(b, bw, bh, 0, 0, bw, bh, bg);
  int x0 = (bw - COLS * pw) / 2, y0 = base_y + dy;
  if (floor_shadow) {  // an ellipse under the feet: smaller and fainter the higher he is
    int fy = base_y + ROWS * ph + ph / 3, lift = -dy;
    float k = fmaxf(0.35f, 1.0f - lift / (2.0f * ph));
    int rx = (int)(COLS * pw * 0.42f * k), ry = max(2, ph / 4);
    lv_color_t sh = lv_color_mix(lv_color_hex(0x3A2018), bg, (uint8_t)(150 * k));
    for (int j = -ry; j <= ry; j++) {
      int half = (int)(rx * sqrtf(1.0f - (float)(j * j) / (ry * ry)));
      fill(b, bw, bh, bw / 2 - half, fy + j, 2 * half, 1, sh);
    }
  }
  int body_h = ROWS * ph;
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      if (m[r][c] != '#') continue;
      for (int j = 0; j < ph; j++) {  // one scanline at a time: the gradient runs through the pixels
        lv_color_t col = lv_color_mix(bottom, top, (uint8_t)(255 * (r * ph + j) / body_h));
        fill(b, bw, bh, x0 + c * pw, y0 + r * ph + j, pw, 1, col);
      }
    }
  }
  if (line_eyes) {
    lv_color_t eye = lv_color_hex(0x1A1210);
    for (int c : {4, 11}) fill(b, bw, bh, x0 + c * pw, y0 + ph + ph / 2, 2 * pw, max(1, ph / 6), eye);
  }
  if (mood == "sleep" && !bang_ok) {  // the hub's badge: two small pixel z's drifting up from the head
    lv_color_t zc = lv_color_mix(lv_color_hex(C_MUTED), bg, 200);
    int u = max(1, pw / 2 + 1);
    for (int k = 0; k < 2; k++) {
      float phase = fmodf(t * 0.5f + k * 0.5f, 1.0f);
      int zx = x0 + (COLS - 3) * pw + (int)(phase * 2 * u) + k * u, zy = y0 - (int)(phase * 3 * u) - u;
      if (zy < 0 || zx + 3 * u > bw) continue;
      fill(b, bw, bh, zx, zy, 3 * u, u, zc);              // top bar
      fill(b, bw, bh, zx + u, zy + u, u, u, zc);          // the diagonal
      fill(b, bw, bh, zx, zy + 2 * u, 3 * u, u, zc);      // bottom bar
    }
  }
  if (bang) {  // "!" above the head: a bar and a dot
    lv_color_t y = lv_color_hex(YELLOW);
    fill(b, bw, bh, x0 + 8 * pw, y0 - 3 * ph + ph / 4, 2 * pw, ph + ph * 2 / 5, y);
    fill(b, bw, bh, x0 + 8 * pw, y0 - ph - ph / 12, 2 * pw, ph / 2, y);
  }
}

static void draw(uint32_t now)
{
  clawd_draw(buf, CW, CH, PW, PH, BASE_Y, mood, now, true, lv_color_hex(C_BG), true);
  lv_obj_invalidate(canvas);
  float t = now / 1000.0f;
  if (mood == "sleep") {
    float phase = fmodf(t * 0.5f, 1.0f);
    lv_obj_clear_flag(zz, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(zz, LV_ALIGN_CENTER, 56 + (int)(phase * 10), -112 - (int)(phase * 24));
    lv_obj_set_style_opa(zz, (lv_opa_t)(255 * sinf(phase * M_PI)), 0);
  } else {
    lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  }
}

static String left_text(int minutes)
{
  if (minutes < 0) return "";
  if (minutes >= 1440) return String(minutes / 1440) + "d " + String(minutes % 1440 / 60) + "h";
  if (minutes >= 60) return String(minutes / 60) + "h";  // short: two of them share the bottom
  return String(minutes) + "m";
}

static uint32_t limit_color(int pct, uint32_t calm)
{
  return pct >= 95 ? 0xFF4B4B : pct >= 80 ? C_ATTENTION : calm;
}

static void paint_info()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);

  // The limits: an arc each, and its number under it.
  int pct[2] = {mac.claude_week, mac.claude_session}, reset[2] = {mac.week_reset_min, mac.session_reset_min};
  uint32_t calm[2] = {C_WORKING, ORANGE};
  const char *name[2] = {"week", "5 h"};
  lv_obj_t *arcs[2] = {week_arc, session_arc};
  for (int k = 0; k < 2; k++) {
    bool known = pct[k] >= 0;
    uint32_t c = known ? limit_color(pct[k], calm[k]) : C_IDLE;
    lv_arc_set_value(arcs[k], max(0, pct[k]));
    lv_obj_set_style_arc_color(arcs[k], lv_color_hex(c), LV_PART_INDICATOR);
    set_text(limit_value[k], known ? String(pct[k]) + "%" : "--");
    lv_obj_set_style_text_color(limit_value[k], lv_color_hex(known ? c : C_IDLE), 0);
    String r = left_text(reset[k]);
    set_text(limit_name[k], known ? String(name[k]) + (r.length() ? " · " + r : "") : String(name[k]) + " · idle");
  }

  // The agents: three counts, a zero dims.
  int counts[3] = {0, 0, 0};
  for (const Agent &a : deck_all_agents()) {
    if (a.bucket == "attention") counts[0]++;
    else if (a.bucket == "working") counts[1]++;
    else if (a.bucket == "done") counts[2]++;
  }
  const uint32_t lit[3] = {C_ATTENTION, C_WORKING, C_DONE};
  for (int k = 0; k < 3; k++) {
    set_text(count_value[k], String(counts[k]));
    lv_obj_set_style_text_color(count_value[k], lv_color_hex(counts[k] ? lit[k] : 0x3A3B40), 0);
    lv_obj_set_style_text_color(count_name[k], lv_color_hex(counts[k] ? C_MUTED : 0x3A3B40), 0);
  }

  // The Mac: its time (as the Clock app), the battery with its level's symbol, the CPU.
  String line;
  if (time(nullptr) > 1700000000) {
    time_t t = time(nullptr) + tz_offset_min() * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    line = hm;
  }
  if (mac.cpu >= 0) line += String(line.length() ? "    " : "") + "CPU " + String(mac.cpu) + "%";
  set_text(mac_label, line);
  // The two batteries on their own line, named: the Mac's and the knob's.
  String power;
  if (mac.battery >= 0) {
    const char *level = mac.battery > 80 ? LV_SYMBOL_BATTERY_FULL : mac.battery > 55 ? LV_SYMBOL_BATTERY_3
                        : mac.battery > 30 ? LV_SYMBOL_BATTERY_2 : mac.battery > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    power = "Mac " + String(level) + " " + String(mac.battery) + "%" + (mac.charging ? " " LV_SYMBOL_CHARGE : "");
  }
  String knob = battery_text();
  if (knob.length()) power += String(power.length() ? "    " : "") + "Knob " + knob;
  set_text(power_label, power);
  bool low = (mac.battery >= 0 && mac.battery < 15 && !mac.charging) || (battery_level() >= 0 && battery_level() < 15);
  lv_obj_set_style_text_color(power_label, lv_color_hex(low ? C_ATTENTION : C_MUTED), 0);
}

#define DONE_CHEER_S 30  // a fresh done: Clawd cheers this long, then he sleeps

// The loudest state among the agents; "sleep" when no relay link is alive, and when every agent is done (user,
// 2026-11-15: «коли всі задачі DONE … персонаж який спить … Zzzz»): a done younger than DONE_CHEER_S still cheers
// first, so the moment a task finishes is seen.
String claude_mood()
{
  if (!shared_mux) return "sleep";
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool online = shared.online;
  xSemaphoreGive(shared_mux);
  if (!online) return "sleep";
  time_t now = time(nullptr);
  bool any = false, attention = false, working = false, fresh_done = false;
  for (const Agent &a : deck_all_agents()) {
    any = true;
    if (a.bucket == "attention") attention = true;
    else if (a.bucket == "working") working = true;
    else if (a.bucket == "done" && a.since && now > 1700000000 && now - a.since / 1000 < DONE_CHEER_S) fresh_done = true;
  }
  if (attention) return "attention";
  if (working) return "working";
  if (fresh_done) return "done";
  return any ? "sleep" : "idle";
}

static void show()
{
  shown = true;
  mood = claude_mood();
  paint_info();
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
    paint_info();
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
  lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -88);
  lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  fill(buf, CW, CH, 0, 0, CW, CH, lv_color_hex(C_BG));

  // The limits' arcs: the week on the left half, the 5 h window on the right; each fills from the bottom up.
  for (int k = 0; k < 2; k++) {
    lv_obj_t *arc = lv_arc_create(page);
    lv_obj_set_size(arc, 348, 348);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, k ? 295 : 115, k ? 65 : 245);  // a 50° gap at the top and at the bottom
    lv_arc_set_range(arc, 0, 100);
    if (k) lv_arc_set_mode(arc, LV_ARC_MODE_REVERSE);  // the right half runs top → bottom: fill from its end
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x17181C), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    (k ? session_arc : week_arc) = arc;
    limit_value[k] = make_label(page, &deck_bold_16, C_INK, 106, 100);
    lv_obj_align(limit_value[k], LV_ALIGN_CENTER, k ? 60 : -60, 106);
    limit_name[k] = make_label(page, &deck_14, C_MUTED, 125, 100);
    lv_obj_align(limit_name[k], LV_ALIGN_CENTER, k ? 60 : -60, 125);
  }
  // The agents' counts, in their states' colours.
  const char *names[3] = {"needs you", "working", "done"};
  for (int k = 0; k < 3; k++) {
    count_value[k] = make_label(page, &deck_bold_30, C_INK, 14, 80);
    lv_obj_align(count_value[k], LV_ALIGN_CENTER, (k - 1) * 80, 14);
    count_name[k] = make_label(page, &deck_14, C_MUTED, 40, 84);
    lv_obj_align(count_name[k], LV_ALIGN_CENTER, (k - 1) * 80, 40);
    set_text(count_name[k], names[k]);
  }
  mac_label = make_label(page, &lv_font_montserrat_14, C_MUTED, 62, 250);    // the Mac's time and CPU
  power_label = make_label(page, &lv_font_montserrat_14, C_MUTED, 81, 260);  // ASCII + the battery / bolt symbols
  zz = lv_label_create(page);
  lv_obj_set_style_text_font(zz, &deck_bold_30, 0);
  lv_obj_set_style_text_color(zz, lv_color_hex(C_MUTED), 0);
  lv_label_set_text(zz, "z");
  lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  last_input = millis();
}

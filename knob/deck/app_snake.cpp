// Snake, an app in the hub (user, 2026-11-15: «додати гру Snake»; ideas in .work/backlog.md). The board is the round
// screen itself: 16 px cells whose centres lie inside the disc; the disc's edge wraps round to the far side. The crown is the
// joystick — a detent turns the head left or right (two quick detents land on two steps); a tap starts, pauses and
// restarts. An apple is a click, a crash a buzz; the snake speeds up as it grows. The best score lives in NVS.
// Levels (user, 2026-11-15: «додай у Snake стіни-кільця і рівні»): every 6 apples the next level draws concentric ring
// walls with gaps (none on level 1, three on levels 4–5, each level's gaps turned differently); the snake restarts short
// on a free run of cells, the score carries on, the pace is a notch faster.
//   crown: steer · tap: start / pause / again · swipe down: the hub (pauses)
#include "deck.h"
#include <Preferences.h>

#define CELL 16
#define GRID 22                 // 22 × 16 = 352 px: the board's square, centred on the 360 px screen
#define BOARD_R 158.0f          // a cell is on the board when its centre is this close to the screen's centre
#define MAX_LEN (GRID * GRID)
#define STEP_START_MS 170
#define STEP_MIN_MS 75
#define STEP_FASTER_MS 4        // per apple
#define STEP_LEVEL_MS 12        // each level starts this much faster
#define APPLES_PER_LEVEL 6
#define LEVELS 5
#define LEVEL_CARD_MS 1600      // "LEVEL N" over the new walls, then it runs
#define GAP_PX 36.0f            // a gap's width along its ring: two cells

struct Ring { float r; int gaps; float offset; };  // radius (px), gaps evenly spread, the first gap's angle (deg)
static const Ring RINGS[LEVELS][3] = {
    {},
    {{72, 4, 45}},
    {{56, 3, 0}, {120, 4, 45}},
    {{48, 2, 90}, {96, 3, 30}, {140, 4, 0}},
    {{48, 3, 15}, {96, 4, 60}, {140, 5, 0}},
};
static const int RING_COUNT[LEVELS] = {0, 1, 2, 3, 3};

enum State { READY, RUNNING, PAUSED, OVER, LEVELUP };

static lv_obj_t *page, *board, *title_label, *score_label, *hint_label;
static lv_color_t *buf;
static State state = READY;
static uint8_t body_x[MAX_LEN], body_y[MAX_LEN];  // a ring buffer: head at `head`, `len` cells back from it
static int head = 0, len = 0, dx = 1, dy = 0, score = 0, best = 0;
static int apple_x = 0, apple_y = 0;
static int8_t turns[2];  // queued crown turns, applied one per step
static int queued = 0;
static uint32_t step_ms = STEP_START_MS, last_step = 0, last_draw = 0, ate_at = 0;
static bool best_dirty = false;
static int level = 1, level_apples = 0;
static uint32_t level_until = 0;
static bool wall[GRID][GRID];

static int px(int cell) { return 180 + (cell - GRID / 2) * CELL; }  // a cell's left / top edge on screen

static bool on_board(int x, int y)
{
  if (x < 0 || y < 0 || x >= GRID || y >= GRID) return false;
  float cx = px(x) + CELL / 2.0f - 180, cy = px(y) + CELL / 2.0f - 180;
  return cx * cx + cy * cy <= BOARD_R * BOARD_R;
}

static void build_walls()
{
  memset(wall, 0, sizeof(wall));
  for (int k = 0; k < RING_COUNT[level - 1]; k++) {
    const Ring &ring = RINGS[level - 1][k];
    float half_gap = GAP_PX / ring.r / 2 * 180 / M_PI;  // degrees
    for (int y = 0; y < GRID; y++)
      for (int x = 0; x < GRID; x++) {
        float cx = px(x) + CELL / 2.0f - 180, cy = px(y) + CELL / 2.0f - 180;
        // CELL/√2: a supercover band, so the ring's cells touch side to side (a corner-only touch is a hole the
        // snake slips through)
        if (fabsf(sqrtf(cx * cx + cy * cy) - ring.r) >= CELL * 0.71f) continue;
        float a = atan2f(cy, cx) * 180 / M_PI;
        bool in_gap = false;
        for (int g = 0; g < ring.gaps; g++) {
          float d = fabsf(fmodf(a - ring.offset - g * 360.0f / ring.gaps + 540, 360) - 180);
          if (d < half_gap) in_gap = true;
        }
        if (!in_gap) wall[y][x] = true;
      }
  }
}

static bool blocked(int x, int y) { return !on_board(x, y) || wall[y][x]; }

static int at(int back) { return (head - back + MAX_LEN) % MAX_LEN; }

static bool on_snake(int x, int y, int skip_tail)
{
  for (int i = 0; i < len - skip_tail; i++) {
    if (body_x[at(i)] == x && body_y[at(i)] == y) return true;
  }
  return false;
}

static void place_apple()
{
  for (int tries = 0; tries < 2000; tries++) {
    int x = random(0, GRID), y = random(0, GRID);
    if (!blocked(x, y) && !on_snake(x, y, 0)) {
      apple_x = x, apple_y = y;
      return;
    }
  }
}

static void fill(int x0, int y0, int w, int h, lv_color_t c)
{
  for (int y = max(0, y0); y < min(360, y0 + h); y++) {
    lv_color_t *row = buf + y * 360;
    for (int x = max(0, x0); x < min(360, x0 + w); x++) row[x] = c;
  }
}

// A rounded cell: a 14 px square with its corners cut.
static void cell(int x, int y, lv_color_t c, int inset = 1)
{
  int l = px(x) + inset, t = px(y) + inset, s = CELL - 2 * inset;
  fill(l + 1, t, s - 2, s, c);
  fill(l, t + 1, s, s - 2, c);
}

static void draw()
{
  if (!buf) return;
  memset(buf, 0, 360 * 360 * sizeof(lv_color_t));  // C_BG is black
  // the board: a faint dot in every free cell, so the round edge reads
  lv_color_t dot = lv_color_hex(0x1A1B1F);
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      if (!blocked(x, y)) fill(px(x) + 7, px(y) + 7, 2, 2, dot);
  // the ring walls: slate blocks, a lighter top edge
  lv_color_t stone = lv_color_hex(0x3B4252), edge = lv_color_hex(0x5E6779);
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      if (wall[y][x]) {
        fill(px(x), px(y), CELL, CELL, stone);
        if (y == 0 || !wall[y - 1][x]) fill(px(x), px(y), CELL, 2, edge);
      }
  // the apple, in Claude's orange, breathing
  uint32_t now = millis();
  float pulse = 0.5f + 0.5f * sinf(now / 260.0f);
  cell(apple_x, apple_y, lv_color_mix(lv_color_hex(0xFFB38A), lv_color_hex(0xD97757), (lv_opa_t)(255 * pulse)), 2 - (int)(pulse + 0.5f));
  // the snake: the saver's gradient, the head brightest; a fresh apple flashes down the body
  lv_color_t head_c = lv_color_hex(0x7FF0B0), tail_c = lv_color_hex(0x1E6B58);
  for (int i = len - 1; i >= 0; i--) {
    float f = len > 1 ? (float)i / (len - 1) : 0;
    lv_color_t c = lv_color_mix(tail_c, head_c, (lv_opa_t)(255 * f));
    uint32_t since = now - ate_at;
    if (since < 400 && abs((int)(since / 25) - i) < 2) c = lv_color_hex(0xFFFFFF);
    cell(body_x[at(i)], body_y[at(i)], c, i == 0 ? 0 : 1);
  }
  if (state == OVER) {  // the head that crashed, in red
    cell(body_x[at(0)], body_y[at(0)], lv_color_hex(0xFF5A4E), 0);
  }
  lv_obj_invalidate(board);
}

static void labels()
{
  set_text(score_label, state == READY ? "" : "L" + String(level) + " · " + String(score));
  bool card = state != RUNNING;
  card ? lv_obj_clear_flag(title_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);
  card ? lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
  set_text(title_label, state == READY ? String("SNAKE") : state == PAUSED ? String("PAUSED") : state == LEVELUP ? "LEVEL " + String(level) : String("GAME OVER"));
  lv_obj_set_style_text_color(title_label, lv_color_hex(state == OVER ? C_ATTENTION : state == LEVELUP ? C_DONE : C_INK), 0);
  String best_line = best > 0 ? "best " + String(best) : "";
  int rings = RING_COUNT[level - 1];
  String hint;
  if (state == READY) hint = String("turn the crown to steer\ntap to start\na new level every ") + APPLES_PER_LEVEL + " apples";
  else if (state == PAUSED) hint = "tap to go on";
  else if (state == LEVELUP) hint = String(rings) + (rings == 1 ? " ring" : " rings") + " · find the gaps";
  else hint = (score >= best && score > 0 ? String("new best!") : best_line) + "\nlevel " + level + " · tap to play again";
  set_text(hint_label, hint);
}

static void save_best()
{
  if (!best_dirty) return;
  best_dirty = false;
  Preferences prefs;  // the LVGL task: flash writes are safe here
  prefs.begin("deck", false);
  prefs.putUShort("snakeBest", best);
  prefs.end();
}

static void wrap(int &nx, int &ny);

// The snake, short, where it has the longest free way ahead, in whichever of the four directions (the edge wraps, so
// a line that only meets the disc's edge is free all round). Level 5's rows all meet a wall within 3 cells heading
// right: a fixed direction started it at a wall.
static void start_level()
{
  build_walls();
  static const int DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  int hx0 = GRID / 2, hy0 = GRID / 2 + 3, bdx = 1, bdy = 0, best_room = -1000;
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      for (const auto &d : DIRS) {
        bool body_free = true;  // the head here, the body three cells behind it
        for (int k = 0; k < 4 && body_free; k++) body_free = !blocked(x - k * d[0], y - k * d[1]);
        if (!body_free) continue;
        dx = d[0], dy = d[1];  // wrap() reads the heading
        int hx = x, hy = y, ahead = 0;
        while (ahead < GRID) {
          int nx = hx + dx, ny = hy + dy;
          if (!on_board(nx, ny)) wrap(nx, ny);
          bool body = false;
          for (int k = 0; k < 4; k++) body |= nx == x - k * d[0] && ny == y - k * d[1];
          if (blocked(nx, ny) || body) break;
          hx = nx, hy = ny, ahead++;
        }
        int room = min(ahead, 12) * 4 - abs(y - (GRID / 2 + 3)) - abs(x - GRID / 2) / 2;  // enough room, then near the lower middle
        if (room > best_room) best_room = room, hx0 = x, hy0 = y, bdx = d[0], bdy = d[1];
      }
  dx = bdx, dy = bdy;
  len = 4;
  head = 3;
  for (int i = 0; i < len; i++) body_x[i] = hx0 - (3 - i) * dx, body_y[i] = hy0 - (3 - i) * dy;
  queued = 0;
  step_ms = STEP_START_MS - (level - 1) * STEP_LEVEL_MS;
  place_apple();
}

static void reset()
{
  score = 0;
  level = 1;
  level_apples = 0;
  start_level();
}

static void crash()
{
  state = OVER;
  haptic_buzz(2);
  if (score > best) best = score, best_dirty = true;
  save_best();
  labels();
  draw();
}

// Off the disc: back in on the far side of the same row (or column), heading the same way (user, 2026-11-15: «може
// проходити по межам екрану… і не вмирати»). Only the snake itself and the ring walls kill.
static void wrap(int &nx, int &ny)
{
  if (dx) {
    for (int i = 0; i < GRID; i++) {
      int x = dx > 0 ? i : GRID - 1 - i;
      if (on_board(x, ny)) return void(nx = x);
    }
  } else {
    for (int i = 0; i < GRID; i++) {
      int y = dy > 0 ? i : GRID - 1 - i;
      if (on_board(nx, y)) return void(ny = y);
    }
  }
}

static void step()
{
  if (queued) {  // right: (dx, dy) → (−dy, dx) on a y-down screen
    int t = turns[0];
    turns[0] = turns[1];
    queued--;
    int ndx = t > 0 ? -dy : dy, ndy = t > 0 ? dx : -dx;
    dx = ndx, dy = ndy;
  }
  int nx = body_x[head] + dx, ny = body_y[head] + dy;
  if (!on_board(nx, ny)) wrap(nx, ny);
  bool eats = nx == apple_x && ny == apple_y;
  if (blocked(nx, ny) || on_snake(nx, ny, eats ? 0 : 1)) return crash();
  head = (head + 1) % MAX_LEN;
  body_x[head] = nx, body_y[head] = ny;
  if (eats) {
    len++;
    score++;
    pet_event(PET_SNAKE_APPLE);  // a game with Clawd: joy (pet.cpp)
    ate_at = millis();
    step_ms = max(STEP_MIN_MS, (int)step_ms - STEP_FASTER_MS);
    if (++level_apples >= APPLES_PER_LEVEL && level < LEVELS) {  // the next level: new walls, a short snake
      level++;
      level_apples = 0;
      start_level();
      state = LEVELUP;
      level_until = millis() + LEVEL_CARD_MS;
      haptic_click(3);
    } else {
      haptic_click();
      place_apple();
    }
    labels();
  }
}

static void turn(int step_dir)
{
  if (state != RUNNING) return;
  if (queued < 2) turns[queued++] = step_dir > 0 ? 1 : -1;
  haptic_tick();
}

static void tap()
{
  if (state == RUNNING) state = PAUSED;
  else if (state == LEVELUP) state = RUNNING, last_step = millis();  // skip the card
  else {
    if (state != PAUSED) reset();
    state = RUNNING;
    last_step = millis();
  }
  haptic_click();
  labels();
  draw();
}

void snake_tick()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  uint32_t now = millis();
  if (state == LEVELUP && now >= level_until) {
    state = RUNNING;
    last_step = now + 700;  // a beat to find the crown before the first step (level 5 has ~8 free cells ahead)
    labels();
  }
  if (state == RUNNING && (int32_t)(now - last_step) >= (int32_t)step_ms) {
    last_step = now;
    step();
  }
  if (now - last_draw >= 40) {  // ~25 fps: the apple breathes, the flash runs down the body
    last_draw = now;
    draw();
  }
}

static String subtitle()
{
  if (state == RUNNING || state == PAUSED || state == LEVELUP) return "paused · L" + String(level) + " · " + String(score);
  return best ? "best " + String(best) : "turn the crown to steer";
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  if (state == READY && len == 0) reset();
  labels();
  draw();
}

static void hide()
{
  if (state == RUNNING || state == LEVELUP) state = PAUSED;
  save_best();
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

// Tests: jump to level n (its walls, the card), as if its apples were eaten.
void snake_test_level(int n)
{
  if (state == READY || state == OVER) reset();
  level = constrain(n, 1, LEVELS);
  level_apples = 0;
  start_level();
  state = LEVELUP;
  level_until = millis() + LEVEL_CARD_MS;
  labels();
  draw();
}

String snake_report()
{
  static const char *NAMES[] = {"ready", "running", "paused", "over", "levelup"};
  return String(NAMES[state]) + " level=" + level + "/" + level_apples + " score=" + score + " best=" + best + " len=" + len + " head=" + body_x[head] + "," + body_y[head] +
         " dir=" + dx + "," + dy + " apple=" + apple_x + "," + apple_y + " step=" + step_ms + "ms";
}

void snake_create()
{
  Preferences prefs;
  prefs.begin("deck", true);
  best = prefs.getUShort("snakeBest", 0);
  prefs.end();

  page = lv_obj_create(lv_scr_act());
  lv_obj_set_size(page, 360, 360);
  lv_obj_center(page);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

  buf = (lv_color_t *)ps_malloc(360 * 360 * sizeof(lv_color_t));
  if (buf) {
    memset(buf, 0, 360 * 360 * sizeof(lv_color_t));
    board = lv_canvas_create(page);
    lv_canvas_set_buffer(board, buf, 360, 360, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(board);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_CLICKABLE);
  }
  score_label = make_label(page, &deck_bold_16, C_DETAIL, -150, 80);
  title_label = make_label(page, &deck_bold_30, C_INK, -78, 260);
  lv_obj_set_style_bg_color(title_label, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(title_label, LV_OPA_70, 0);
  hint_label = make_label(page, &deck_14, C_DETAIL, -24, 240);
  lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(hint_label, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(hint_label, LV_OPA_70, 0);
  lv_obj_set_style_text_line_space(hint_label, 4, 0);
}

const App app_snake = {"Snake", LV_SYMBOL_LOOP, 0x3DDC84, show, hide, subtitle, turn, tap, nullptr, nullptr};

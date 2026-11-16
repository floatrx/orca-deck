// Catch — the pet's own game (user, 2026-11-16: «окрему апку … взаємодіяти з улюбленцем … пограти з ним»). Clawd runs
// along the bottom of the round screen; the crown moves him left and right. Apples fall (food), now and then a star
// (a bonus), and bugs (dodge them). A round lasts 30 s and speeds up; the result feeds his gauges
// (pet_game_result). A tap ends the round early; the result screen's tap goes back to the Clawd app.
#include "deck.h"

#define ROUND_MS 30000
#define PW 6                  // Clawd: 108 × 50 px
#define PH 10
#define SW (18 * PW)
#define SH (9 * PH)           // his canvas: the sprite plus room for a hop
#define FLOOR_Y 262           // his feet
#define STEP_PX 20            // one crown detent
#define MAX_ITEMS 8
#define ITEM 14

enum Kind { APPLE, STAR, BUG };
struct Item {
  bool live = false;
  Kind kind = APPLE;
  float x = 0, y = 0, vy = 0;
};

static lv_obj_t *page, *board, *score_label, *time_label, *result_label;
static lv_color_t *buf, *sprite;
static bool active = false, over = false;
static Item items[MAX_ITEMS];
static float clawd_x = 180, shown_x = 180;
static int apples = 0, stars = 0, bugs = 0;
static uint32_t started = 0, last_spawn = 0, last_step = 0, hit_at = 0;
static bool hit_bad = false;

bool catch_active() { return active; }

static void fill(int x0, int y0, int w, int h, lv_color_t c)
{
  for (int y = max(0, y0); y < min(360, y0 + h); y++)
    for (int x = max(0, x0); x < min(360, x0 + w); x++) buf[y * 360 + x] = c;
}

static void draw_item(const Item &it)
{
  int x = (int)it.x - ITEM / 2, y = (int)it.y - ITEM / 2;
  if (it.kind == APPLE) {
    fill(x + 2, y + 3, ITEM - 4, ITEM - 3, lv_color_hex(0xE5484D));
    fill(x + 1, y + 5, ITEM - 2, ITEM - 7, lv_color_hex(0xE5484D));
    fill(x + ITEM / 2, y, 3, 4, lv_color_hex(0x3DDC84));
  } else if (it.kind == STAR) {
    fill(x + ITEM / 2 - 2, y, 4, ITEM, lv_color_hex(0xFFD166));
    fill(x, y + ITEM / 2 - 2, ITEM, 4, lv_color_hex(0xFFD166));
    fill(x + 3, y + 3, ITEM - 6, ITEM - 6, lv_color_hex(0xFFD166));
  } else {
    fill(x + 1, y + 3, ITEM - 2, ITEM - 6, lv_color_hex(0x3E8E41));
    for (int k = 0; k < 3; k++) fill(x - 1, y + 3 + k * 3, ITEM + 2, 1, lv_color_hex(0x1F4D22));  // legs
    fill(x + 3, y + 1, 2, 2, lv_color_hex(0x1F4D22)), fill(x + ITEM - 5, y + 1, 2, 2, lv_color_hex(0x1F4D22));
  }
}

// The x range Clawd may run at his row: the disc's chord there, less his half-width.
static float limit_x()
{
  float dy = FLOOR_Y - 180.0f, half = sqrtf(170.0f * 170.0f - dy * dy);
  return half - SW / 2.0f + 6;
}

static void draw(uint32_t now)
{
  if (!buf || !sprite) return;
  memset(buf, 0, 360 * 360 * sizeof(lv_color_t));
  for (int x = 40; x < 320; x += 6) fill(x, FLOOR_Y + 2, 3, 2, lv_color_hex(0x1A1B1F));  // the floor, dotted
  for (const Item &it : items)
    if (it.live) draw_item(it);
  // Clawd: his own drawing (his stage and look), small, blitted at his x
  PetLook look = pet_look();
  look.reaction = PET_REACT_NONE;
  look.sleeping = look.hungry = false;  // playing: wide awake, no thought bubble
  String mood = over ? "done" : (now - hit_at < 300 && hit_bad) ? "attention" : "working";  // running legs while playing
  clawd_draw(sprite, SW, SH, PW, PH, 3 * PH, mood, now, false, lv_color_hex(0x000000), false, &look);
  int x0 = (int)shown_x - SW / 2, y0 = FLOOR_Y - SH + PH;
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      lv_color_t c = sprite[y * SW + x];
      if (c.full && x0 + x >= 0 && x0 + x < 360 && y0 + y >= 0 && y0 + y < 360) buf[(y0 + y) * 360 + x0 + x] = c;
    }
  lv_obj_invalidate(board);
}

static void labels()
{
  set_text(score_label, String(L("apples ", "яблук ")) + apples + (stars ? L(" · stars ", " · зірок ") + String(stars) : ""));
  uint32_t left = over ? 0 : (ROUND_MS - min((uint32_t)ROUND_MS, millis() - started)) / 1000;
  set_text(time_label, over ? "" : String(left) + L(" s · crown: run · tap: stop", " с · колесо: біжи · тап: стоп"));
}

static void finish()
{
  over = true;
  for (Item &it : items) it.live = false;  // the board clears for the result
  pet_game_result(apples, stars, bugs);
  haptic_click(3);
  set_text(result_label, String(L("apples ", "яблук ")) + apples + L(" · stars ", " · зірок ") + stars + L(" · bugs ", " · багів ") + bugs +
                             L("\nhe loved it!\n\ntap: back", "\nйому сподобалось!\n\nтап: назад"));
  lv_obj_clear_flag(result_label, LV_OBJ_FLAG_HIDDEN);
  labels();
}

void catch_start()
{
  for (Item &it : items) it.live = false;
  apples = stars = bugs = 0;
  clawd_x = shown_x = 180;
  over = false;
  active = true;
  started = last_spawn = last_step = millis();
  lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  labels();
  draw(millis());
}

void catch_abort() { active = false; if (page) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }  // left mid-round: no reward

static void close_game()
{
  active = false;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

void catch_turn(int step)
{
  if (over) return;
  float lim = limit_x(), next = fmaxf(180 - lim, fminf(180 + lim, clawd_x + (step > 0 ? STEP_PX : -STEP_PX)));
  if (next == clawd_x) return;  // the wall: no tick
  clawd_x = next;
  haptic_tick();
}

void catch_tap()
{
  if (!over) return finish();  // the round ends early
  haptic_click();
  close_game();
}

void catch_tick()
{
  if (!active) return;
  uint32_t now = millis();
  if (now - last_step < 33) return;  // ~30 fps
  float dt = (now - last_step) / 1000.0f;
  last_step = now;
  shown_x += (clawd_x - shown_x) * fminf(1, dt * 14);  // he runs to where the crown put him, not teleports
  if (!over) {
    float t = (now - started) / (float)ROUND_MS;  // 0 → 1 over the round: faster and denser
    if (now - last_spawn > (uint32_t)(900 - 450 * t)) {
      last_spawn = now;
      for (Item &it : items) {
        if (it.live) continue;
        int r = random(0, 100);
        it.kind = r < 14 ? BUG : r < 20 ? STAR : APPLE;
        float lim = limit_x() + SW / 2.0f - 20;
        it.x = 180 + random(-(int)lim, (int)lim);
        it.y = 40;
        it.vy = 70 + 90 * t + random(0, 30);
        it.live = true;
        break;
      }
    }
    for (Item &it : items) {
      if (!it.live) continue;
      it.y += it.vy * dt;
      bool at_him = it.y >= FLOOR_Y - 5 * PH && it.y <= FLOOR_Y && fabsf(it.x - shown_x) < SW / 2.0f;
      if (at_him) {
        it.live = false;
        hit_at = now;
        hit_bad = it.kind == BUG;
        if (it.kind == APPLE) apples++, haptic_tick();
        else if (it.kind == STAR) stars++, haptic_click(2);
        else bugs++, haptic_buzz(1);
        labels();
      } else if (it.y > FLOOR_Y + 20) {
        it.live = false;  // missed
      }
    }
    if (now - started >= ROUND_MS) finish();
    static uint32_t last_label = 0;
    if (now - last_label > 500) last_label = now, labels();
  }
  draw(now);
}

String catch_report()
{
  return String(active ? (over ? "over" : "playing") : "off") + " apples=" + apples + " stars=" + stars + " bugs=" + bugs +
         " x=" + (int)clawd_x;
}

void catch_create()
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
  buf = (lv_color_t *)ps_malloc(360 * 360 * sizeof(lv_color_t));
  sprite = (lv_color_t *)ps_malloc(SW * SH * sizeof(lv_color_t));
  if (buf && sprite) {
    board = lv_canvas_create(page);
    lv_canvas_set_buffer(board, buf, 360, 360, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(board);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_CLICKABLE);
  }
  score_label = make_label(page, &deck_bold_16, C_INK, -146, 220);
  time_label = make_label(page, &deck_14, C_MUTED, -124, 240);
  result_label = make_label(page, &deck_bold_16, C_INK, -30, 260);
  lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(result_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(result_label, 6, 0);
  lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
}

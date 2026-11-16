// The hub as a honeycomb of round icons (Settings → Hub layout · user, 2026-11-16: «щось Apple Watch-like, така
// сітка кругленька … іконки великі, щоб палець … не промазав … 44 пікселя радіус» and «ті, що по краям, чуть-чуть
// менші … у центрі більші … придумай щось по UX-у підходяще для шайби»).
//   Rows of 3 and 2, offset; an icon is 88 px across (radius 44) at the centre. A fisheye toward the round rim:
//   icons shrink and are drawn in, so the ring shows more of the list while the one under a thumb stays big.
//   The crown walks a focus ring icon by icon and the grid glides (up / down and sideways) to put it in the middle,
//   at full size; a tap opens the icon
//   under the finger (or the focused one when the tap misses). The focused app's name and live line sit at the bottom.
#include "deck.h"
#include <math.h>

#define MAX_ICONS 20
#define ICON 88          // at the centre (radius 44)
#define PITCH_X 96       // 88 + 8 between neighbours
#define PITCH_Y 82       // honeycomb: ~0.86 × PITCH_X
#define FLAT_R 64        // full size inside this radius
#define RIM_R 172        // the edge of the glass
#define MIN_SCALE 0.34f
#define GLIDE 12.0f      // 1/s: how fast the rows settle on the focus

static lv_obj_t *layer, *ring, *name_label, *sub_label;
static lv_obj_t *icons[MAX_ICONS], *glyphs[MAX_ICONS], *hands[MAX_ICONS][2], *mics[MAX_ICONS], *canvases[MAX_ICONS];
static lv_color_t *canvas_buf[MAX_ICONS];
static lv_point_t hand_pts[MAX_ICONS][2][2];
static const App *const *apps = nullptr;
static int count = 0, focus = 0;
static float scroll = 0, target = 0;   // the grid's vertical offset (px): eases to `target`
static float pan = 0, pan_target = 0;  // and its sideways one: the focused icon always sits in the middle, biggest
static uint32_t last_frame = 0, last_draw = 0;
static bool shown = false;
struct Place { float x, y, size; bool visible; };
static Place places[MAX_ICONS];

// Row r holds 3 icons when even, 2 when odd. Index → its row and column.
static void cell(int i, int &row, int &col, int &cols)
{
  row = 0;
  int left = i;
  for (;;) {
    cols = row % 2 ? 2 : 3;
    if (left < cols) break;
    left -= cols;
    row++;
  }
  col = left;
}

static void grid_xy(int i, float &x, float &y)
{
  int row, col, cols;
  cell(i, row, col, cols);
  x = (col - (cols - 1) / 2.0f) * PITCH_X;
  y = row * PITCH_Y;
}

// The fisheye: inside FLAT_R nothing changes; outside, the distance is compressed and the icon shrinks.
static void lens(float x, float y, Place &p)
{
  float d = sqrtf(x * x + y * y);
  float s = 1.0f, k = 1.0f;
  if (d > FLAT_R) {
    float t = fminf(1.0f, (d - FLAT_R) / (RIM_R + 40 - FLAT_R));
    s = fmaxf(MIN_SCALE, 1.0f - 0.66f * t * t * (3 - 2 * t));  // smoothstep: no kink at the flat edge
    k = (FLAT_R + (d - FLAT_R) * 0.78f) / d;                  // drawn in toward the centre
  }
  p.x = x * k;
  p.y = y * k;
  p.size = ICON * s;
  p.visible = sqrtf(p.x * p.x + p.y * p.y) - p.size / 2 < RIM_R;
}

static bool is_claude(const App *a) { return a == &app_deck || a == &app_pet; }

static void clock_hands(int i, int size)
{
  time_t now = time(nullptr) + tz_offset_min() * 60;
  struct tm t;
  gmtime_r(&now, &t);
  float minute = (t.tm_min + t.tm_sec / 60.0f) / 60 * 2 * M_PI, hour = (t.tm_hour % 12 + t.tm_min / 60.0f) / 12 * 2 * M_PI;
  float len[2] = {size * 0.24f, size * 0.36f}, ang[2] = {hour, minute};
  int c = size / 2;
  for (int h = 0; h < 2; h++) {
    hand_pts[i][h][0] = {(lv_coord_t)c, (lv_coord_t)c};
    hand_pts[i][h][1] = {(lv_coord_t)(c + len[h] * sinf(ang[h])), (lv_coord_t)(c - len[h] * cosf(ang[h]))};
    lv_line_set_points(hands[i][h], hand_pts[i][h], 2);
  }
}

static void layout()
{
  for (int i = 0; i < MAX_ICONS; i++) {
    if (i >= count) {
      lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    float gx, gy;
    grid_xy(i, gx, gy);
    Place &p = places[i];
    lens(gx + pan, gy + scroll, p);
    if (!p.visible) {
      lv_obj_add_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    int size = (int)p.size;
    lv_obj_clear_flag(icons[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(icons[i], size, size);
    lv_obj_align(icons[i], LV_ALIGN_CENTER, (int)p.x, (int)p.y);
    // the further out, the dimmer: the eye goes to the middle
    float d = sqrtf(p.x * p.x + p.y * p.y);
    lv_obj_set_style_opa(icons[i], (lv_opa_t)(d < FLAT_R ? 255 : max(90, (int)(255 - (d - FLAT_R) * 1.3f))), 0);
    const App *a = apps[i];
    bool special = is_claude(a) || a == &app_clock || a == &app_mic;
    lv_obj_set_style_text_font(glyphs[i], size >= 62 ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    special || size < 34 ? lv_obj_add_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN);
    if (a == &app_clock) clock_hands(i, size);
    lv_obj_center(mics[i]);
    if (canvases[i]) lv_obj_center(canvases[i]);
  }
  if (focus < count && places[focus].visible) {
    int size = (int)places[focus].size + 12;
    lv_obj_set_size(ring, size, size);
    lv_obj_align(ring, LV_ALIGN_CENTER, (int)places[focus].x, (int)places[focus].y);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  }
}

static void paint_icons()
{
  for (int i = 0; i < count && i < MAX_ICONS; i++) {
    const App *a = apps[i];
    bool claude = is_claude(a);
    lv_obj_set_style_bg_color(icons[i], lv_color_hex(claude ? 0x000000 : a->color), 0);
    lv_obj_set_style_border_width(icons[i], claude ? 2 : 0, 0);  // a black badge needs an edge on the black glass
    set_text(glyphs[i], a->icon);
    for (int h = 0; h < 2; h++) a == &app_clock ? lv_obj_clear_flag(hands[i][h], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(hands[i][h], LV_OBJ_FLAG_HIDDEN);
    a == &app_mic ? lv_obj_clear_flag(mics[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(mics[i], LV_OBJ_FLAG_HIDDEN);
    if (a == &app_mic && shared_mux) {
      xSemaphoreTake(shared_mux, portMAX_DELAY);
      bool muted = shared.mac.mic_muted;
      xSemaphoreGive(shared_mux);
      mic_glyph_set(mics[i], C_BG, muted);
    }
    if (canvases[i]) claude ? lv_obj_clear_flag(canvases[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(canvases[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void grid_set(const App *const *list, int n, int f)
{
  bool fresh = list != apps || n != count;
  apps = list;
  count = min(n, MAX_ICONS);
  focus = max(0, min(count - 1, f));
  float gx, gy;
  grid_xy(focus, gx, gy);
  target = -gy;
  pan_target = -gx;
  if (fresh) scroll = target, pan = pan_target;  // a new list (the folder) starts in place, no glide from the old one
  paint_icons();
  if (focus < count) {
    const App *a = apps[focus];
    set_text(name_label, hub_shown_name(a));
    set_text(sub_label, a->subtitle && shared_mux ? a->subtitle() : String(""));
  }
  last_frame = millis();
  layout();
}

void grid_show(bool on)
{
  if (!layer || on == shown) return;
  shown = on;
  on ? lv_obj_clear_flag(layer, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
}

int grid_hit(int x, int y)
{
  int best = -1;
  float best_d = 1e9;
  for (int i = 0; i < count; i++) {
    const Place &p = places[i];
    if (!p.visible) continue;
    float dx = x - p.x, dy = y - p.y, d = sqrtf(dx * dx + dy * dy);
    if (d <= p.size / 2 + 8 && d < best_d) best = i, best_d = d;  // 8 px of slack round each icon
  }
  return best;
}

void grid_tick(uint32_t now)
{
  if (!shown || !apps) return;
  if (fabsf(scroll - target) > 0.3f || fabsf(pan - pan_target) > 0.3f) {
    float dt = min(0.05f, (now - last_frame) / 1000.0f), k = fminf(1.0f, dt * GLIDE);
    scroll += (target - scroll) * k;
    pan += (pan_target - pan) * k;
    if (fabsf(scroll - target) <= 0.3f) scroll = target;
    if (fabsf(pan - pan_target) <= 0.3f) pan = pan_target;
    layout();
  }
  last_frame = now;
  if (now - last_draw < 80) return;  // Clawd on the Claude and Clawd icons, ~12 fps
  last_draw = now;
  String mood = claude_mood();
  PetLook look = pet_look();
  for (int i = 0; i < count; i++) {
    if (!canvases[i] || !canvas_buf[i] || !is_claude(apps[i]) || !places[i].visible) continue;
    bool pet = apps[i] == &app_pet;
    clawd_draw(canvas_buf[i], ICON, ICON, 3, 5, (ICON - 5 * 5) / 2 + 2, mood, now, false, lv_color_hex(0x000000), false,
               pet && pet_born() ? &look : nullptr);
    lv_obj_invalidate(canvases[i]);
  }
}

String grid_report()
{
  String out = String("grid focus=") + focus + " scroll=" + String(scroll, 1) + " pan=" + String(pan, 1) + " visible=";
  for (int i = 0; i < count; i++)
    if (places[i].visible) out += String(i) + "(" + (int)places[i].size + ") ";
  return out;
}

void grid_create(lv_obj_t *parent)
{
  layer = lv_obj_create(parent);
  lv_obj_set_size(layer, 360, 360);
  lv_obj_center(layer);
  lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(layer, 0, 0);
  lv_obj_set_style_pad_all(layer, 0, 0);
  lv_obj_clear_flag(layer, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);

  ring = lv_obj_create(layer);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ring, 3, 0);
  lv_obj_set_style_border_color(ring, lv_color_hex(C_INK), 0);
  lv_obj_clear_flag(ring, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  int claude_canvases = 0;
  for (int i = 0; i < MAX_ICONS; i++) {
    lv_obj_t *icon = icons[i] = lv_obj_create(layer);
    lv_obj_set_size(icon, ICON, ICON);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(icon, true, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(0x2A2B30), 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    // Clawd draws into the first two icons' canvases only: the Claude and Clawd apps sit there in the hub's list.
    if (i < 2 && claude_canvases < 2) {
      canvas_buf[i] = (lv_color_t *)ps_malloc(ICON * ICON * sizeof(lv_color_t));
      if (canvas_buf[i]) {
        canvases[i] = lv_canvas_create(icon);
        lv_canvas_set_buffer(canvases[i], canvas_buf[i], ICON, ICON, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(canvases[i]);
        lv_obj_clear_flag(canvases[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(canvases[i], LV_OBJ_FLAG_HIDDEN);
        claude_canvases++;
      }
    }
    for (int h = 0; h < 2; h++) {
      lv_obj_t *hand = hands[i][h] = lv_line_create(icon);
      lv_obj_set_pos(hand, 0, 0);
      lv_obj_set_style_line_width(hand, h ? 3 : 4, 0);
      lv_obj_set_style_line_color(hand, lv_color_hex(C_BG), 0);
      lv_obj_set_style_line_rounded(hand, true, 0);
      lv_obj_add_flag(hand, LV_OBJ_FLAG_HIDDEN);
    }
    mics[i] = mic_glyph(icon, ICON * 6 / 10);
    lv_obj_center(mics[i]);
    lv_obj_add_flag(mics[i], LV_OBJ_FLAG_HIDDEN);
    glyphs[i] = lv_label_create(icon);
    lv_obj_set_style_text_color(glyphs[i], lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_font(glyphs[i], &lv_font_montserrat_28, 0);
    lv_obj_center(glyphs[i]);
  }
  lv_obj_move_foreground(ring);

  // The focused app, named: the icons carry no labels (watchOS-like), so the bottom says where the ring is.
  name_label = make_label(layer, &deck_bold_16, C_INK, 128, 200);
  lv_obj_set_style_bg_color(name_label, lv_color_hex(0x0B0C0E), 0);
  lv_obj_set_style_bg_opa(name_label, LV_OPA_80, 0);
  lv_obj_set_style_pad_hor(name_label, 10, 0);
  lv_obj_set_style_pad_ver(name_label, 3, 0);
  lv_obj_set_style_radius(name_label, 12, 0);
  sub_label = make_label(layer, &deck_14, C_DETAIL, 150, 170);
  lv_label_set_long_mode(sub_label, LV_LABEL_LONG_DOT);
}

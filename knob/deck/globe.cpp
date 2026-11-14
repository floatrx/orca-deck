// The hub's background: a dotted Earth turning slowly, as the factory firmware's timer (user's pick, 2026-11-14:
// "the globe, no satellites"). Land is a bright 2×2 dot, sea a faint 1 px one. No atmosphere glow: the user had it
// removed (a blue rim while agents worked).
//
// Cheap per frame: every dot's latitude and base longitude are worked out once (orthographic, tilted); a frame
// copies the static background (the night side), shifts the longitudes and plots ~1800 dots into one canvas.
#include "deck.h"
#include <math.h>
#include "earth_mask.h"

#define SIZE 360          // the canvas: the whole round screen (user, 2026-11-14: «на весь екран»)
#define R 178             // globe radius: edge to edge
#define STEP 6            // dot pitch, px: ~2800 dots across the whole face
#define TILT 0.38f        // the north pole leans toward the viewer, rad
#define DEG_PER_S 9.0f    // one turn in 40 s

struct Dot {
  int16_t x, y;      // canvas pixel
  uint8_t row;       // mask row (latitude)
  uint16_t lon;      // base longitude in 1/64 mask columns
  uint8_t shade;     // 0–3: lit side brighter
};

static lv_obj_t *canvas;
static lv_color_t *buf, *background;
static Dot *dots;
static int dot_count = 0;
static lv_color_t land[4], sea[4];
static uint32_t frames = 0, frame_ms_sum = 0, frame_ms_worst = 0, draw_us_sum = 0;

static inline bool is_land(int row, int col)
{
  return EARTH_MASK[row][col >> 3] & (0x80 >> (col & 7));
}

static void build_dots()
{
  dots = (Dot *)ps_malloc(sizeof(Dot) * 4000);
  for (int y = -R; y <= R; y += STEP) {
    for (int x = -R; x <= R; x += STEP) {
      int d = x * x + y * y;
      if (d > R * R || dot_count >= 4000) continue;
      float z = sqrtf((float)(R * R - d));
      float y2 = y * cosf(TILT) - z * sinf(TILT), z2 = y * sinf(TILT) + z * cosf(TILT);
      float lat = -asinf(y2 / R) * 180 / M_PI, lon = atan2f((float)x, z2) * 180 / M_PI;
      Dot &dot = dots[dot_count++];
      dot.x = SIZE / 2 + x;
      dot.y = SIZE / 2 + y;
      dot.row = (uint8_t)min(EARTH_H - 1, max(0, (int)((90 - lat) / 180 * EARTH_H)));
      dot.lon = (uint16_t)((lon + 180) / 360 * EARTH_W * 64);
      dot.shade = (uint8_t)min(3, (int)(z / R * 4));
    }
  }
}

// The night side of the globe, black around it.
static void paint_background()
{
  lv_color_t night = lv_color_hex(0x07080B), black = lv_color_hex(C_BG);
  for (int y = 0; y < SIZE; y++) {
    for (int x = 0; x < SIZE; x++) {
      int dx = x - SIZE / 2, dy = y - SIZE / 2;
      background[y * SIZE + x] = dx * dx + dy * dy <= R * R ? night : black;
    }
  }
}

void globe_tick(uint32_t now)
{
  static uint32_t last = 0;
  if (now - last < 25) return;  // once per display refresh (30 ms)
  if (now - last < 200) {
    frames++;
    frame_ms_sum += now - last;
    frame_ms_worst = max(frame_ms_worst, now - last);
  }
  last = now;
  uint32_t t0 = micros();
  memcpy(buf, background, SIZE * SIZE * sizeof(lv_color_t));
  // West to east, as the real one: what the viewer sees drifts to the right.
  const int32_t turn = EARTH_W * 64;
  int32_t shift = (int32_t)((uint64_t)now * (uint64_t)(DEG_PER_S * EARTH_W * 64 / 360) / 1000 % turn);
  for (int i = 0; i < dot_count; i++) {
    const Dot &d = dots[i];
    int col = ((d.lon - shift + turn) % turn) >> 6;
    lv_color_t *p = buf + d.y * SIZE + d.x;
    if (is_land(d.row, col)) {
      lv_color_t c = land[d.shade];
      p[0] = p[1] = p[SIZE] = p[SIZE + 1] = c;
    } else {
      p[0] = sea[d.shade];
    }
  }
  lv_obj_invalidate(canvas);
  draw_us_sum += micros() - t0;
}

String globe_report()
{
  if (!frames) return "";
  String out = " frame=" + String(frame_ms_sum / frames) + "ms worst=" + String(frame_ms_worst) + "ms draw=" +
               String(draw_us_sum / frames / 1000.0f, 1) + "ms dots=" + String(dot_count);
  frames = frame_ms_sum = frame_ms_worst = draw_us_sum = 0;
  return out;
}

void globe_create(lv_obj_t *parent)
{
  buf = (lv_color_t *)ps_malloc(SIZE * SIZE * sizeof(lv_color_t));
  background = (lv_color_t *)ps_malloc(SIZE * SIZE * sizeof(lv_color_t));
  if (!buf || !background) return;
  for (int s = 0; s < 4; s++) {
    land[s] = lv_color_mix(lv_color_hex(0xE1E8F0), lv_color_hex(0x07080B), (lv_opa_t)(90 + s * 50));
    sea[s] = lv_color_mix(lv_color_hex(0x5A6E8C), lv_color_hex(0x07080B), (lv_opa_t)(40 + s * 18));
  }
  build_dots();
  paint_background();
  canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, buf, SIZE, SIZE, LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(canvas);
  lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  memcpy(buf, background, SIZE * SIZE * sizeof(lv_color_t));
  lv_obj_move_background(canvas);
}

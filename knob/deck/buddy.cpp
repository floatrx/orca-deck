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

static lv_obj_t *page, *canvas, *zz, *speech;  // speech: what the pet says, for a few seconds (pet_said)
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
static const char *const EGG[ROWS] = {
    ".......####.......",
    "......######......",
    ".....########.....",
    ".....########.....",
    "......######......",
};

// The pet's layer over Clawd (pet.cpp, .work/plans/clawd-pet.md): his look (an adult's accessory, x eyes when dead)
// and the reactions to Claude work, in sprite cells (pw × ph) from the sprite's top-left (x0, y0).
static void pet_overlay(lv_color_t *b, int bw, int bh, int pw, int ph, int x0, int y0, int base_y, uint32_t now,
                        lv_color_t bg, bool big, const PetLook &look, const String &mood)
{
  int u = max(1, pw / 3);  // a line's thickness
  auto rect = [&](float c, float r, float w, float h, uint32_t col) {
    fill(b, bw, bh, x0 + (int)(c * pw), y0 + (int)(r * ph), max(1, (int)(w * pw)), max(1, (int)(h * ph)), lv_color_hex(col));
  };
  if (look.stage == PET_DEAD) {  // x eyes
    for (int c : {4, 11})
      for (int k = 0; k < 2 * pw; k++) {
        int yy = y0 + ph + k * ph / (2 * pw), x1 = x0 + c * pw + k, x2 = x0 + c * pw + 2 * pw - 1 - k;
        fill(b, bw, bh, x1, yy, u, u, lv_color_hex(0x3A4250)), fill(b, bw, bh, x2, yy, u, u, lv_color_hex(0x3A4250));
      }
    return;
  }
  uint8_t phx = look.phase;
  bool sleeping = mood == "sleep";
  // § age artifacts: what he carries tells how old he is
  if (phx == PHASE_NEWBORN) {  // the shell still on his head
    rect(4, -0.55f, 10, 0.55f, 0xF6EEDF);
    for (int k = 0; k < 5; k++) rect(4 + k * 2, 0.0f, 1, 0.25f, 0xF6EEDF);  // its zigzag edge
  } else if (phx == PHASE_BABY && !sleeping) {  // a pacifier
    rect(8.0f, 2.2f, 2, 0.5f, 0xFF7A9A), rect(8.6f, 2.6f, 0.8f, 0.35f, 0xFFD1DC);
  } else if (phx == PHASE_TODDLER) {  // a sprout
    rect(8.8f, -0.9f, 0.5f, 0.9f, 0x3DDC84), rect(9.3f, -1.1f, 1.4f, 0.4f, 0x3DDC84), rect(7.6f, -0.8f, 1.2f, 0.35f, 0x2FB36B);
  } else if (phx == PHASE_KID) {  // a red cap, brim to the side
    rect(4, -0.6f, 10, 0.6f, 0xE5484D), rect(12, -0.1f, 4, 0.25f, 0xB83A3E);
  } else if (phx == PHASE_TEEN) {  // a skateboard under his feet
    rect(1.5f, 5.0f, 15, 0.3f, 0x8A5A3C), rect(3.5f, 5.3f, 1, 0.35f, 0x2B2D33), rect(13.5f, 5.3f, 1, 0.35f, 0x2B2D33);
  } else if (phx == PHASE_ELDER && !sleeping) {  // white brows and a cane
    rect(4, 0.75f, 2, 0.2f, 0xF1EFE9), rect(11, 0.75f, 2, 0.2f, 0xF1EFE9);
    rect(17.0f, 1.0f, 0.6f, 4.0f, 0x8A5A3C), rect(16.2f, 0.8f, 1.4f, 0.35f, 0x8A5A3C);
  } else if (phx == PHASE_LEGEND) {  // a golden halo and sparkles
    rect(6, -1.0f, 6, 0.2f, 0xFFD166), rect(5.4f, -0.8f, 0.6f, 0.2f, 0xFFD166), rect(12, -0.8f, 0.6f, 0.2f, 0xFFD166);
    for (int k = 0; k < 4; k++)
      if ((now / 250 + k * 3) % 7 == 0) rect(k % 2 ? 0.5f + k * 4 : 15.0f - k, -1.5f + k * 1.4f, 0.5f, 0.3f, 0xFFF3B0);
  }
  if (look.stage == PET_ADULT && phx == PHASE_ADULT && !sleeping) {
    if (look.style == STYLE_OWL) {  // round-ish glasses: two frames round the eyes and a bridge
      for (int c : {4, 11}) {
        fill(b, bw, bh, x0 + c * pw - u, y0 + ph - u, 3 * pw + 2 * u, u, lv_color_hex(0xF1EFE9));
        fill(b, bw, bh, x0 + c * pw - u, y0 + 2 * ph, 3 * pw + 2 * u, u, lv_color_hex(0xF1EFE9));
        fill(b, bw, bh, x0 + c * pw - u, y0 + ph, u, ph, lv_color_hex(0xF1EFE9));
        fill(b, bw, bh, x0 + (c + 3) * pw, y0 + ph, u, ph, lv_color_hex(0xF1EFE9));
      }
      fill(b, bw, bh, x0 + 7 * pw, y0 + ph + ph / 3, 4 * pw, u, lv_color_hex(0xF1EFE9));
    } else if (look.style == STYLE_SHIPPER) {  // a crown on top
      rect(6, -0.45f, 6, 0.4f, 0xFFD166);
      for (float c : {6.0f, 8.5f, 11.0f}) rect(c, -1.0f, 1, 0.6f, 0xFFD166);
      rect(8.5f, -0.35f, 1, 0.2f, 0xFF5A4E);
    } else if (look.style == STYLE_DJ) {  // headphones: a band over the head, a cup on each side
      rect(3, -0.5f, 12, 0.25f, 0x3A3D45);
      rect(2, -0.5f, 1, 0.6f, 0x3A3D45), rect(15, -0.5f, 1, 0.6f, 0x3A3D45);
      rect(1, 0.1f, 2, 1.4f, 0x4CC3FF), rect(15, 0.1f, 2, 1.4f, 0x4CC3FF);
    }
  }
  int floor_y = base_y + ROWS * ph;  // where the feet stand
  // bugs from failed PR checks, crawling by his feet until squashed
  for (int i = 0; i < look.bugs; i++) {
    static const float SPOT[3] = {15.2f, 1.0f, 13.4f};  // by his feet, inside the canvas (it is as wide as he is)
    int bx = x0 + (int)(SPOT[i] * pw) + (int)(sinf(now / 300.0f + i * 2) * pw / 2);
    if (look.reaction == PET_REACT_BUG && i == look.bugs - 1 && look.reaction_ms < 1500)  // the new one crawls in
      bx = bw - (int)((bw - bx) * look.reaction_ms / 1500.0f);
    fill(b, bw, bh, bx, floor_y - ph / 2, max(2, pw * 3 / 2), max(2, ph / 3), lv_color_hex(0x3E8E41));
    fill(b, bw, bh, bx - u, floor_y - ph / 4, max(3, pw * 3 / 2 + 2 * u), u, lv_color_hex(0x1F4D22));  // legs
  }
  if (sleeping && big) {  // § bed: a frame and a headboard, a blanket up to his chest; Zzz rising from the pillow side
    rect(-0.4f, 1.0f, 0.8f, 4.6f, 0x6B4A33);                 // headboard
    rect(-0.4f, 4.7f, 18.8f, 0.5f, 0x8A5A3C);                // frame
    rect(0.0f, 5.2f, 0.6f, 0.5f, 0x6B4A33), rect(17.4f, 5.2f, 0.6f, 0.5f, 0x6B4A33);  // legs
    rect(1.0f, 2.8f, 16.0f, 1.9f, 0x3F5BD9);                 // the blanket
    rect(1.0f, 2.8f, 16.0f, 0.3f, 0xDDE4FF);                 // its folded edge
    for (int k = 0; k < 4; k++) rect(2.5f + k * 4, 3.6f, 1.2f, 0.25f, 0x6F86F0);  // a pattern
  }
  if (sleeping && big) {
    for (int k = 0; k < 3; k++) {  // three Zs, each bigger, drifting up and fading out in turn
      float p = fmodf(now / 2400.0f + k / 3.0f, 1.0f), sz = 0.5f + 0.25f * k;
      float zc = 13.5f + p * 2.5f, zr = -0.5f - p * 3.0f;
      uint32_t col = p < 0.75f ? 0xC9C6BF : 0x6A6862;
      rect(zc, zr, sz * 1.6f, sz * 0.3f, col);
      rect(zc + sz * 0.6f, zr + sz * 0.3f, sz * 0.5f, sz * 0.3f, col);
      rect(zc, zr + sz * 0.6f, sz * 1.6f, sz * 0.3f, col);
    }
  }
  uint32_t ms = look.reaction_ms;
  switch (look.reaction) {
    case PET_REACT_EAT:  // a fruit drops into his mouth — a different one each meal
      if (ms < 700) {
        float y = -3.0f + 4.5f * (ms / 700.0f) * (ms / 700.0f);
        switch (((now - ms) / 97) % 6) {
          case 0: rect(8.3f, y, 1.4f, 0.75f, 0xE5484D), rect(8.9f, y - 0.3f, 0.5f, 0.3f, 0x3DDC84); break;            // apple
          case 1: rect(7.6f, y, 2.6f, 0.4f, 0xFFD166), rect(7.4f, y - 0.25f, 0.5f, 0.3f, 0xFFD166),
                  rect(10.0f, y - 0.25f, 0.5f, 0.3f, 0x8A5A3C); break;                                                // banana
          case 2: for (int k = 0; k < 3; k++) rect(8.0f + k * 0.6f, y + 0.1f, 0.5f, 0.35f, 0x8E5BD9);
                  rect(8.3f, y + 0.45f, 0.5f, 0.35f, 0x8E5BD9), rect(8.9f, y + 0.45f, 0.5f, 0.35f, 0x8E5BD9),
                  rect(8.6f, y - 0.25f, 0.3f, 0.3f, 0x3DDC84); break;                                                 // grapes
          case 3: rect(8.2f, y, 1.6f, 0.8f, 0xFF8C1A), rect(8.8f, y - 0.2f, 0.4f, 0.25f, 0x3DDC84); break;           // orange
          case 4: rect(7.8f, y + 0.3f, 0.8f, 0.45f, 0xC81E3A), rect(9.4f, y + 0.3f, 0.8f, 0.45f, 0xC81E3A),
                  rect(8.4f, y - 0.3f, 0.3f, 0.6f, 0x3DDC84), rect(9.2f, y - 0.3f, 0.3f, 0.6f, 0x3DDC84); break;       // cherries
          default: rect(8.3f, y, 1.4f, 0.6f, 0xE5484D), rect(8.6f, y + 0.6f, 0.8f, 0.3f, 0xE5484D),
                   rect(8.4f, y - 0.25f, 1.2f, 0.25f, 0x3DDC84), rect(8.7f, y + 0.2f, 0.2f, 0.15f, 0xFFD166); break;  // strawberry
        }
      }
      break;
    case PET_REACT_PLANE:  // a paper plane from the left, caught
      if (ms < 900) {
        float x = -6.0f + 9.0f * ms / 900.0f, y = -1.5f + 1.0f * ms / 900.0f;
        rect(x, y, 2.2f, 0.25f, 0xF1EFE9);
        rect(x + 0.4f, y + 0.25f, 1.4f, 0.25f, 0xC9C6BF);
        rect(x + 1.6f, y - 0.25f, 0.6f, 0.25f, 0xF1EFE9);
      }
      break;
    case PET_REACT_TYPING:  // a tiny laptop in front of him
      rect(6, 3.4f, 6, 1.0f, 0x2B2D33);
      rect(6.4f, 3.6f, 5.2f, 0.15f, (now / 120) % 2 ? 0x4CC3FF : 0x2B2D33);
      rect(5.5f, 4.4f, 7, 0.3f, 0x8A8F99);
      break;
    case PET_REACT_LOVE:  // stroked: hearts float up
      for (int k = 0; k < 3; k++) {
        int hm = (int)ms - k * 350;
        if (hm < 0 || hm > 1400) continue;
        float rise = hm / 1400.0f, c = (k == 1 ? 13.0f : 3.5f) + sinf(hm / 200.0f) * 0.4f, r = -0.6f - 2.2f * rise;
        uint32_t pink = 0xFF7A9A;
        rect(c, r, 0.5f, 0.3f, pink), rect(c + 0.7f, r, 0.5f, 0.3f, pink);   // the two lobes
        rect(c - 0.1f, r + 0.25f, 1.4f, 0.3f, pink), rect(c + 0.3f, r + 0.5f, 0.6f, 0.3f, pink);
      }
      break;
    case PET_REACT_HATCH:  // the shell's halves fly apart
      if (ms < 1200) {
        float k = ms / 1200.0f;
        rect(5 - 4 * k, -1.5f - 2 * k, 3, 0.6f, 0xF6EEDF);
        rect(10 + 4 * k, -1.2f - 1.5f * k, 3, 0.6f, 0xEBDCC3);
      }
      break;
    default: break;
  }
  if (big && look.hungry && look.stage != PET_EGG && look.reaction == PET_REACT_NONE) {  // a thought: an apple
    rect(14.4f, -0.45f, 0.4f, 0.2f, 0x5D5B57);
    rect(14.9f, -0.95f, 0.6f, 0.25f, 0x5D5B57);
    rect(15.4f, -2.1f, 2.4f, 1.0f, 0x2A2B30);
    rect(16.1f, -1.95f, 1.0f, 0.6f, 0xE5484D);
  }
}

// The egg (a new pet): cream with orange speckles, a wobble now and then; a shadow under it like Clawd's.
static void draw_egg(lv_color_t *b, int bw, int bh, int pw, int ph, int base_y, uint32_t now, lv_color_t bg, bool floor_shadow,
                     uint8_t stage_phase, bool warmed)
{
  fill(b, bw, bh, 0, 0, bw, bh, bg);
  // rests, then rocks left and right — more often the closer it is to hatching
  int every = stage_phase >= PHASE_EGG_CRACK ? 3 : stage_phase >= PHASE_EGG_WOBBLE ? 5 : 8;
  int phase = (now / 300) % every;
  int dx = phase == every - 2 ? -max(1, pw / 2) : phase == every - 1 ? max(1, pw / 2) : 0;
  int x0 = (bw - COLS * pw) / 2 + dx, y0 = base_y;
  if (floor_shadow) fill(b, bw, bh, bw / 2 - 3 * pw, y0 + ROWS * ph + ph / 4, 6 * pw, max(2, ph / 4), lv_color_mix(lv_color_hex(0x3A2018), bg, 150));
  lv_color_t top = lv_color_hex(0xF6EEDF), bottom = lv_color_hex(0xD9C4A0), dot = lv_color_hex(0xD97757);
  if (stage_phase >= PHASE_EGG_WARM)  // warming up: a peach blush, pulsing slowly
    top = lv_color_mix(lv_color_hex(0xFFC9A8), top, (uint8_t)(90 + 60 * sinf(now / 600.0f))), bottom = lv_color_mix(lv_color_hex(0xF0A07C), bottom, 90);
  if (warmed) top = lv_color_hex(0xFFE2B0), bottom = lv_color_hex(0xFFB27A);  // just warmed by a tap: it glows
  bool open_top = stage_phase == PHASE_EGG_HATCH;  // hatching: the top is off, two eyes peek out
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      if (EGG[r][c] != '#' || (open_top && r < 2)) continue;
      bool speck = (r == 1 && c == 8) || (r == 2 && c == 11) || (r == 3 && c == 6) || (r == 2 && c == 7);
      for (int j = 0; j < ph; j++)
        fill(b, bw, bh, x0 + c * pw, y0 + r * ph + j, pw, 1, speck ? dot : lv_color_mix(bottom, top, (uint8_t)(255 * (r * ph + j) / (ROWS * ph))));
    }
  lv_color_t crack = lv_color_hex(0x5A4636);
  if (stage_phase >= PHASE_EGG_CRACK && !open_top)  // a zigzag crack across the middle
    for (int k = 0; k < 8; k++) fill(b, bw, bh, x0 + (5 + k) * pw, y0 + 2 * ph + (k % 2 ? ph / 3 : 0), pw, max(1, ph / 6), crack);
  if (open_top) {  // the jagged rim, and Clawd's eyes inside, blinking
    for (int k = 0; k < 8; k++) fill(b, bw, bh, x0 + (5 + k) * pw, y0 + 2 * ph - (k % 2 ? ph / 3 : 0), pw, ph / 3, lv_color_hex(0xF6EEDF));
    fill(b, bw, bh, x0 + 6 * pw, y0 + 2 * ph - ph / 2, 6 * pw, ph / 2, lv_color_hex(0xF0A07C));  // the top of his head
    if ((now / 1800) % 6) fill(b, bw, bh, x0 + 7 * pw, y0 + 2 * ph - ph / 3, pw, ph / 4, crack), fill(b, bw, bh, x0 + 10 * pw, y0 + 2 * ph - ph / 3, pw, ph / 4, crack);
  }
}

// The grave: a stone with a cross, a flower by it; now and then his ghost drifts up from it.
static void draw_grave(lv_color_t *b, int bw, int bh, int pw, int ph, int base_y, uint32_t now, lv_color_t bg)
{
  fill(b, bw, bh, 0, 0, bw, bh, bg);
  int x0 = (bw - COLS * pw) / 2, y0 = base_y;
  auto rect = [&](float c, float r, float w, float h, uint32_t col) {
    fill(b, bw, bh, x0 + (int)(c * pw), y0 + (int)(r * ph), max(1, (int)(w * pw)), max(1, (int)(h * ph)), lv_color_hex(col));
  };
  rect(3, 5.0f, 12, 0.3f, 0x2E3B26);                     // grass
  rect(6, 0.6f, 6, 4.4f, 0x6E7480), rect(6.5f, 0.3f, 5, 0.4f, 0x6E7480);  // the stone, rounded top
  rect(6.6f, 0.6f, 0.6f, 4.4f, 0x8A909C);                // its lit edge
  rect(8.6f, 1.2f, 0.8f, 2.4f, 0x3A3F48), rect(7.7f, 1.8f, 2.6f, 0.5f, 0x3A3F48);  // the cross
  rect(13.2f, 3.8f, 0.3f, 1.2f, 0x3DDC84), rect(12.9f, 3.4f, 0.9f, 0.5f, 0xFF7A9A);  // a flower
  rect(4.0f, 4.2f, 0.3f, 0.8f, 0x3DDC84), rect(3.8f, 3.9f, 0.7f, 0.4f, 0xFFD166);
  uint32_t cyc = now % 9000;  // his ghost, every 9 s, rises and fades
  if (cyc < 2500) {
    float p = cyc / 2500.0f;
    uint32_t g = p < 0.7f ? 0xC9D6E5 : 0x5E6A78;
    rect(8.0f, -0.4f - p * 2.5f, 2.2f, 1.2f, g), rect(8.0f, 0.8f - p * 2.5f, 0.6f, 0.4f, g), rect(9.6f, 0.8f - p * 2.5f, 0.6f, 0.4f, g);
  }
}

void clawd_draw(lv_color_t *b, int bw, int bh, int pw, int ph, int base_y, const String &mood_in, uint32_t now, bool bang_ok,
                lv_color_t bg, bool floor_shadow, const PetLook *look)
{
  if (look && look->phase == PHASE_GRAVE) return draw_grave(b, bw, bh, pw, ph, base_y, now, bg);
  if (look && look->stage == PET_EGG)
    return draw_egg(b, bw, bh, pw, ph, base_y, now, bg, floor_shadow, look->phase,
                    look->reaction == PET_REACT_LOVE && look->reaction_ms < 600);
  if (look && look->scale < 0.99f) {  // he grows: smaller while young, standing on the same floor
    int npw = max(1, (int)lroundf(pw * look->scale)), nph = max(2, (int)lroundf(ph * look->scale));
    base_y += ROWS * (ph - nph);
    pw = npw, ph = nph;
  }
  bool ghost = look && look->stage == PET_DEAD, sick = look && look->sick && !ghost;
  PetReaction react = look ? look->reaction : PET_REACT_NONE;
  uint32_t rms = look ? look->reaction_ms : 0;
  // The pet's own state takes the stage over the agents' mood while it lasts: a reaction, sleep, death.
  String mood = mood_in;
  if (ghost) mood = "ghost";
  else if (react == PET_REACT_CHEER || react == PET_REACT_EAT && rms > 700) mood = "done";
  else if (react == PET_REACT_ALERT) mood = "attention";
  else if (react == PET_REACT_TYPING) mood = "working";
  else if (react == PET_REACT_LOVE) mood = "love";  // eyes closed, content (the hearts come from the overlay)
  else if (look && look->sleeping) mood = "sleep";
  // Woken up while every agent is done (the agents' "sleep"): he is up, so he plays, not lies in bed (user, 2026-11-16:
  // «розбудив … іконка досі ліжечко … очікував, що він … стрибати, ходити»).
  else if (look && mood == "sleep") mood = "play";
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
  } else if (mood == "love") {  // stroked: eyes shut happily, a little bounce
    line_eyes = true;
    set(1, 5, '#'), set(1, 12, '#');
    dy = -(int)(fabsf(sinf(t * 4)) * ph * 0.2f);
  } else if (mood == "ghost") {  // dead: he floats, pale, with x eyes (drawn below)
    dy = -(int)(ph * 0.4f + sinf(t * 1.5f) * ph * 0.25f);
    set(1, 5, '#'), set(1, 12, '#');
  } else if (mood == "walk") {  // the app walks him across the screen: legs step, a little bounce each step
    bool step = (now / 200) % 2;
    for (int c : {4, 6, 11, 13}) set(4, c, '.');
    for (int c : step ? std::initializer_list<int>{4, 6, 11, 13} : std::initializer_list<int>{3, 5, 12, 14}) set(4, c, '#');
    dy = step ? -max(1, (int)(ph * 0.15f)) : 0;
  } else if (mood == "play") {  // awake with nothing to do: a hop every ~2.6 s, eyes looking about between hops
    float phase = fmodf(t, 2.6f);
    if (phase < 0.42f) dy = -(int)(sinf(phase / 0.42f * M_PI) * ph * 1.1f);
    else dy = (int)(sinf(t * 2) * ph * 0.12f);
    bool left = fmodf(t, 5.2f) < 2.6f;
    if (phase > 0.6f && phase < 2.2f) set(1, 5, '#'), set(1, 12, '#'), set(1, left ? 4 : 6, '.'), set(1, left ? 11 : 13, '.');
  } else {  // idle: a slow bob and a blink every ~4 s
    dy = (int)(sinf(t * 2) * ph * 0.15f);
    if (fmodf(t, 4.0f) > 3.85f) set(1, 5, '#'), set(1, 12, '#');
  }

  // The body: a light gradient, warm peach at the top to a deeper terracotta at the feet.
  lv_color_t top = lv_color_hex(0xF0A07C), bottom = lv_color_hex(0xB8573A);
  if (sick) top = lv_color_mix(lv_color_hex(0x9CCB7A), top, 140), bottom = lv_color_mix(lv_color_hex(0x5E8F4A), bottom, 140);
  if (ghost) top = lv_color_hex(0xE4ECF5), bottom = lv_color_hex(0x8FA3BA);
  if (look && look->phase == PHASE_ELDER) top = lv_color_mix(lv_color_hex(0xD8CFC6), top, 90), bottom = lv_color_mix(lv_color_hex(0x9C8E82), bottom, 90);
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
  if (look) pet_overlay(b, bw, bh, pw, ph, x0, y0, base_y, now, bg, bang_ok, *look, mood);
}

static void draw(uint32_t now)
{
  PetLook look = pet_look();  // the pet: his stage, look and reactions (pet.cpp)
  clawd_draw(buf, CW, CH, PW, PH, BASE_Y, mood, now, true, lv_color_hex(C_BG), true, pet_born() ? &look : nullptr);
  lv_obj_invalidate(canvas);
  String words = pet_said();
  if (words.length()) {
    set_text(speech, words);
    lv_obj_clear_flag(speech, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(speech, LV_OBJ_FLAG_HIDDEN);
  }
  float t = now / 1000.0f;
  if (mood == "sleep" && !pet_born()) {  // the pet draws its own Zzz (pet_overlay)
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
  const char *name[2] = {L("week", "тиждень"), L("5 h", "5 год")};
  lv_obj_t *arcs[2] = {week_arc, session_arc};
  for (int k = 0; k < 2; k++) {
    bool known = pct[k] >= 0;
    uint32_t c = known ? limit_color(pct[k], calm[k]) : C_IDLE;
    lv_arc_set_value(arcs[k], max(0, pct[k]));
    lv_obj_set_style_arc_color(arcs[k], lv_color_hex(c), LV_PART_INDICATOR);
    set_text(limit_value[k], known ? String(pct[k]) + "%" : "--");
    lv_obj_set_style_text_color(limit_value[k], lv_color_hex(known ? c : C_IDLE), 0);
    String r = left_text(reset[k]);
    set_text(limit_name[k], known ? String(name[k]) + (r.length() ? " · " + r : "") : String(name[k]) + L(" · idle", " · тихо"));
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
  const char *names[3] = {L("needs you", "чекають"), L("working", "працюють"), L("done", "готово")};
  for (int k = 0; k < 3; k++) {
    count_value[k] = make_label(page, &deck_bold_30, C_INK, 14, 80);
    lv_obj_align(count_value[k], LV_ALIGN_CENTER, (k - 1) * 80, 14);
    count_name[k] = make_label(page, &deck_14, C_MUTED, 40, 84);
    lv_obj_align(count_name[k], LV_ALIGN_CENTER, (k - 1) * 80, 40);
    set_text(count_name[k], names[k]);
  }
  mac_label = make_label(page, &lv_font_montserrat_14, C_MUTED, 62, 250);    // the Mac's time and CPU
  power_label = make_label(page, &lv_font_montserrat_14, C_MUTED, 81, 260);  // ASCII + the battery / bolt symbols
  speech = make_label(page, &deck_bold_16, C_INK, -150, 230);  // over his head, inside the round top
  lv_obj_set_style_bg_color(speech, lv_color_hex(0x1C1D21), 0);
  lv_obj_set_style_bg_opa(speech, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(speech, 10, 0);
  lv_obj_set_style_pad_hor(speech, 10, 0);
  lv_obj_set_style_pad_ver(speech, 4, 0);
  lv_obj_set_width(speech, LV_SIZE_CONTENT);
  lv_obj_add_flag(speech, LV_OBJ_FLAG_HIDDEN);
  zz = lv_label_create(page);
  lv_obj_set_style_text_font(zz, &deck_bold_30, 0);
  lv_obj_set_style_text_color(zz, lv_color_hex(C_MUTED), 0);
  lv_label_set_text(zz, "z");
  lv_obj_add_flag(zz, LV_OBJ_FLAG_HIDDEN);
  last_input = millis();
}

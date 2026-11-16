// Small shared UX pieces for the round screen (user, 2026-11-16: «ідеї … UI/UX … анімацій … можна все і одразу»):
//   rim_*    a scroll indicator on the right edge of the glass, like watchOS: where you are in a long list
//   accel_*  the crown speeds up when turned fast: long lists (32 worktrees) no longer take 32 detents
#include "deck.h"

// ---- the rim scroll indicator ----
#define RIM_SPAN 70   // degrees of the right edge it uses (centred on 3 o'clock)
#define RIM_MIN 12    // the thumb is never shorter than this

lv_obj_t *rim_create(lv_obj_t *parent)
{
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_set_size(arc, 350, 350);
  lv_obj_center(arc);
  lv_arc_set_bg_angles(arc, 360 - RIM_SPAN / 2, RIM_SPAN / 2);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x26272C), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(C_DETAIL), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
  return arc;
}

// `pos` of `count` items, `visible` of them fit on screen at once: a short list shows no indicator.
void rim_set(lv_obj_t *arc, int pos, int count, int visible)
{
  if (!arc) return;
  if (count <= visible || count < 2) {
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  int thumb = max(RIM_MIN, RIM_SPAN * visible / count);
  int start = (360 - RIM_SPAN / 2) + (RIM_SPAN - thumb) * pos / (count - 1);
  lv_arc_set_angles(arc, start % 360, (start + thumb) % 360);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_HIDDEN);
}

// ---- the crown's acceleration ----
// Three detents within 150 ms count double, six or more triple. A slow, deliberate turn stays one item a detent, so
// precise picking is untouched.
int accel_step(Accel &a, int step)
{
  // Measured where the detents happen (ui.cpp crown_rate): the UI loop hands a frame's detents over one by one, and
  // gaps measured there tripled a 5-detent test turn into 13 rows (2026-11-16).
  int rate = crown_rate();
  int dir = step > 0 ? 1 : -1;
  int mult = dir != a.dir ? 1 : rate >= 6 ? 3 : rate >= 3 ? 2 : 1;
  a.at = millis();
  a.dir = dir;
  return step * mult;
}

// Small shared UX pieces for the round screen (user, 2026-11-16: «ідеї … UI/UX … анімацій … можна все і одразу»):
//   rim_*    a scroll indicator on the right edge of the glass, like watchOS: where you are in a long list
//   accel_*  the crown speeds up when turned fast: long lists (32 worktrees) no longer take 32 detents
//   bloom    an app opens as its colour blooming out of the place it was tapped, then fading off the app
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

// ---- the bloom: an app opens out of its icon ----
static lv_obj_t *bloom;

static void bloom_size(void *obj, int32_t v)
{
  lv_obj_set_size((lv_obj_t *)obj, v, v);
}
static void bloom_pos(void *obj, int32_t v)  // 0 → 1000: from the icon to the centre
{
  lv_obj_t *o = (lv_obj_t *)obj;
  int x = (int)(intptr_t)lv_obj_get_user_data(o);
  int fx = (int16_t)(x & 0xFFFF), fy = (int16_t)(x >> 16);
  lv_obj_align(o, LV_ALIGN_CENTER, fx * (1000 - v) / 1000, fy * (1000 - v) / 1000);
}
static void bloom_opa(void *obj, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)obj, v, 0); }

void bloom_open(int x, int y, uint32_t color)
{
  if (!bloom) {
    bloom = lv_obj_create(lv_layer_top());
    lv_obj_set_style_radius(bloom, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(bloom, 0, 0);
    lv_obj_clear_flag(bloom, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  lv_anim_del(bloom, NULL);
  lv_obj_set_user_data(bloom, (void *)(intptr_t)(((int32_t)(int16_t)y << 16) | (uint16_t)(int16_t)x));
  lv_obj_set_style_bg_color(bloom, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(bloom, LV_OPA_COVER, 0);
  lv_obj_set_size(bloom, 60, 60);
  lv_obj_align(bloom, LV_ALIGN_CENTER, x, y);
  lv_obj_clear_flag(bloom, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bloom);
  lv_anim_set_time(&a, 230);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, 60, 520);
  lv_anim_set_exec_cb(&a, bloom_size);
  lv_anim_start(&a);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_exec_cb(&a, bloom_pos);
  lv_anim_start(&a);
  // then the colour lifts off the app that opened under it
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, 260);
  lv_anim_set_delay(&a, 200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&a, bloom_opa);
  lv_anim_set_ready_cb(&a, [](lv_anim_t *) { lv_obj_add_flag(bloom, LV_OBJ_FLAG_HIDDEN); });
  lv_anim_start(&a);
}

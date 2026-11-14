// The banner: an agent turned needs-you or done while orca-deck is not on screen, or an app's own alert (the timer
// ran out). It slides in at the top for 3 s over whatever is open; a tap on it opens orca-deck on that agent (or the
// app), a tap elsewhere goes to the app under it.
#include "deck.h"

#define BANNER_MS 3000
#define BANNER_Y -96    // centre of the pill: at -118 the round edge clipped its corners (the screen is ~208 px
                        // wide there, the pill 250)
#define BANNER_H 58

static lv_obj_t *pill, *dot, *title, *text;
static String wt_id, agent_id;
static const App *target = nullptr;  // an app's alert: the tap opens it instead of an agent
static uint32_t hide_at = 0;

static void set_y(void *obj, int32_t y) { lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, y); }
static void set_opa(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); }

static void animate(int32_t y_from, int32_t y_to, lv_opa_t opa_from, lv_opa_t opa_to, uint32_t ms)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, y_from, y_to);
  lv_anim_set_exec_cb(&a, set_y);
  lv_anim_start(&a);
  lv_anim_set_values(&a, opa_from, opa_to);
  lv_anim_set_exec_cb(&a, set_opa);
  lv_anim_start(&a);
}

static void show(const String &name, const String &line, uint32_t c)
{
  lv_obj_set_style_bg_color(dot, lv_color_hex(c), 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(c), 0);
  lv_obj_set_style_text_color(text, lv_color_hex(c), 0);
  set_text(title, name);
  set_text(text, line);
  bool shown = !lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_HIDDEN);
  if (!shown) animate(BANNER_Y - 90, BANNER_Y, LV_OPA_TRANSP, LV_OPA_COVER, 220);
  hide_at = millis() + BANNER_MS;
}

void banner_show_app(const App *app, const String &line)
{
  if (hub_current() == app) return;  // the app on screen shows it itself
  buddy_hide();                      // an app's alert wakes the knob
  target = app;
  show(app->name, line, app->color);
}

void banner_show(const String &worktree, const String &agent, const String &name, const String &bucket)
{
  if (buddy_is_shown()) return;  // Clawd acts the state out himself
  wt_id = worktree;
  agent_id = agent;
  target = nullptr;
  show(name, bucket == "attention" ? "needs you" : "done", color_of(bucket));
}

void banner_hide()
{
  hide_at = 0;
  lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
}

bool banner_is_shown() { return pill && !lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN); }

bool banner_take(bool forced)
{
  if (!banner_is_shown()) return false;
  if (!forced) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (p.y > 180 + BANNER_Y + BANNER_H / 2 + 10) return false;  // below the pill: the app's tap
  }
  String wt = wt_id, agent = agent_id;
  const App *app = target;
  banner_hide();
  if (app) hub_open(app);
  else deck_open_agent(wt, agent);
  return true;
}

void banner_tick()
{
  if (!hide_at || millis() < hide_at) return;
  hide_at = 0;
  animate(BANNER_Y, BANNER_Y - 90, LV_OPA_COVER, LV_OPA_TRANSP, 260);
  lv_timer_t *t = lv_timer_create([](lv_timer_t *timer) {
    if (!hide_at) lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);  // unless a new alert came meanwhile
    lv_timer_del(timer);
  }, 280, NULL);
  (void)t;
}

void banner_create()
{
  pill = lv_obj_create(lv_layer_top());
  lv_obj_set_size(pill, 250, BANNER_H);
  lv_obj_align(pill, LV_ALIGN_CENTER, 0, BANNER_Y);
  lv_obj_set_style_radius(pill, BANNER_H / 2, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x16171A), 0);
  lv_obj_set_style_border_width(pill, 2, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_clear_flag(pill, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);

  dot = lv_obj_create(pill);
  lv_obj_set_size(dot, 14, 14);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 22, 0);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  title = lv_label_create(pill);
  lv_obj_set_style_text_font(title, &deck_bold_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_INK), 0);
  lv_obj_set_width(title, 180);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 46, -9);
  lv_label_set_text(title, "");

  text = lv_label_create(pill);
  lv_obj_set_style_text_font(text, &deck_14, 0);
  lv_obj_align(text, LV_ALIGN_LEFT_MID, 46, 11);
  lv_label_set_text(text, "");
}

// Decision Wheel, an app in the hub (idea 17 in .work/notes/knob-app-ideas.md): pick lunch, a break, yes or no by
// spinning. A flick of the crown spins the wheel and friction slows it; the haptic ticks at every segment slow down
// with it, and the stop lands on a strong click with the winner in the middle.
//   crown: spin (each detent adds speed) · tap: spin / dismiss the winner · swipe left / right: the list
#include "deck.h"
#include <math.h>

#define MAX_SEG 8
#define WHEEL 300        // diameter
#define RING 70          // segment width (the ring from r = 80 to 150)
#define KICK 55.0f       // deg/s a detent adds
#define FRICTION 0.55f   // exponential slow-down, 1/s
#define DRAG 18.0f       // plus a constant drag, deg/s², so it really stops
#define STOP 5.0f        // deg/s: below this it has stopped

struct List {
  const char *name;
  const char *items[MAX_SEG];
};
static const List LISTS[] = {
    {"Lunch", {"Pizza", "Sushi", "Burger", "Borscht", "Shawarma", "Salad", nullptr}},
    {"Break", {"Walk", "Coffee", "Stretch", "Water", "Music", "Eyes off", nullptr}},
    {"Yes / No", {"Yes", "No", "Yes", "No", nullptr}},
    {"Dice", {"1", "2", "3", "4", "5", "6", nullptr}},
};
static const int LIST_COUNT = sizeof(LISTS) / sizeof(LISTS[0]);
static const uint32_t COLORS[MAX_SEG] = {0x4CC3FF, 0xFF7A1A, 0x3DDC84, 0xB18CFF, 0xFFD166, 0xFF7A9A, 0x7FD1B9, 0xD97757};

static lv_obj_t *page, *segs[MAX_SEG], *labels[MAX_SEG], *pointer, *hubcap, *list_label, *winner_label, *hint_label;
static int list = 0, count = 0;
static float angle = 0, omega = 0;  // degrees, deg/s
static bool spinning = false, showing_winner = false;
static int last_seg = -1;
static uint32_t last_frame = 0;

static int count_of(int l)
{
  int n = 0;
  while (n < MAX_SEG && LISTS[l].items[n]) n++;
  return n;
}

// The segment under the pointer (12 o'clock).
static int under_pointer()
{
  float seg = 360.0f / count;
  float a = fmodf(360 - fmodf(angle, 360) + 360, 360);  // the wheel turned by `angle`: the pointer reads back
  return (int)(a / seg) % count;
}

static void layout()
{
  float seg = 360.0f / count;
  for (int i = 0; i < MAX_SEG; i++) {
    if (i >= count) {
      lv_obj_add_flag(segs[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(segs[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
    // LVGL angles start at 3 o'clock and go clockwise; the wheel's 0 is 12 o'clock.
    int start = (int)lroundf(fmodf(270 + angle + i * seg, 360) + 360) % 360;
    lv_arc_set_bg_angles(segs[i], start, (start + (int)lroundf(seg) - 2 + 360) % 360);  // a 2° gap between
    float mid = (270 + angle + (i + 0.5f) * seg) * M_PI / 180;
    lv_obj_align(labels[i], LV_ALIGN_CENTER, (int)(115 * cosf(mid)), (int)(115 * sinf(mid)));
  }
}

static void paint_list()
{
  count = count_of(list);
  for (int i = 0; i < count; i++) {
    lv_obj_set_style_arc_color(segs[i], lv_color_hex(COLORS[i % MAX_SEG]), LV_PART_MAIN);
    set_text(labels[i], LISTS[list].items[i]);
  }
  set_text(list_label, LISTS[list].name);
  layout();
}

static void show_winner(bool on)
{
  showing_winner = on;
  if (on) {
    int w = under_pointer();
    set_text(winner_label, LISTS[list].items[w]);
    lv_obj_set_style_text_color(winner_label, lv_color_hex(COLORS[w % MAX_SEG]), 0);
  }
  on ? lv_obj_clear_flag(winner_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(winner_label, LV_OBJ_FLAG_HIDDEN);
  on ? lv_obj_add_flag(list_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(list_label, LV_OBJ_FLAG_HIDDEN);
  set_text(hint_label, on ? "tap: again" : "flick the crown · tap: spin");
}

static void kick(float speed)
{
  if (showing_winner) show_winner(false);
  omega += speed;
  omega = fmaxf(-1440, fminf(1440, omega));
  spinning = true;
  last_frame = millis();
}

static void turn(int step) { kick(step * KICK); }

static void tap()
{
  if (spinning) return;  // let it land
  if (showing_winner) {
    show_winner(false);
    haptic_click();
    return;
  }
  kick((random(0, 2) ? 1 : -1) * random(500, 900));  // a good random spin
  haptic_click();
}

static void swipe(lv_dir_t dir)
{
  if (spinning || (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)) return;
  list = (list + (dir == LV_DIR_LEFT ? 1 : LIST_COUNT - 1)) % LIST_COUNT;
  angle = 0;
  show_winner(false);
  paint_list();
  haptic_click();
}

// Every frame while spinning: friction, the segment ticks, the stop.
void wheel_tick()
{
  if (!spinning || !page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  uint32_t now = millis();
  if (now - last_frame < 25) return;
  float dt = (now - last_frame) / 1000.0f;
  last_frame = now;
  angle = fmodf(angle + omega * dt, 360);
  float slow = omega * expf(-FRICTION * dt);
  omega = slow > 0 ? fmaxf(0, slow - DRAG * dt) : fminf(0, slow + DRAG * dt);
  int seg = under_pointer();
  if (seg != last_seg) {  // a segment edge passed the pointer: the ticks slow down with the wheel
    last_seg = seg;
    haptic_tick();
  }
  if (fabsf(omega) < STOP) {
    omega = 0;
    spinning = false;
    haptic_click(2);
    show_winner(true);
  }
  layout();
}

static String subtitle() { return showing_winner ? String(LISTS[list].name) + ": " + LISTS[list].items[under_pointer()] : LISTS[list].name; }

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint_list();
}

static void hide()
{
  omega = 0;
  spinning = false;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

String wheel_report()
{
  return String("list=") + LISTS[list].name + " angle=" + String((int)angle) + " omega=" + String((int)omega) +
         (spinning ? " spinning" : "") + (showing_winner ? String(" winner=") + LISTS[list].items[under_pointer()] : "");
}

void wheel_create()
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

  for (int i = 0; i < MAX_SEG; i++) {
    lv_obj_t *a = segs[i] = lv_arc_create(page);
    lv_obj_set_size(a, WHEEL, WHEEL);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_style(a, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, RING, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    labels[i] = make_label(page, &deck_bold_16, C_BG, 0, 90);
  }
  hubcap = lv_obj_create(page);  // the centre disc over the ring's inside
  lv_obj_set_size(hubcap, WHEEL - 2 * RING - 4, WHEEL - 2 * RING - 4);
  lv_obj_center(hubcap);
  lv_obj_set_style_radius(hubcap, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hubcap, lv_color_hex(0x101114), 0);
  lv_obj_set_style_border_width(hubcap, 0, 0);
  lv_obj_clear_flag(hubcap, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  pointer = lv_label_create(page);  // the pointer at 12 o'clock, over the rim
  lv_obj_set_style_text_font(pointer, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(pointer, lv_color_hex(C_INK), 0);
  lv_label_set_text(pointer, LV_SYMBOL_DOWN);
  lv_obj_align(pointer, LV_ALIGN_TOP_MID, 0, 2);

  list_label = make_label(page, &deck_bold_16, C_MUTED, 0, 130);
  lv_obj_set_style_text_letter_space(list_label, 1, 0);
  winner_label = make_label(page, &deck_bold_30, C_INK, 0, 140);
  lv_obj_add_flag(winner_label, LV_OBJ_FLAG_HIDDEN);
  hint_label = make_label(page, &deck_14, C_MUTED, 150, 170);  // lower clips: the screen is ~140 px wide at 166
  paint_list();
  show_winner(false);
}

const App app_wheel = {"Wheel", LV_SYMBOL_SHUFFLE, 0xFFD166, show, hide, subtitle, turn, tap, nullptr, swipe};

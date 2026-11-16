// The Clawd app (.work/plans/clawd-pet.md, phase 4): the pet up close. Clawd big in the middle (the saver's drawing,
// with his stage, look and reactions), his three gauges as arcs round the top of the rim — food, joy, energy — and
// the actions along the bottom: the crown picks one, a tap does it. Stats shows his age and how he grew; a dead
// Clawd leaves a tombstone, and a tap lays a new egg.
// Catch (catch.cpp) is his own game; a hold strokes him (hearts, a purr of vibration, joy).
//   crown: pick an action · tap: do it · hold: stroke him · swipe down: the hub
#include "deck.h"

#define PW 10
#define PH 18
#define CW (18 * PW)
#define CH (9 * PH)
#define BASE_Y (3 * PH)

enum Act { ACT_FEED, ACT_CATCH, ACT_PLAY, ACT_SQUASH, ACT_HEAL, ACT_LIGHTS, ACT_STATS, ACT_SETUP, ACT_COUNT };
// Ukrainian on screen (user, 2026-11-16: «зробимо українською інтерфейс … це ж моя OS»)
static const char *const ACT_UA[ACT_COUNT] = {"Годувати", "Ловити", "Змійка", "Баги", "Лікувати", "Спати", "Про нього", "Опції"};
static const char *const ACT_EN[ACT_COUNT] = {"Feed", "Catch", "Snake", "Squash", "Heal", "Sleep", "About him", "Options"};
#define ACT_NAMES (settings.lang ? ACT_EN : ACT_UA)

static lv_obj_t *page, *canvas, *arcs[3], *gauge_labels[3], *state_label, *act_label, *prev_label, *next_label,
    *hint_label, *stats_box, *stats_label;
static lv_color_t *buf;
static int act = ACT_FEED;
static bool stats_open = false;
// Settings (user, 2026-11-16: «окремий екран з налаштуваннями … скинути до стану яйця»): vacation, start over, back.
// Starting over takes two taps within 4 s — one stray tap must not erase him.
enum SetupRow { SETUP_VACATION, SETUP_RESET, SETUP_BACK, SETUP_COUNT };
static bool setup_open = false;
static int setup_row = SETUP_VACATION;
static uint32_t reset_armed_at = 0;
static lv_obj_t *setup_box, *setup_title, *setup_rows[SETUP_COUNT], *setup_hint, *hatch_arc;
static bool egg_setup = false;  // an egg's carousel: «Гріти» or «Опції»
// Where the crown is in the carousel: a dot per action under it (the neighbours' names clipped at the round edge
// and ran into the action's own name, 2026-11-16).
static lv_obj_t *dots[ACT_COUNT];

static void paint_dots(int count, int at)
{
  const int gap = 14, x0 = -(count - 1) * gap / 2;
  for (int i = 0; i < ACT_COUNT; i++) {
    if (i >= count) {
      lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(dots[i], LV_ALIGN_CENTER, x0 + i * gap, 130);
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == at ? 0xD97757 : 0x3A3B40), 0);
  }
}
static uint32_t last_frame = 0, last_paint = 0;

static const uint32_t GAUGE_COLORS[3] = {0xD97757, 0x3DDC84, 0x4CC3FF};
static const char *const GAUGE_UA[3] = {"ситість", "радість", "енергія"}, *const GAUGE_EN[3] = {"food", "joy", "energy"};
#define GAUGE_NAMES (settings.lang ? GAUGE_EN : GAUGE_UA)
// LVGL angles: 0 at 3 o'clock, clockwise. The gauges share the top of the rim; the actions own the bottom.
static const int GAUGE_FROM[3] = {196, 248, 300}, GAUGE_SPAN = 44;

static String age_text(uint32_t s)
{
  if (s < 3600) return String(s / 60) + L(" min", " хв");
  if (s < 86400) return String(s / 3600) + L(" h ", " год ") + String(s % 3600 / 60) + L(" min", " хв");
  return String(s / 86400) + L(" d ", " д ") + String(s % 86400 / 3600) + L(" h", " год");
}

// The neediest thing, in a few words: the hub's subtitle and the line under Clawd.
static String need_text()
{
  if (!pet_born()) return L("waiting for the clock", "чекаю на годинник");
  PetLook l = pet_look();
  if (l.stage == PET_DEAD) return L("at rest · lived ", "спочив · прожив ") + age_text(pet_age_s());
  if (l.stage == PET_EGG) {
    uint32_t left = pet_hatch_left_s();
    return String(pet_phase_name(l.phase)) + L(" · ", " · ще ") + (left >= 3600 ? String(left / 3600) + L(" h ", " год ") : "") + String(left % 3600 / 60) + L(" min to go", " хв");
  }
  if (l.sick) return L("sick · heal him", "хворий · полікуй");
  if (pet_food() < 25) return L("hungry", "голодний");
  if (l.bugs) return String(l.bugs) + (l.bugs == 1 ? L(" bug", " баг") : L(" bugs", " баги")) + L(" · squash", " · розчави");
  if (l.sleeping) return L("asleep", "спить");
  if (pet_energy() < 20) return L("tired", "втомився");
  if (pet_joy() < 25) return L("bored · play with him", "сумує · пограйся");
  return String(pet_phase_name(l.phase)) + (l.stage == PET_ADULT && l.style != STYLE_PLAIN ? String(" · ") + pet_style_name(l.style) : "") +
         " · " + age_text(pet_age_s());
}

static void paint_setup()
{
  bool armed = reset_armed_at && millis() - reset_armed_at < 4000;
  const String texts[SETUP_COUNT] = {String(L("Vacation: ", "Відпустка: ")) + (pet_vacation() ? L("on, time stands still", "так, час стоїть") : L("off", "ні")),
                                     armed ? L("Tap again — from scratch", "Тапни ще раз — з нуля") : L("Start from an egg", "Почати з яйця"),
                                     L("Back", "Назад")};
  for (int i = 0; i < SETUP_COUNT; i++) {
    set_text(setup_rows[i], texts[i]);
    bool on = i == setup_row;
    lv_obj_set_style_text_color(setup_rows[i], lv_color_hex(on ? (i == SETUP_RESET && armed ? 0xFF4B4B : C_INK) : C_MUTED), 0);
    lv_obj_set_style_bg_opa(setup_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  }
  set_text(setup_hint, setup_row == SETUP_RESET ? L("erases age, counts and streak", "стирає вік, лічильники і серію") : L("crown: pick · tap: do", "колесо: вибір · тап: дія"));
}

static void open_setup(bool on)
{
  setup_open = on;
  reset_armed_at = 0;
  setup_row = SETUP_VACATION;
  on ? lv_obj_clear_flag(setup_box, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(setup_box, LV_OBJ_FLAG_HIDDEN);
  if (on) lv_obj_move_foreground(setup_box), paint_setup();
}

static void paint()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  if (setup_open) paint_setup();
  PetLook l = pet_look();
  bool dead = l.stage == PET_DEAD;
  int vals[3] = {pet_food(), pet_joy(), pet_energy()};
  bool egg = l.stage == PET_EGG;  // an egg has no gauges: it is only hatching (user, 2026-11-16)
  for (int i = 0; i < 3; i++) {
    egg ? lv_obj_add_flag(arcs[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(arcs[i], LV_OBJ_FLAG_HIDDEN);
    egg ? lv_obj_add_flag(gauge_labels[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(gauge_labels[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (egg) {
    lv_obj_clear_flag(hatch_arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(hatch_arc, 1000 - (int)(pet_hatch_left_s() * 1000UL / (24 * 3600UL)));
  } else {
    lv_obj_add_flag(hatch_arc, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = 0; i < 3; i++) {
    lv_arc_set_value(arcs[i], dead ? 0 : vals[i]);
    bool low = vals[i] < 25 && !dead && l.stage != PET_EGG;
    lv_obj_set_style_arc_color(arcs[i], lv_color_hex(low ? 0xFF4B4B : GAUGE_COLORS[i]), LV_PART_INDICATOR);
    set_text(gauge_labels[i], String(GAUGE_NAMES[i]) + " " + (dead ? 0 : vals[i]));
  }
  String words = pet_said();  // what he says, for a few seconds; else what he needs
  set_text(state_label, words.length() ? "«" + words + "»" : need_text());
  lv_obj_set_style_text_color(state_label, lv_color_hex(words.length() ? C_INK : C_DETAIL), 0);
  if (l.stage == PET_EGG) {  // an egg: keep it warm — or its settings
    set_text(act_label, egg_setup ? L("Options", "Опції") : L("Warm", "Гріти"));
    set_text(prev_label, ""), set_text(next_label, "");
    paint_dots(2, egg_setup ? 1 : 0);
    set_text(hint_label, egg_setup ? L("tap: open", "тап: відкрити") : L("tap: warm", "тап: гріти"));
  } else if (dead) {
    set_text(act_label, L("a new egg", "нове яйце"));
    set_text(prev_label, ""), set_text(next_label, "");
    paint_dots(0, 0);
    set_text(hint_label, L("tap", "тап"));
  } else {
    set_text(act_label, act == ACT_LIGHTS && l.sleeping ? L("Wake", "Розбудити") : ACT_NAMES[act]);
    set_text(prev_label, ""), set_text(next_label, "");
    paint_dots(ACT_COUNT, act);
    set_text(hint_label, L("tap · hold: pet him", "тап · утримай: погладь"));
  }
  if (stats_open) {
    set_text(stats_label, String("Clawd · ") + pet_phase_name(l.phase) +
                              (l.stage == PET_ADULT ? String(" · ") + pet_style_name(l.style) : "") + L("\nage ", "\nвік ") + age_text(pet_age_s()) +
                              "\n" + pet_counts() + "\n" + (pet_vacation() ? L("vacation: time stands still", "відпустка: час стоїть") : L("vacation: off", "відпустка: вимкнена")) +
                              L("\n\ntap: close", "\n\nтап: закрити"));
  }
}

static void draw(uint32_t now)
{
  PetLook l = pet_look();
  clawd_draw(buf, CW, CH, PW, PH, BASE_Y, claude_mood(), now, true, lv_color_hex(C_BG), true, pet_born() ? &l : nullptr);
  lv_obj_invalidate(canvas);
}

static void turn(int step)
{
  if (catch_active()) return catch_turn(step);
  if (setup_open) {
    int next = max(0, min(SETUP_COUNT - 1, setup_row + (step > 0 ? 1 : -1)));
    if (next == setup_row) return;
    setup_row = next, reset_armed_at = 0;
    haptic_tick();
    paint_setup();
    return;
  }
  if (pet_look().stage == PET_EGG && !stats_open) {
    bool next = step > 0;
    if (next == egg_setup) return;
    egg_setup = next;
    haptic_tick();
    paint();
    return;
  }
  if (stats_open || pet_look().stage == PET_DEAD) return;
  int next = max(0, min(ACT_COUNT - 1, act + (step > 0 ? 1 : -1)));
  if (next == act) return;  // the end: no tick, the crown says there is nothing more
  act = next;
  haptic_tick();
  paint();
}

static void tap()
{
  if (catch_active()) {
    catch_tap();
    paint();
    return;
  }
  if (setup_open) {
    if (setup_row == SETUP_VACATION) pet_action(PET_VACATION), haptic_click();
    else if (setup_row == SETUP_BACK) open_setup(false), haptic_click();
    else if (reset_armed_at && millis() - reset_armed_at < 4000) {  // the second tap: a fresh egg
      pet_action(PET_RESET);
      haptic_click(3);
      egg_setup = false;
      open_setup(false);
    } else {
      reset_armed_at = millis();
      haptic_buzz(1);  // a warning feel: the next tap erases him
    }
    paint();
    return;
  }
  if (stats_open) {
    stats_open = false;
    lv_obj_add_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
    haptic_click();
    return;
  }
  if (pet_look().stage == PET_EGG) {
    if (egg_setup) {
      haptic_click();
      open_setup(true);
      return;
    }
    pet_action(PET_WARM) ? haptic_click() : haptic_buzz(1);
    paint();
    return;
  }
  if (pet_look().stage == PET_DEAD) {
    pet_action(PET_NEW_EGG);
    haptic_click(2);
    paint();
    return;
  }
  bool ok = true;
  switch (act) {
    case ACT_FEED: ok = pet_action(PET_FEED); break;
    case ACT_CATCH:
      ok = pet_look().stage != PET_EGG;
      if (ok) {
        haptic_click();
        catch_start();
        return;
      }
      break;
    case ACT_PLAY:
      ok = pet_action(PET_PLAY);
      if (ok) {
        haptic_click();
        hub_open(&app_snake);  // a game together: every apple there is joy for him
        return;
      }
      break;
    case ACT_SQUASH: ok = pet_action(PET_SQUASH); break;
    case ACT_HEAL: ok = pet_action(PET_HEAL); break;
    case ACT_LIGHTS: ok = pet_action(PET_LIGHTS); break;
    case ACT_SETUP:
      haptic_click();
      open_setup(true);
      return;
    case ACT_STATS:
      stats_open = true;
      lv_obj_clear_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(stats_box);
      break;
  }
  ok ? haptic_click() : haptic_buzz(1);  // nothing to do (no bugs, not sick, an egg): a short no
  paint();
}

// Hold: he is stroked — hearts float up, the knob purrs (three soft ticks).
static void hold()
{
  if (catch_active() || stats_open || setup_open) return;
  if (pet_look().stage == PET_EGG) return tap();  // holding the egg warms it too
  if (!pet_action(PET_PET)) return haptic_buzz(1);  // an egg or a tombstone does not purr
  for (int i = 0; i < 3; i++) haptic_tick();
}

void pet_app_tick()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  catch_tick();
  uint32_t now = millis();
  if (now - last_frame >= 80) last_frame = now, draw(now);  // pixel art: ~12 fps
  if (now - last_paint >= 1000) last_paint = now, paint();
}

static String subtitle() { return need_text(); }

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  draw(millis());
  paint();
}

static void hide()
{
  catch_abort();
  if (setup_open) open_setup(false);
  stats_open = false;
  lv_obj_add_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

String pet_app_report() { return String("act=") + ACT_NAMES[act] + (stats_open ? " stats" : "") + " · catch " + catch_report() + " · " + need_text(); }

void pet_app_create()
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

  for (int i = 0; i < 3; i++) {  // the gauges round the top of the rim
    lv_obj_t *a = arcs[i] = lv_arc_create(page);
    lv_obj_set_size(a, 344, 344);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(a, GAUGE_FROM[i], GAUGE_FROM[i] + GAUGE_SPAN);
    lv_arc_set_range(a, 0, 100);
    lv_obj_set_style_arc_width(a, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x1C1D21), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    // the gauge's name and number just inside its arc
    float mid = (GAUGE_FROM[i] + GAUGE_SPAN / 2.0f) * M_PI / 180;
    gauge_labels[i] = make_label(page, &deck_14, GAUGE_COLORS[i], 0, 90);
    lv_obj_align(gauge_labels[i], LV_ALIGN_CENTER, (int)(116 * cosf(mid)), (int)(116 * sinf(mid)));  // inside: the rim clips
  }

  buf = (lv_color_t *)ps_malloc(CW * CH * sizeof(lv_color_t));
  if (buf) {
    canvas = lv_canvas_create(page);
    lv_canvas_set_buffer(canvas, buf, CW, CH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -26);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  }
  for (int i = 0; i < 3; i++) lv_obj_move_foreground(gauge_labels[i]);  // over the canvas's black, which covers them
  state_label = make_label(page, &deck_16, C_DETAIL, 66, 250);
  // the actions: the picked one big in the middle, its neighbours small at the sides
  act_label = make_label(page, &deck_bold_30, C_INK, 108, 230);
  prev_label = make_label(page, &deck_14, C_MUTED, 100, 70);
  lv_obj_align(prev_label, LV_ALIGN_CENTER, -112, 112);
  next_label = make_label(page, &deck_14, C_MUTED, 100, 70);
  lv_obj_align(next_label, LV_ALIGN_CENTER, 112, 112);
  hint_label = make_label(page, &deck_14, C_MUTED, 150, 200);
  for (int i = 0; i < ACT_COUNT; i++) {
    lv_obj_t *d = dots[i] = lv_obj_create(page);
    lv_obj_set_size(d, 7, 7);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }

  hatch_arc = lv_arc_create(page);  // an egg: how far to hatching, round the whole rim
  lv_obj_set_size(hatch_arc, 344, 344);
  lv_obj_center(hatch_arc);
  lv_obj_remove_style(hatch_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(hatch_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_bg_angles(hatch_arc, 0, 360);
  lv_arc_set_rotation(hatch_arc, 270);
  lv_arc_set_range(hatch_arc, 0, 1000);
  lv_obj_set_style_arc_width(hatch_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(hatch_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(hatch_arc, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_color(hatch_arc, lv_color_hex(0xFFC9A8), LV_PART_INDICATOR);
  lv_obj_add_flag(hatch_arc, LV_OBJ_FLAG_HIDDEN);

  setup_box = lv_obj_create(page);  // Settings: over everything
  lv_obj_set_size(setup_box, 360, 360);
  lv_obj_center(setup_box);
  lv_obj_set_style_bg_color(setup_box, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(setup_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(setup_box, 0, 0);
  lv_obj_set_style_radius(setup_box, 0, 0);
  lv_obj_clear_flag(setup_box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(setup_box, LV_OBJ_FLAG_HIDDEN);
  setup_title = make_label(setup_box, &deck_bold_16, C_MUTED, -96, 260);
  set_text(setup_title, L("CLAWD SETTINGS", "НАЛАШТУВАННЯ CLAWD"));
  for (int i = 0; i < SETUP_COUNT; i++) {
    setup_rows[i] = make_label(setup_box, &deck_bold_16, C_MUTED, -50 + i * 54, 260);
    lv_obj_set_style_bg_color(setup_rows[i], lv_color_hex(0x1C1D21), 0);
    lv_obj_set_style_radius(setup_rows[i], 14, 0);
    lv_obj_set_style_pad_ver(setup_rows[i], 13, 0);  // ≥ 40 px: a finger's target (CLAUDE.md, user 2026-11-16)
    lv_obj_set_style_min_height(setup_rows[i], 44, 0);
  }
  setup_hint = make_label(setup_box, &deck_14, C_MUTED, 112, 240);

  stats_box = lv_obj_create(page);  // Stats: over everything, a tap closes it
  lv_obj_set_size(stats_box, 360, 360);
  lv_obj_center(stats_box);
  lv_obj_set_style_bg_color(stats_box, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(stats_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(stats_box, 0, 0);
  lv_obj_set_style_radius(stats_box, 0, 0);
  lv_obj_clear_flag(stats_box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
  stats_label = make_label(stats_box, &deck_16, C_INK, 0, 250);
  lv_label_set_long_mode(stats_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(stats_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(stats_label, 6, 0);
  lv_obj_center(stats_label);
}

const App app_pet = {"Clawd", LV_SYMBOL_HOME, 0xD97757, show, hide, subtitle, turn, tap, hold, nullptr};

// Prompt Palette, an app in the hub (idea 1 in .work/notes/knob-app-ideas.md): stock prompts on a barrel drum, typed
// into a Claude chat with one tap — "продовжуй", "запусти тести", "так" without the keyboard.
//   crown: the drum turns, one prompt a detent · tap: send it to the target (`say`) · hold: the next chat agent as
//   the target · swipe left / right: the next / previous group
// The target is the agent on screen in the Claude app, so the palette follows the crown there; after a send it moves
// on as Claude does (`ui_after_send`: the next agent waiting on you). Prompts: ~/.config/orca-deck/prompts.json
// (the bridge's defaults without it), via the relay's P line.
#include "deck.h"
#include <math.h>

#define ROWS 7            // drum rows drawn: the selected one and three each side
#define DRUM_R 104        // the drum's radius: rows sit on a cylinder seen from the side
#define ROW_ANGLE 0.42f   // radians between rows
#define DRUM_Y -14        // the drum's axis
#define EASE 16.0f        // 1/s: how fast the drum settles on a detent
#define SENT_MS 1200

static lv_obj_t *page, *band, *group_label, *rows[ROWS], *target_label, *hint_label, *sent_label;
static std::vector<PromptGroup> groups;
static uint32_t seen_version = 0;
static int group = 0, sel = 0, target_index = 0;
static float pos = 0;  // the drum's angle, in rows; eases to `sel`
static uint32_t last_frame = 0, sent_at = 0;
static String target_agent, sent_agent, sent_text;  // "" until a hold picks one: follows the Claude app
static bool strict = false;  // tests: target_agent or nothing — never a fallback to a real session

static const PromptGroup *current()
{
  return groups.empty() ? nullptr : &groups[group % groups.size()];
}

static bool target(DeckTarget &out)
{
  std::vector<DeckTarget> all = deck_chat_targets();
  if (all.empty()) return false;
  for (const DeckTarget &t : all) {
    if (t.agent == target_agent) {
      out = t;
      return true;
    }
  }
  if (strict) return false;
  out = all[0];  // the agent on screen in Claude
  return true;
}

static void paint_target()
{
  DeckTarget t;
  if (target(t)) {
    set_text(target_label, "→ " + t.title);  // "→", not LV_SYMBOL_RIGHT: the deck fonts have no symbol glyphs
    lv_obj_set_style_text_color(target_label, lv_color_hex(color_of(t.bucket)), 0);
  } else {
    set_text(target_label, "no Claude chat open");
    lv_obj_set_style_text_color(target_label, lv_color_hex(C_MUTED), 0);
  }
}

// The drum: each row on a cylinder, its height sin(angle), its light cos(angle); the row nearest the front is the
// selected one, bold and wrapped to two lines.
static void layout()
{
  const PromptGroup *g = current();
  int n = g ? g->prompts.size() : 0;
  int base = (int)floorf(pos + 0.5f);
  for (int k = 0; k < ROWS; k++) {
    int item = base + k - ROWS / 2;
    lv_obj_t *row = rows[k];
    if (item < 0 || item >= n) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    float angle = (item - pos) * ROW_ANGLE;
    float light = cosf(angle);
    if (light <= 0.05f) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    bool front = item == base;
    lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
    set_text(row, g->prompts[item]);
    lv_obj_set_style_text_font(row, front ? &deck_bold_16 : &deck_16, 0);
    lv_obj_set_style_text_color(row, lv_color_hex(front ? C_INK : C_DETAIL), 0);
    lv_label_set_long_mode(row, front ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(row, front ? 250 : (int)(180 + 60 * light));
    lv_obj_set_style_opa(row, (lv_opa_t)(255 * light * light), 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, DRUM_Y + (int)(DRUM_R * sinf(angle)));
  }
}

static void paint_group()
{
  const PromptGroup *g = current();
  if (!g) {
    set_text(group_label, "PROMPTS");
    set_text(rows[ROWS / 2], "waiting for the Mac…");
    return;
  }
  set_text(group_label, groups.size() > 1 ? g->name + "  " + String(group % groups.size() + 1) + "/" + String(groups.size()) : g->name);
  layout();
}

static void take_prompts()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool fresh = shared.prompts_version != seen_version;
  if (fresh) {
    seen_version = shared.prompts_version;
    groups = shared.prompts;
  }
  xSemaphoreGive(shared_mux);
  if (!fresh) return;
  if (groups.size()) group %= groups.size();
  const PromptGroup *g = current();
  int n = g ? g->prompts.size() : 0;
  sel = max(0, min(sel, n - 1));
  pos = sel;
  paint_group();
}

static void turn(int step)
{
  const PromptGroup *g = current();
  if (!g || sent_at) return;
  int next = max(0, min((int)g->prompts.size() - 1, sel + step));
  if (next == sel) return;  // the drum's end: no tick, it does not move
  sel = next;
  last_frame = millis();
  haptic_tick();
}

static void tap()
{
  const PromptGroup *g = current();
  DeckTarget t;
  if (!g || sent_at) return;
  if (!target(t)) {
    haptic_buzz();
    paint_target();
    return;
  }
  JsonDocument cmd;
  cmd["cmd"] = "say";
  cmd["id"] = t.agent;
  cmd["text"] = g->prompts[sel];
  push_command(cmd);
  haptic_click(2);
  sent_agent = t.agent;
  sent_text = g->prompts[sel];
  sent_at = millis();
  set_text(sent_label, "SENT → " + t.title);
  lv_obj_clear_flag(sent_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(band, lv_color_hex(0x10301E), 0);
}

static void hold()
{
  std::vector<DeckTarget> all = deck_chat_targets();
  if (all.size() < 2) {
    haptic_buzz();
    return;
  }
  DeckTarget now;
  target(now);
  size_t i = 0;
  while (i < all.size() && all[i].agent != now.agent) i++;
  target_agent = all[(i + 1) % all.size()].agent;
  haptic_click();
  paint_target();
}

static void swipe(lv_dir_t dir)
{
  if (groups.size() < 2 || sent_at || (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)) return;
  group = (group + (dir == LV_DIR_LEFT ? 1 : groups.size() - 1)) % groups.size();
  sel = 0;
  pos = 0;
  haptic_click();
  paint_group();
}

void prompts_tick()
{
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  take_prompts();
  uint32_t now = millis();
  if (sent_at && now - sent_at > SENT_MS) {  // SENT shown long enough: Claude moves on, the palette with it
    sent_at = 0;
    if (!strict) target_agent = "";
    lv_obj_add_flag(sent_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(band, lv_color_hex(0x15161A), 0);
    ui_after_send(sent_agent, sent_text);
    paint_target();
  }
  static uint32_t last_target = 0;
  if (now - last_target > 1000) {  // the target's colour follows its agent's state
    last_target = now;
    if (!sent_at) paint_target();
  }
  if (fabsf(pos - sel) < 0.002f) return;
  float dt = min(0.1f, (now - last_frame) / 1000.0f);
  last_frame = now;
  pos += (sel - pos) * fminf(1.0f, dt * EASE);
  if (fabsf(pos - sel) < 0.002f) pos = sel;
  layout();
}

static String subtitle()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  int n = 0;
  for (const PromptGroup &g : shared.prompts) n += g.prompts.size();
  xSemaphoreGive(shared_mux);
  return n ? String(n) + " prompts" : "stock prompts";
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  seen_version = 0;  // take the list again
  take_prompts();
  paint_group();
  paint_target();
}

static void hide()
{
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  if (sent_at) {  // home within SENT: the send went; Claude moves on now, the palette opens clean
    sent_at = 0;
    lv_obj_add_flag(sent_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(band, lv_color_hex(0x15161A), 0);
    ui_after_send(sent_agent, sent_text);
  }
  if (!strict) target_agent = "";
}

void prompts_test_target(const String &needle)
{
  strict = true;
  target_agent = "";
  for (const DeckTarget &t : deck_chat_targets()) {
    if (needle.length() && t.agent.indexOf(needle) >= 0) target_agent = t.agent;
  }
  if (!target_agent.length()) target_agent = "-";  // matches nothing: a tap refuses
}

String prompts_report()
{
  const PromptGroup *g = current();
  DeckTarget t;
  bool has = target(t);
  return String("groups=") + groups.size() + " group=" + (g ? g->name : "-") + " sel=" + sel + " pos=" + String(pos, 2) +
         " prompt=" + (g && sel < (int)g->prompts.size() ? g->prompts[sel] : "-") + " target=" + (has ? t.title + "/" + t.agent : "none") +
         (sent_at ? " SENT" : "");
}

void prompts_create()
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

  band = lv_obj_create(page);  // the drum's window: the selected row sits in it
  lv_obj_set_size(band, 280, 54);
  lv_obj_align(band, LV_ALIGN_CENTER, 0, DRUM_Y);
  lv_obj_set_style_radius(band, 27, 0);
  lv_obj_set_style_bg_color(band, lv_color_hex(0x15161A), 0);
  lv_obj_set_style_border_width(band, 0, 0);
  lv_obj_clear_flag(band, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  for (int k = 0; k < ROWS; k++) rows[k] = make_label(page, &deck_16, C_DETAIL, DRUM_Y, 240);
  group_label = make_label(page, &deck_bold_16, 0xFF7A9A, -142, 200);
  lv_obj_set_style_text_letter_space(group_label, 2, 0);
  target_label = make_label(page, &deck_bold_16, C_MUTED, 116, 220);
  hint_label = make_label(page, &deck_14, C_MUTED, 140, 170);  // ~140 px of screen at this height
  set_text(hint_label, "tap: send · hold: agent");
  sent_label = make_label(page, &deck_bold_16, C_DONE, 116, 220);
  lv_obj_set_style_bg_color(sent_label, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(sent_label, LV_OPA_COVER, 0);
  lv_obj_add_flag(sent_label, LV_OBJ_FLAG_HIDDEN);
  paint_group();
}

const App app_prompts = {"Prompts", LV_SYMBOL_LIST, 0xFF7A9A, show, hide, subtitle, turn, tap, hold, swipe};

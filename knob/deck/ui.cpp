// orca-deck, an app in the hub (hub.cpp): the main page and its pages, as Apex (watch/Apex/ContentView.swift):
// its settings (swipe right) · the main page · the agent page (swipe left). Swipe down goes to the hub.
// Main page: crown → workspace (Orca follows after 1 s of rest) · tap → Orca shows this agent (go + enter) · hold → dictate to the agent
// (dictate.cpp) · swipe up → next agent · double tap → send. Selection rules come from Apex's Selection (watch/ApexCore/.../Selection.swift).
#include "deck.h"
#include <map>
#include <math.h>
#include <time.h>
#include "bidi_switch_knob.h"
#include "sound.h"
#include "mbedtls/base64.h"

#define KNOB_PIN_A 8
#define KNOB_PIN_B 7

#define COMMIT_MS 1000       // the crown rests this long before Orca is told to switch
#define GO_PATIENCE_MS 4000  // how long the screen keeps the choice while Orca gets there
#define DOUBLE_TAP_MS 260    // a tap waits this long to see whether it becomes a double tap

#define MAX_DOTS 48
#define RING_SIZE 330  // the workspaces' arcs, inside the halo (356) — one arc a workspace, as PR Radar

// LVGL-task state.
static std::vector<Worktree> all_worktrees, worktrees;  // the bridge's list, and the visible one
static uint32_t seen_version = 0;
static int index_shown = 0;
static String selected_id, last_active_id;
static uint32_t commit_at = 0, chosen_until = 0, single_tap_at = 0;
static std::map<String, String> picked_agent;  // worktree id → agent picked by holding
static std::map<String, String> last_bucket;   // agent id → bucket, for the done / needs-you buzz
static volatile int pending_steps = 0;

static lv_obj_t *dots[MAX_DOTS];  // arcs since 2026-11-14 (user: PR Radar's arcs look much nicer than dots)
static bool dot_attention[MAX_DOTS];  // its lead agent needs you: the arc pulses
static int dot_count = 0;
static String dot_ids;
static lv_obj_t *comet;
static String motion_bucket;  // the bucket of the agent on screen, for the comet and the sonar
static lv_obj_t *halo, *summary, *state_label, *name_label, *meta_label, *detail_label, *counter_label, *status_label;
// Motion added 2026-11-14 (user: «анімації … щоб виглядало симпатичніше», all four picked):
static float dial_pos = 0;          // the dial's turn, in workspaces: eases to index_shown, so the arcs slide
static int dial_count = 0;
#define AURA_DISCS 3
static lv_obj_t *aura[AURA_DISCS];  // a soft glow in the state's colour behind the card, breathing
static lv_color_t aura_from, aura_now;
static uint32_t aura_target = C_IDLE, aura_since = 0;
static lv_obj_t *bloom, *check;     // a done: a green ring blooms out and a tick draws itself
static lv_point_t check_pts[3];
static lv_obj_t *fly;               // a sent text flies up and away

// ---- shared helpers ----

static int urgency(const String &bucket)
{
  if (bucket == "attention") return 3;
  if (bucket == "working") return 2;
  if (bucket == "done") return 1;
  return 0;
}

uint32_t color_of(const String &bucket)
{
  if (bucket == "attention") return C_ATTENTION;
  if (bucket == "working") return C_WORKING;
  if (bucket == "done") return C_DONE;
  return C_IDLE;
}

void set_text(lv_obj_t *label, const String &text)
{
  if (strcmp(lv_label_get_text(label), text.c_str()) != 0) lv_label_set_text(label, text.c_str());
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y, int width)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_label_set_text(label, "");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
  return label;
}

// Apex Centre.title / Centre.meta: the project is the big label, the worktree (branch) leads the small one.
String ui_title(const Worktree &w)
{
  return w.repo.length() && w.repo != w.name ? w.repo : w.name;
}

String ui_meta(const Worktree &w, const Agent *a)
{
  String meta;
  if (w.repo.length() && w.repo != w.name) meta = w.name;
  if (a) {
    meta += String(meta.length() ? " · " : "") + (a->chat ? "chat" : "terminal");
    if (a->kind.length()) meta += " · " + a->kind;
  }
  return meta;
}

static const Agent *lead_agent(const Worktree &w)
{
  const Agent *best = nullptr;
  for (const Agent &a : w.agents) {
    if (!best || urgency(a.bucket) > urgency(best->bucket)) best = &a;
  }
  return best;
}

static const Agent *shown_agent(const Worktree &w)
{
  auto it = picked_agent.find(w.id);
  if (it != picked_agent.end()) {
    for (const Agent &a : w.agents) {
      if (a.id == it->second) return &a;
    }
  }
  return lead_agent(w);
}

static int64_t now_ms()
{
  time_t now = time(nullptr);
  return now < 1700000000 ? 0 : (int64_t)now * 1000;
}

static String ago(int64_t since)
{
  int64_t now = now_ms();
  if (!since || !now) return "";
  int64_t s = (now - since) / 1000;
  if (s < 0) s = 0;
  if (s < 60) return String((int)s) + "s";
  if (s < 3600) return String((int)lround(s / 60.0)) + "m";
  if (s < 86400) return String((int)lround(s / 3600.0)) + "h";
  return String((int)lround(s / 86400.0)) + "d";
}

static String upper(String s)
{
  s.toUpperCase();
  return s;
}

// ---- Apex Selection rules ----

// Every agent is done, and the latest of them finished at least `seconds` ago.
static bool is_completed(const Worktree &w, int64_t seconds, int64_t now)
{
  if (w.agents.empty() || !now) return false;
  int64_t finished = 0;
  for (const Agent &a : w.agents) {
    if (a.bucket != "done") return false;
    finished = max(finished, a.since);
  }
  return now - finished >= seconds * 1000;
}

// Without the inactive ones (no agents) when hide_inactive, and without the completed ones; a fresh done stays,
// and `keep` (the worktree on screen) is never hidden from under the user.
static std::vector<Worktree> visible(const std::vector<Worktree> &all, const String &keep)
{
  int64_t after = settings.hide_completed == 1 ? 3600 : settings.hide_completed == 2 ? 86400 : 0;
  int64_t now = now_ms();
  std::vector<Worktree> out;
  for (const Worktree &w : all) {
    if (w.id != keep) {
      if (settings.hide_inactive && w.agents.empty()) continue;
      if (after && is_completed(w, after, now)) continue;
    }
    out.push_back(w);
  }
  return out;
}

static int index_of(const String &id)
{
  for (size_t i = 0; i < worktrees.size(); i++) {
    if (worktrees[i].id == id) return i;
  }
  return -1;
}

// After a send: the next agent waiting on the user, in knob order after `current` (the current worktree last),
// skipping the agent just sent to. A needs-you beats a done.
static int next_waiting(int current, const String &excluding, String &agent_out)
{
  int n = worktrees.size();
  for (const char *bucket : {"attention", "done"}) {
    for (int step = 1; step <= n; step++) {
      int i = (current + step) % n;
      for (const Agent &a : worktrees[i].agents) {
        if (a.bucket == bucket && a.id != excluding) {
          agent_out = a.id;
          return i;
        }
      }
    }
  }
  return -1;
}

// ---- rendering ----

static void build_dots()
{
  String ids;
  for (const Worktree &w : worktrees) ids += w.id + "|";
  if (ids == dot_ids) return;
  dot_ids = ids;
  int n = min((int)worktrees.size(), MAX_DOTS);
  for (int i = 0; i < MAX_DOTS; i++) {
    if (i < n && !dots[i]) {
      lv_obj_t *a = dots[i] = lv_arc_create(lv_scr_act());
      lv_obj_set_size(a, RING_SIZE, RING_SIZE);
      lv_obj_center(a);
      lv_obj_remove_style(a, NULL, LV_PART_KNOB);
      lv_obj_remove_style(a, NULL, LV_PART_INDICATOR);
      lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
      lv_obj_move_background(a);
    }
    if (dots[i]) {
      if (i < n) lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  dot_count = n;
}

// The dial turns so the chosen workspace sits at 12 o'clock; dial_pos slides there (motion()).
static void place_dots()
{
  // One arc a workspace, the chosen one centred at 12 o'clock; LVGL's 0° is 3 o'clock, clockwise.
  float span = dot_count ? 360.0f / dot_count : 360;
  int gap = dot_count > 1 ? 6 : 0;
  for (int i = 0; i < dot_count; i++) {
    if (dot_count == 1) {
      lv_arc_set_bg_angles(dots[i], 0, 360);
      continue;
    }
    float mid = 270 + (i - dial_pos) * span;
    int start = ((int)lroundf(mid - span / 2 + gap / 2.0f) % 360 + 360) % 360;
    int end = ((int)lroundf(mid + span / 2 - gap / 2.0f) % 360 + 360) % 360;
    lv_arc_set_bg_angles(dots[i], start, end);
  }
}

static void bloom_start();

static void render()
{
  int n = worktrees.size();
  int attention = 0, working = 0, done = 0;
  for (const Worktree &w : all_worktrees) {  // the summary counts every agent, as Apex
    for (const Agent &a : w.agents) {
      if (a.bucket == "attention") attention++;
      else if (a.bucket == "working") working++;
      else if (a.bucket == "done") done++;
    }
  }
  String sum;
  if (attention) sum += String(attention) + " WAITING";
  if (working) sum += String(sum.length() ? " · " : "") + working + " WORKING";
  if (done) sum += String(sum.length() ? " · " : "") + done + " DONE";
  set_text(summary, sum.length() ? sum : (n ? "ALL QUIET" : "NO WORKSPACES"));

  build_dots();
  if (dial_count != dot_count) {  // a new list: no slide from a stale place
    dial_count = dot_count;
    dial_pos = index_shown;
  }
  for (int i = 0; i < dot_count; i++) {
    const Worktree &w = worktrees[i];
    const Agent *lead = lead_agent(w);
    String bucket = lead ? lead->bucket : "idle";
    bool sel = i == index_shown;
    bool empty = w.agents.empty();
    lv_obj_set_style_arc_width(dots[i], sel ? 16 : empty ? 4 : 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dots[i], empty ? lv_color_hex(0x444444) : lv_color_hex(color_of(bucket)), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(dots[i], sel ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    dot_attention[i] = bucket == "attention";
  }
  place_dots();

  if (!n) {
    set_text(state_label, "");
    set_text(name_label, "Claude");
    set_text(meta_label, "");
    set_text(detail_label, "");
    set_text(counter_label, "");
    return;
  }

  const Worktree &w = worktrees[index_shown];
  const Agent *a = shown_agent(w);
  String bucket = a ? a->bucket : "idle";
  lv_color_t c = lv_color_hex(color_of(bucket));
  lv_obj_set_style_arc_color(halo, c, LV_PART_MAIN);
  lv_obj_set_style_arc_color(comet, c, LV_PART_INDICATOR);
  motion_bucket = bucket;
  if (color_of(bucket) != aura_target) {  // the glow cross-fades to the new state's colour
    aura_from = aura_now;
    aura_target = color_of(bucket);
    aura_since = millis();
  }
  lv_obj_set_style_text_color(state_label, c, 0);
  set_text(state_label, !a ? "NO AGENTS" : bucket == "attention" ? "NEEDS YOU" : upper(bucket));
  set_text(name_label, ui_title(w));
  set_text(meta_label, ui_meta(w, a));

  String detail;
  if (a) {
    if (bucket == "working" && a->tool.length()) {
      String tool = a->tool;
      int cut = tool.lastIndexOf("__");
      if (cut >= 0) tool = tool.substring(cut + 2);
      detail = "→ " + tool + (a->last.length() ? " — " + a->last : "");
    } else {
      detail = a->last.length() ? a->last : a->prompt;
    }
  }
  set_text(detail_label, detail);

  String counter = String(index_shown + 1) + "/" + n;
  if (w.agents.size() > 1 && a) {
    int at = 0;
    for (size_t k = 0; k < w.agents.size(); k++) {
      if (w.agents[k].id == a->id) at = k;
    }
    counter += " · agent " + String(at + 1) + "/" + String((int)w.agents.size());
  }
  if (a) {
    String age = ago(a->since);
    if (age.length()) counter += " · " + age;
  }
  set_text(counter_label, counter);
}

static void select_index(int i)
{
  if (worktrees.empty()) return;
  index_shown = max(0, min(i, (int)worktrees.size() - 1));
  selected_id = worktrees[index_shown].id;
}

static void on_state(std::vector<Worktree> &list)
{
  all_worktrees.swap(list);

  // Events: buzz on a fresh needs-you (2) or a working → done (1); remember a fresh done for `follows`.
  int pulses = 0;
  String fresh_done_worktree, fresh_done_agent;
  const Worktree *alert_w = nullptr;  // the agent for the banner: a needs-you beats a done
  const Agent *alert_a = nullptr;
  for (const Worktree &w : all_worktrees) {
    for (const Agent &a : w.agents) {
      auto it = last_bucket.find(a.id);
      if (it != last_bucket.end() && it->second != a.bucket) {
        if (a.bucket == "attention") {
          pulses = 2;
          alert_w = &w, alert_a = &a;
        } else if (a.bucket == "done" && it->second == "working" && !focus_is_on()) {  // Focus: a done stays quiet
          pulses = max(pulses, 1);
          if (!alert_a || alert_a->bucket != "attention") alert_w = &w, alert_a = &a;
          fresh_done_worktree = w.id;
          fresh_done_agent = a.id;
        }
      }
      last_bucket[a.id] = a.bucket;
    }
  }

  // Orca moved elsewhere (a click in Orca, the watch): the knob follows — unless the knob's own choice is
  // still on its way there.
  String active_id;
  for (const Worktree &w : all_worktrees) {
    if (w.active) active_id = w.id;
  }
  bool own_choice_pending = commit_at || millis() < chosen_until;
  if (active_id.length() && active_id != last_active_id && !own_choice_pending) selected_id = active_id;
  last_active_id = active_id;

  // Apex `follows`: a fresh done takes the knob to it only while the agent on screen is working.
  int previous = index_of(selected_id);
  if (fresh_done_agent.length() && previous >= 0 && !own_choice_pending) {
    const Agent *shown = shown_agent(worktrees[previous]);
    if (shown && shown->bucket == "working" && shown->id != fresh_done_agent) {
      selected_id = fresh_done_worktree;
      picked_agent[fresh_done_worktree] = fresh_done_agent;
    }
  }

  worktrees = visible(all_worktrees, selected_id);
  int i = index_of(selected_id);
  if (i < 0) i = selected_id.length() ? min(index_shown, (int)worktrees.size() - 1) : index_of(active_id);
  select_index(i < 0 ? 0 : i);
  render();
  if (fresh_done_agent.length() && hub_current() == &app_deck && !worktrees.empty()) {
    const Agent *shown = shown_agent(worktrees[index_shown]);
    if (shown && shown->id == fresh_done_agent) bloom_start();
  }
  if (pulses) haptic_buzz(pulses);
  if (alert_a && hub_current() != &app_deck) banner_show(alert_w->id, alert_a->id, ui_title(*alert_w), alert_a->bucket);
}

static void swap_card();

// Sends the knob's choice to Orca. The screen holds that choice while Orca gets there.
static void commit()
{
  commit_at = 0;
  if (worktrees.empty()) return;
  const Worktree &w = worktrees[index_shown];
  const Agent *a = shown_agent(w);
  chosen_until = millis() + GO_PATIENCE_MS;
  push_simple("go", w.id, a ? a->id : "");
}

static void turn(int step)
{
  if (worktrees.empty()) return;
  haptic_tick();
  int n = worktrees.size();
  select_index(((index_shown + step) % n + n) % n);
  commit_at = millis() + COMMIT_MS;
  render();
  swap_card();
}

// Hold: the next agent of the workspace on screen, shown in Orca right away.
static void next_agent()
{
  if (worktrees.empty()) return;
  const Worktree &w = worktrees[index_shown];
  if (w.agents.size() < 2) {
    haptic_buzz(1);
    return;
  }
  const Agent *a = shown_agent(w);
  size_t at = 0;
  for (size_t k = 0; k < w.agents.size(); k++) {
    if (a && w.agents[k].id == a->id) at = k;
  }
  picked_agent[w.id] = w.agents[(at + 1) % w.agents.size()].id;
  haptic_click();
  render();
  commit();
}

void ui_close_pages()
{
  agent_page_close();
  settings_page_show(false);
}

// Apex: after a send or an answer, back to the main page, turned to the next waiting agent (no `go`:
// the crown or a tap takes Orca there).
static void fly_y(void *obj, int32_t v) { lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, v); }
static void fly_opa(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); }
static void fly_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

// The text just sent rises off the screen and fades, as a message leaving (over any page: the top layer).
static void fly_away(const String &text)
{
  if (!fly || !text.length()) return;
  set_text(fly, text);
  lv_obj_set_style_opa(fly, LV_OPA_COVER, 0);
  lv_obj_clear_flag(fly, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, fly);
  lv_anim_set_values(&a, 24, -170);
  lv_anim_set_time(&a, 700);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&a, fly_y);
  lv_anim_start(&a);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_exec_cb(&a, fly_opa);
  lv_anim_set_ready_cb(&a, fly_done);
  lv_anim_start(&a);
}

void ui_after_send(const String &agent_id, const String &sent)
{
  fly_away(sent);
  ui_close_pages();
  String agent;
  int i = next_waiting(index_shown, agent_id, agent);
  if (i >= 0) {
    select_index(i);
    picked_agent[worktrees[i].id] = agent;
  }
  render();
}

std::vector<Agent> deck_all_agents()
{
  std::vector<Agent> out;
  for (const Worktree &w : all_worktrees) out.insert(out.end(), w.agents.begin(), w.agents.end());
  return out;
}

std::vector<DeckTarget> deck_chat_targets()
{
  std::vector<DeckTarget> out;
  if (index_shown < (int)worktrees.size()) {
    const Worktree &w = worktrees[index_shown];
    const Agent *a = shown_agent(w);
    if (a && a->chat) out.push_back({ui_title(w), a->id, a->bucket});
  }
  for (const Worktree &w : all_worktrees) {
    for (const Agent &a : w.agents) {
      if (a.chat && (out.empty() || out[0].agent != a.id)) out.push_back({ui_title(w), a.id, a.bucket});
    }
  }
  return out;
}

void deck_open_agent(const String &worktree, const String &agent)
{
  hub_open(&app_deck);
  ui_close_pages();
  selected_id = worktree;
  picked_agent[worktree] = agent;
  worktrees = visible(all_worktrees, selected_id);
  int i = index_of(worktree);
  if (i >= 0) select_index(i);
  render();
  swap_card();
  commit();  // Orca shows the agent too, as a crown choice would
}

// ---- motion (Apex: comet while working, sonar while it needs you, one calm swap per crown step) ----

static void bloom_step(void *, int32_t v)  // 0…1000: the ring grows from the card out past the rim and fades
{
  int size = 110 + (360 - 110) * v / 1000;
  lv_obj_set_size(bloom, size, size);
  lv_obj_align(bloom, LV_ALIGN_CENTER, 0, -20 * (1000 - v) / 1000);
  lv_obj_set_style_border_width(bloom, 1 + 7 * (1000 - v) / 1000, 0);
  lv_obj_set_style_border_opa(bloom, (lv_opa_t)(255 * (1000 - v) / 1000), 0);
}

static void check_step(void *, int32_t v)  // 0…1000: the short stroke, then the long one
{
  const lv_point_t a = {2, 15}, b = {12, 25}, c = {32, 3};
  check_pts[0] = a;
  if (v < 350) {
    check_pts[1] = {(lv_coord_t)(a.x + (b.x - a.x) * v / 350), (lv_coord_t)(a.y + (b.y - a.y) * v / 350)};
    lv_line_set_points(check, check_pts, 2);
  } else {
    check_pts[1] = b;
    check_pts[2] = {(lv_coord_t)(b.x + (c.x - b.x) * (v - 350) / 650), (lv_coord_t)(b.y + (c.y - b.y) * (v - 350) / 650)};
    lv_line_set_points(check, check_pts, 3);
  }
}

static void hide_when_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }
static void check_opa(void *obj, int32_t v) { lv_obj_set_style_line_opa((lv_obj_t *)obj, v, 0); }

static void bloom_start()
{
  lv_obj_clear_flag(bloom, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(check, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_line_opa(check, LV_OPA_COVER, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bloom);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, bloom_step);
  lv_anim_set_ready_cb(&a, hide_when_done);
  lv_anim_start(&a);
  lv_anim_init(&a);
  lv_anim_set_var(&a, check);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 420);
  lv_anim_set_delay(&a, 150);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, check_step);
  lv_anim_start(&a);
  lv_anim_init(&a);  // the tick stays a moment, then fades
  lv_anim_set_var(&a, check);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, 400);
  lv_anim_set_delay(&a, 1800);
  lv_anim_set_exec_cb(&a, check_opa);
  lv_anim_set_ready_cb(&a, hide_when_done);
  lv_anim_start(&a);
}

static void fade_in(lv_obj_t *obj)
{
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
  lv_anim_set_time(&a, 160);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); });
  lv_anim_start(&a);
}

static void swap_card()
{
  for (lv_obj_t *label : {state_label, name_label, meta_label, detail_label, counter_label}) fade_in(label);
}

// ~30 fps; everything stops while another page covers the main one.
static void motion(uint32_t now)
{
  bool main_page = hub_current() == &app_deck && !settings_page_open() && !agent_page_is_open() && !dictate_is_open();
  // The dots slide to the crown's choice every frame, the shortest way round, settling without overshoot.
  static uint32_t last_slide = 0;
  if (dot_count > 1) {
    float delta = fmodf(index_shown - dial_pos, dot_count);
    if (delta > dot_count / 2.0f) delta -= dot_count;
    if (delta < -dot_count / 2.0f) delta += dot_count;
    if (fabsf(delta) > 0.002f) {
      float dt = min(0.05f, (now - last_slide) / 1000.0f);
      dial_pos += fabsf(delta) < 0.004f ? delta : delta * fminf(1.0f, dt * 14);
      dial_pos = fmodf(dial_pos + dot_count, dot_count);
      if (main_page) place_dots();
    }
  }
  last_slide = now;

  // The aura, 15 fps, main page only (covered, it would make the page above redraw).
  static uint32_t last_aura = 0;
  if (main_page && now - last_aura >= 66) {
    last_aura = now;
    float t = fminf(1.0f, (now - aura_since) / 600.0f);
    aura_now = lv_color_mix(lv_color_hex(aura_target), aura_from, (uint8_t)(255 * t));
    float strength = motion_bucket == "attention" ? 1.0f : motion_bucket == "working" ? 0.8f : motion_bucket == "done" ? 0.6f : 0.25f;
    float period = motion_bucket == "attention" ? 1600.0f : 4200.0f;
    float breath = 0.7f + 0.3f * sinf(now / period * 2 * M_PI);
    for (int k = 0; k < AURA_DISCS; k++) {
      float drift = now / 9000.0f * 2 * M_PI + k * 2.1f;
      int r = 6 + 3 * k;
      lv_obj_align(aura[k], LV_ALIGN_CENTER, (int)(r * cosf(drift)), (int)(r * sinf(drift)) - 20);
      lv_obj_set_style_bg_color(aura[k], aura_now, 0);
      lv_obj_set_style_bg_opa(aura[k], (lv_opa_t)((10 + 6 * k) * strength * breath), 0);
    }
  }

  static uint32_t last = 0;
  if (now - last < 33) return;
  last = now;
  bool working = main_page && motion_bucket == "working";
  if (working) {
    lv_obj_clear_flag(comet, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_rotation(comet, (now / 8) % 360);  // one lap in ~2.9 s
  } else {
    lv_obj_add_flag(comet, LV_OBJ_FLAG_HIDDEN);
  }
  if (main_page && motion_bucket == "attention") {
    float phase = (now % 1100) / 1100.0f;  // the spike's halo pulse: 1.1 s
    lv_obj_set_style_arc_opa(halo, LV_OPA_30 + (lv_opa_t)(LV_OPA_70 * (0.5f + 0.5f * cosf(phase * 2 * M_PI))), LV_PART_MAIN);
    lv_obj_set_style_arc_width(halo, 3 + (int)(3 * (0.5f - 0.5f * cosf(phase * 2 * M_PI))), LV_PART_MAIN);
  } else {
    lv_obj_set_style_arc_opa(halo, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_width(halo, 3, LV_PART_MAIN);
  }
  if (main_page) {  // a workspace that needs you breathes on its arc, as a running PR does on PR Radar's
    float pulse = 0.5f + 0.5f * sinf(now / 1400.0f * 2 * M_PI);
    for (int i = 0; i < dot_count; i++) {
      if (!dot_attention[i]) continue;
      lv_opa_t base = i == index_shown ? 150 : 60;
      lv_obj_set_style_arc_opa(dots[i], (lv_opa_t)(base + (255 - base) * pulse), LV_PART_MAIN);
    }
  }
}

// ---- input ----

static void deck_tap()
{
  if (dictate_is_open()) return dictate_tap();
  if (settings_page_open()) return settings_page_tap();
  if (agent_page_is_open()) return agent_page_tap();
  if (single_tap_at) {
    single_tap_at = 0;  // the second tap of a double tap
    haptic_click(2);
    push_simple("send");
    return;
  }
  single_tap_at = millis() + DOUBLE_TAP_MS;
}

// Hold: dictate to the agent on screen (it was "next agent" until 2026-11-14; that moved to swipe up).
static void deck_hold()
{
  if (dictate_is_open() || settings_page_open() || agent_page_is_open() || worktrees.empty()) return;
  single_tap_at = 0;
  const Worktree &w = worktrees[index_shown];
  const Agent *a = shown_agent(w);
  if (!a) {
    haptic_buzz(1);
    return;
  }
  dictate_begin(ui_title(w), a->id);
}

static void deck_turn(int step)
{
  if (dictate_is_open()) return;
  if (settings_page_open()) settings_page_turn(step);
  else if (agent_page_is_open()) agent_page_turn(step);
  else turn(step);
}

static void deck_swipe(lv_dir_t dir)
{
  if (dictate_is_open()) return dictate_swipe();
  if (dir == LV_DIR_TOP) {
    if (!settings_page_open() && !agent_page_is_open()) next_agent();
    return;
  }
  single_tap_at = 0;
  if (settings_page_open()) {
    if (dir == LV_DIR_LEFT) settings_page_show(false), haptic_click();
    return;
  }
  if (agent_page_is_open()) {
    if (dir == LV_DIR_RIGHT) agent_page_close(), haptic_click();
    return;
  }
  if (dir == LV_DIR_RIGHT) {
    haptic_click();
    settings_page_show(true, true);
  } else if (dir == LV_DIR_LEFT && !worktrees.empty()) {
    const Worktree &w = worktrees[index_shown];
    const Agent *a = shown_agent(w);
    if (!a) {
      haptic_buzz(1);
      return;
    }
    haptic_click();
    agent_page_open(w, *a);
  }
}

static String duration(int minutes)
{
  if (minutes >= 1440) return String(minutes / 1440) + "d " + String(minutes % 1440 / 60) + "h";
  if (minutes >= 60) return String(minutes / 60) + "h " + String(minutes % 60) + "m";
  return String(minutes) + "m";
}

String claude_limits(bool with_reset)
{
  if (!shared_mux) return "";
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  String out;
  if (mac.claude_session >= 0) out = "5h " + String(mac.claude_session) + "%";
  if (mac.claude_week >= 0) {
    out += String(out.length() ? " · " : "") + "wk " + String(mac.claude_week) + "%";
    if (with_reset && mac.week_reset_min >= 0) out += " · " + duration(mac.week_reset_min);
  }
  return out;
}

static String deck_subtitle()
{
  int attention = 0, working = 0, done = 0;
  for (const Worktree &w : all_worktrees) {
    for (const Agent &a : w.agents) {
      if (a.bucket == "attention") attention++;
      else if (a.bucket == "working") working++;
      else if (a.bucket == "done") done++;
    }
  }
  String state = attention ? String(attention) + " needs you"
                 : working ? String(working) + " working"
                 : done    ? String(done) + " done"
                           : String(all_worktrees.empty() ? "no workspaces" : "all quiet");
  String limits = claude_limits(false);  // the plan limits next to the state (user, 2026-11-14)
  return limits.length() ? state + " · " + limits : state;
}

static void deck_show()
{
  banner_hide();  // orca-deck is on screen now: it shows the agent itself
  ui_close_pages();
  render();
}

static void deck_hide()
{
  dictate_close();
  ui_close_pages();
  single_tap_at = 0;
  if (commit_at) commit();  // a crown choice still resting goes to Orca, as it would have
}

const App app_deck = {"Claude", LV_SYMBOL_LIST, C_ATTENTION, deck_show, deck_hide, deck_subtitle,
                      deck_turn, deck_tap, deck_hold, deck_swipe};

// ---- test channel: "T …" lines over USB drive the UI as the hands would (like Apex's DebugLaunch) ----

// "T shot": the screen as RGB565 (big-endian: LV_COLOR_16_SWAP), base64 in SHOTD lines between SHOT and SHOT END —
// knob/test/shot.py turns it into a PNG. The top layer (a flying text) is not in it.
static void screenshot()
{
  lv_img_dsc_t *snap = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
  if (!snap) {
    Serial.println("SHOT failed: no memory");
    return;
  }
  Serial.setTxTimeoutMs(200);  // a reader is there (shot.py): wait for it, or the 260 KB drop (boot sets 0)
  Serial.printf("SHOT %d %d\n", (int)snap->header.w, (int)snap->header.h);
  const size_t CHUNK = 3000;  // raw bytes a line
  static unsigned char out[4100];
  for (size_t at = 0; at < snap->data_size; at += CHUNK) {
    size_t n = min(CHUNK, (size_t)snap->data_size - at), written = 0;
    mbedtls_base64_encode(out, sizeof(out), &written, snap->data + at, n);
    out[written] = 0;
    Serial.print("SHOTD ");
    Serial.println((const char *)out);
  }
  Serial.println("SHOT END");
  Serial.flush();
  Serial.setTxTimeoutMs(0);
  lv_snapshot_free(snap);
}

static SemaphoreHandle_t test_mux;
static std::vector<String> test_queue;

void ui_test_enqueue(const String &line)
{
  if (!test_mux) return;
  xSemaphoreTake(test_mux, portMAX_DELAY);
  test_queue.push_back(line);
  xSemaphoreGive(test_mux);
}

static void test_report(const String &what)
{
  String page = hub_report() + (banner_is_shown() ? "+banner" : "");
  if (hub_current() == &app_deck) page += settings_page_open() ? "/settings" : agent_page_is_open() ? "/agent" : "/main";
  String sel = worktrees.empty() ? "-" : ui_title(worktrees[index_shown]);
  const Agent *a = worktrees.empty() ? nullptr : shown_agent(worktrees[index_shown]);
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  String link = link_name(shared.link), status = shared.status;
  xSemaphoreGive(shared_mux);
  Serial.printf("test %s: page=%s sel=%s (%d/%d) agent=%s bucket=%s link=%s status=\"%s\" age=%s%s\n", what.c_str(),
                page.c_str(), sel.c_str(), index_shown + 1, (int)worktrees.size(), a ? a->id.substring(0, 40).c_str() : "-",
                a ? a->bucket.c_str() : "-", link.c_str(), status.c_str(), a ? ago(a->since).c_str() : "",
                agent_page_is_open() ? (" · " + agent_page_debug()).c_str() : "");
}

static void run_test(const String &line)
{
  int space = line.indexOf(' ');
  String verb = space < 0 ? line : line.substring(0, space);
  String arg = space < 0 ? "" : line.substring(space + 1);
  if (verb == "turn") pending_steps += arg.toInt();
  else if (verb == "tap" || verb == "hold" || verb == "swipe" || verb == "open" || verb == "home" || verb == "saver")
    hub_test(verb, arg);
  else if (verb == "select") {  // "select <worktree id>|<agent id>": the knob's own choice, no `go`
    int bar = arg.indexOf('|');
    String wt = bar < 0 ? arg : arg.substring(0, bar);
    int i = index_of(wt);
    if (i >= 0) {
      select_index(i);
      if (bar >= 0) picked_agent[wt] = arg.substring(bar + 1);
      render();
    }
  } else if (verb == "sound") {
    if (arg == "buzz") haptic_buzz(2);
    else if (arg == "test") sound_test();
    else haptic_tick();
    Serial.printf("sound: ready=%d level=%d\n", sound_ready(), settings.sound);
  } else if (verb == "ble-drop") ble_drop();
  else if (verb == "haptics") {  // "haptics" reports; "haptics 3" sets the level (saved)
    if (arg.length()) settings_set_haptics(arg.toInt());
    Serial.println(haptic_report());
  }
  else if (verb == "mic") mic_test(arg.length() ? arg.toInt() : 3);
  else if (verb == "dictate") Serial.println(dictate_report());
  else if (verb == "dictdemo") dictate_demo(arg.length() ? arg : "Перевіряємо роботу шайби без вайфаю.");
  else if (verb == "bloom") bloom_start();  // tests: a done's bloom and tick on the main page
  else if (verb == "fly") fly_away(arg.length() ? arg : "запусти тести");  // tests: a sent text flying away
  else if (verb == "wifi") Serial.println(wifi_store_report());
  else if (verb == "shot") screenshot();
  else if (verb == "bat") Serial.println(battery_report());
  else if (verb == "slow") Serial.println("slowest UI pass: " + ui_slowest_note() + " · " + boot_note);
  else if (verb == "wifitry") wifi_app_test_try(arg);
  else if (verb == "ptarget") prompts_test_target(arg);  // tests: pin the palette to a scratch session
  else if (verb == "subtitle") Serial.println("subtitle: " + deck_subtitle() + " | limits: " + claude_limits(true));
  else if (verb == "alert" && !worktrees.empty()) {  // "alert": the banner for the agent on screen, as a needs-you
    const Agent *a = shown_agent(worktrees[index_shown]);
    if (a) banner_show(worktrees[index_shown].id, a->id, ui_title(worktrees[index_shown]), "attention");
  }
  test_report(line);
}

static void knob_left(void *, void *) { pending_steps--; }
static void knob_right(void *, void *) { pending_steps++; }

static void loop_tick(lv_timer_t *)
{
  static uint32_t last_render = 0;
  std::vector<Worktree> fresh;
  bool got = false;

  xSemaphoreTake(shared_mux, portMAX_DELAY);
  if (shared.version != seen_version) {
    seen_version = shared.version;
    fresh = shared.list;
    got = true;
  }
  xSemaphoreGive(shared_mux);

  if (got) on_state(fresh);

  int steps = pending_steps;
  if (steps) {
    pending_steps -= steps;
    int dir = steps > 0 ? 1 : -1;
    for (int k = 0; k != steps; k += dir) hub_turn(dir);
  }

  uint32_t now = millis();
  if (commit_at && now >= commit_at) commit();
  if (single_tap_at && now >= single_tap_at) {
    single_tap_at = 0;
    // Always `go` first: the knob can be on an agent Orca is not showing (it turns by itself after a send or to a
    // fresh done, without moving Orca) — a tap that only raised Orca left the user stuck (user, 2026-11-14).
    commit();
    push_simple("enter");  // the hub ticked when the finger tapped: a second buzz 260 ms later felt like a stutter
  }
  agent_page_tick();
  settings_tick();
  timer_tick();
  clock_tick();
  mac_tick();
  jog_tick();
  wheel_tick();
  focus_tick();
  usage_tick();
  day_tick();
  prompts_tick();
  micmute_tick();
  prs_tick();
  wifi_app_tick();
  battery_tick();
  cc_tick();
  dictate_tick();
  tz_persist();
  wifi_store_persist();
  hub_tick();
  banner_tick();
  motion(now);
  if (test_mux) {
    std::vector<String> tests;
    xSemaphoreTake(test_mux, portMAX_DELAY);
    tests.swap(test_queue);
    xSemaphoreGive(test_mux);
    for (const String &t : tests) run_test(t);
  }
  if (now - last_render >= 1000) {
    last_render = now;
    render();  // keeps the "9m" ages fresh
    settings_page_refresh();
  }

  String problem = status_problem();
  set_text(status_label, problem.length() ? "• " + problem : "");
  lv_obj_set_style_text_color(status_label, lv_color_hex(C_ATTENTION), 0);
}

extern "C" void ui_init(void)
{
  // First of all: pages paint while they are built and read `shared` under this mutex. Made in link_init (after the
  // UI) it crashed the boot three times (the hub, Focus: assert in xQueueSemaphoreTake).
  if (!shared_mux) shared_mux = xSemaphoreCreateMutex();
  settings_load();  // applied in setup(), after the backlight PWM exists
  tz_load();
  wifi_store_load();

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  // The aura: three soft discs, largest first, behind everything on the main page.
  aura_from = aura_now = lv_color_hex(C_IDLE);
  for (int k = 0; k < AURA_DISCS; k++) {
    int size = 300 - 70 * k;
    lv_obj_t *d = aura[k] = lv_obj_create(screen);
    lv_obj_set_size(d, size, size);
    lv_obj_align(d, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }

  // The halo: a thin ring at the edge in the colour of the agent on screen.
  halo = lv_arc_create(screen);
  lv_obj_set_size(halo, 356, 356);
  lv_obj_center(halo);
  lv_arc_set_bg_angles(halo, 0, 360);
  lv_obj_remove_style(halo, NULL, LV_PART_KNOB);
  lv_obj_remove_style(halo, NULL, LV_PART_INDICATOR);
  lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(halo, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(halo, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_color(halo, lv_color_hex(C_IDLE), LV_PART_MAIN);

  // The comet: a short bright arc that runs round the rim while the agent works.
  comet = lv_arc_create(screen);
  lv_obj_set_size(comet, 356, 356);
  lv_obj_center(comet);
  lv_arc_set_bg_angles(comet, 0, 0);
  lv_arc_set_angles(comet, 0, 46);
  lv_obj_remove_style(comet, NULL, LV_PART_KNOB);
  lv_obj_set_style_arc_opa(comet, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(comet, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(comet, true, LV_PART_INDICATOR);
  lv_obj_clear_flag(comet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(comet, LV_OBJ_FLAG_HIDDEN);

  summary = make_label(screen, &deck_14, C_MUTED, -96, 220);
  state_label = make_label(screen, &deck_bold_16, C_IDLE, -70);
  name_label = make_label(screen, &deck_bold_30, C_INK, -40, 250);
  meta_label = make_label(screen, &deck_14, C_MUTED, -10);
  detail_label = make_label(screen, &deck_16, C_DETAIL, 30, 240);
  lv_obj_set_height(detail_label, 42);  // two lines, then an ellipsis
  counter_label = make_label(screen, &deck_14, C_MUTED, 72);
  status_label = make_label(screen, &deck_14, C_MUTED, 112, 200);
  lv_obj_set_style_text_letter_space(summary, 1, 0);
  lv_obj_set_style_text_letter_space(state_label, 2, 0);
  set_text(name_label, "Claude");

  bloom = lv_obj_create(screen);  // a done's ring, and its tick above the summary
  lv_obj_set_style_radius(bloom, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(bloom, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(bloom, lv_color_hex(C_DONE), 0);
  lv_obj_clear_flag(bloom, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(bloom, LV_OBJ_FLAG_HIDDEN);
  check = lv_line_create(screen);
  lv_obj_set_size(check, 34, 28);
  lv_obj_align(check, LV_ALIGN_CENTER, 0, -128);
  lv_obj_set_style_line_width(check, 5, 0);
  lv_obj_set_style_line_rounded(check, true, 0);
  lv_obj_set_style_line_color(check, lv_color_hex(C_DONE), 0);
  lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);

  fly = make_label(lv_layer_top(), &deck_16, C_INK, 24, 250);
  lv_label_set_long_mode(fly, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(fly, LV_OBJ_FLAG_HIDDEN);

  settings_page_create();
  agent_page_create();
  timer_create();
  clock_create();
  mac_create();
  jog_create();
  wheel_create();
  focus_create();
  usage_create();
  day_create();
  prompts_create();
  micmute_create();
  prs_create();
  wifi_app_create();
  dictate_create();
  banner_create();
  hub_create();
  cc_create();  // over the hub and the apps; the screensaver still covers it  // the hub covers the apps, and the knob boots into it
  buddy_create();  // last: the screensaver covers everything

  sound_init();
  sound_set_level(settings.sound);
  haptic_init();
  knob_config_t cfg = {
    .gpio_encoder_a = KNOB_PIN_A,
    .gpio_encoder_b = KNOB_PIN_B,
  };
  knob_handle_t knob = iot_knob_create(&cfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right, NULL);
  }
  test_mux = xSemaphoreCreateMutex();
  lv_timer_create(loop_tick, 16, NULL);
  link_init();
}

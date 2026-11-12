// orca-deck knob: the main page and the page switching. Pages as Apex (watch/Apex/ContentView.swift):
// Settings ← (swipe right) the main page → (swipe left) the agent page.
// Main page: crown → workspace (Orca follows after 1 s of rest) · tap → enter · hold → next agent ·
// double tap → send. Selection rules come from Apex's Selection (watch/ApexCore/.../Selection.swift).
#include "deck.h"
#include <map>
#include <math.h>
#include <time.h>
#include "bidi_switch_knob.h"
#include "sound.h"

#define KNOB_PIN_A 8
#define KNOB_PIN_B 7

#define COMMIT_MS 1000       // the crown rests this long before Orca is told to switch
#define GO_PATIENCE_MS 4000  // how long the screen keeps the choice while Orca gets there
#define DOUBLE_TAP_MS 260    // a tap waits this long to see whether it becomes a double tap

#define MAX_DOTS 48
#define RING_RADIUS 163

// LVGL-task state.
static std::vector<Worktree> all_worktrees, worktrees;  // the bridge's list, and the visible one
static uint32_t seen_version = 0;
static int index_shown = 0;
static String selected_id, last_active_id;
static uint32_t commit_at = 0, chosen_until = 0, single_tap_at = 0;
static std::map<String, String> picked_agent;  // worktree id → agent picked by holding
static std::map<String, String> last_bucket;   // agent id → bucket, for the done / needs-you buzz
static volatile int pending_steps = 0;

static lv_obj_t *dots[MAX_DOTS];
static int dot_count = 0;
static String dot_ids;
static lv_obj_t *comet;
static String motion_bucket;  // the bucket of the agent on screen, for the comet and the sonar
static lv_obj_t *halo, *summary, *state_label, *name_label, *meta_label, *detail_label, *counter_label, *status_label;

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
      dots[i] = lv_obj_create(lv_scr_act());
      lv_obj_clear_flag(dots[i], (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
      lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_pad_all(dots[i], 0, 0);
      lv_obj_move_background(dots[i]);
    }
    if (dots[i]) {
      if (i < n) lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  dot_count = n;
}

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
  for (int i = 0; i < dot_count; i++) {
    // The dial turns so the chosen workspace sits at 12 o'clock.
    float angle = (float)(i - index_shown) / dot_count * 2 * M_PI - M_PI / 2;
    const Worktree &w = worktrees[i];
    const Agent *lead = lead_agent(w);
    String bucket = lead ? lead->bucket : "idle";
    bool sel = i == index_shown;
    int size = sel ? 18 : 10;
    lv_obj_set_size(dots[i], size, size);
    lv_obj_set_pos(dots[i], 180 + RING_RADIUS * cosf(angle) - size / 2, 180 + RING_RADIUS * sinf(angle) - size / 2);
    lv_color_t c = lv_color_hex(color_of(bucket));
    lv_obj_set_style_bg_color(dots[i], c, 0);
    lv_obj_set_style_bg_opa(dots[i], w.agents.empty() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dots[i], w.agents.empty() ? lv_color_hex(0x444444) : c, 0);
    lv_obj_set_style_border_width(dots[i], 2, 0);
    lv_obj_set_style_shadow_width(dots[i], sel ? 14 : 0, 0);
    lv_obj_set_style_shadow_color(dots[i], c, 0);
  }

  if (!n) {
    set_text(state_label, "");
    set_text(name_label, "orca-deck");
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
  for (const Worktree &w : all_worktrees) {
    for (const Agent &a : w.agents) {
      auto it = last_bucket.find(a.id);
      if (it != last_bucket.end() && it->second != a.bucket) {
        if (a.bucket == "attention") pulses = 2;
        else if (a.bucket == "done" && it->second == "working") {
          pulses = max(pulses, 1);
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
  if (pulses) haptic_buzz(pulses);
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
void ui_after_send(const String &agent_id)
{
  ui_close_pages();
  String agent;
  int i = next_waiting(index_shown, agent_id, agent);
  if (i >= 0) {
    select_index(i);
    picked_agent[worktrees[i].id] = agent;
  }
  render();
}

// ---- motion (Apex: comet while working, sonar while it needs you, one calm swap per crown step) ----

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
  static uint32_t last = 0;
  if (now - last < 33) return;
  last = now;
  bool main_page = !settings_page_open() && !agent_page_is_open();
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
}

// ---- input ----

static void on_short_click(lv_event_t *)
{
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

static void on_long_press(lv_event_t *)
{
  if (!settings_page_open() && !agent_page_is_open()) next_agent();
}

static void swipe(lv_dir_t dir)
{
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
    settings_page_show(true);
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

static void on_gesture(lv_event_t *)
{
  swipe(lv_indev_get_gesture_dir(lv_indev_get_act()));
}

// ---- test channel: "T …" lines over USB drive the UI as the hands would (like Apex's DebugLaunch) ----

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
  String page = settings_page_open() ? "settings" : agent_page_is_open() ? "agent" : "main";
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
  else if (verb == "tap") on_short_click(nullptr);
  else if (verb == "hold") on_long_press(nullptr);
  else if (verb == "swipe") swipe(arg == "left" ? LV_DIR_LEFT : LV_DIR_RIGHT);
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
  bool online = shared.online;
  String status = shared.status;
  xSemaphoreGive(shared_mux);

  if (got) on_state(fresh);

  int steps = pending_steps;
  if (steps) {
    pending_steps -= steps;
    int dir = steps > 0 ? 1 : -1;
    for (int k = 0; k != steps; k += dir) {
      if (settings_page_open()) settings_page_turn(dir);
      else if (agent_page_is_open()) agent_page_turn(dir);
      else turn(dir);
    }
  }

  uint32_t now = millis();
  if (commit_at && now >= commit_at) commit();
  if (single_tap_at && now >= single_tap_at) {
    single_tap_at = 0;
    if (commit_at) commit();
    haptic_click();
    push_simple("enter");
  }
  agent_page_tick();
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

  set_text(status_label, "• " + status);
  lv_obj_set_style_text_color(status_label, lv_color_hex(online ? C_DONE : C_ATTENTION), 0);
}

extern "C" void ui_init(void)
{
  settings_load();  // applied in setup(), after the backlight PWM exists

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(screen, on_short_click, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(screen, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);

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
  set_text(name_label, "orca-deck");

  settings_page_create();
  agent_page_create();

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

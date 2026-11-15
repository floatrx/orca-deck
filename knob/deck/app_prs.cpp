// PR Radar, an app in the hub (idea 2 in .work/notes/knob-app-ideas.md): your open PRs and the ones waiting on your
// review, round the rim — one arc a PR, in its checks' colour: red failing, blue running (it pulses), green passed,
// grey none. Review requests come first (clockwise from the top), then your own.
//   crown: the PR in the middle · tap: open it in the browser · hold: re-run its failed jobs — armed, a tap confirms
// When a running PR lands (fail or pass), a banner says so in any app.
// Data: the bridge's GET /api/prs (`gh api graphql`, every 60 s) via the relay's R line. The knob names a PR by its
// key; the bridge opens only PRs it listed itself.
#include "deck.h"
#include <map>
#include <math.h>

#define MAX_PRS 10
#define ARM_MS 4000
#define C_FAIL 0xFF4B4B

static lv_obj_t *page, *arcs[MAX_PRS], *head_label, *repo_label, *num_label, *title_label, *state_label, *hint_label,
    *spinner;
static std::vector<PrItem> prs;
static String error;
static bool loaded = false;
static uint32_t seen_version = 0, armed_at = 0;
static int sel = 0;
static String sel_key;  // the selection follows its PR when the list reorders
static std::map<String, String> last_checks;  // key → checks, for the "it landed" banner

static uint32_t checks_color(const String &checks)
{
  if (checks == "fail") return C_FAIL;
  if (checks == "run") return C_WORKING;
  if (checks == "ok") return C_DONE;
  return C_IDLE;
}

static String short_repo(const String &key)  // "MGear-AI/core#210" → "core"
{
  int slash = key.indexOf('/'), hash = key.lastIndexOf('#');
  return key.substring(slash + 1, hash < 0 ? key.length() : hash);
}

static String number_of(const String &key)
{
  int hash = key.lastIndexOf('#');
  return hash < 0 ? "" : key.substring(hash);
}

static void layout()
{
  int n = min((int)prs.size(), MAX_PRS);
  float span = n ? 360.0f / n : 360;
  for (int i = 0; i < MAX_PRS; i++) {
    if (i >= n) {
      lv_obj_add_flag(arcs[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(arcs[i], LV_OBJ_FLAG_HIDDEN);
    int gap = n > 1 ? 5 : 0;
    int start = (int)lroundf(270 + i * span + gap / 2.0f) % 360;
    int end = (int)lroundf(270 + (i + 1) * span - gap / 2.0f) % 360;
    lv_arc_set_bg_angles(arcs[i], start, n > 1 ? end : (start + 359) % 360);
    bool on = i == sel;
    lv_obj_set_style_arc_width(arcs[i], on ? 18 : 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arcs[i], lv_color_hex(checks_color(prs[i].checks)), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arcs[i], on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  }
}

static void paint()
{
  if (!page) return;
  int fails = 0, runs = 0, asks = 0;
  for (const PrItem &p : prs) {
    fails += p.checks == "fail";
    runs += p.checks == "run";
    asks += !p.mine;
  }
  bool waiting = !loaded && !error.length();
  waiting ? lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  if (prs.empty()) {
    set_text(head_label, "PR RADAR");
    set_text(repo_label, "");
    set_text(num_label, "");
    set_text(title_label, error.length() ? error : waiting ? "asking GitHub…" : "no open PRs");
    set_text(state_label, "");
    set_text(hint_label, "");
    layout();
    return;
  }
  String head = String(prs.size()) + " open";
  if (fails) head += " · " + String(fails) + " failing";
  if (runs) head += " · " + String(runs) + " running";
  if (asks) head += " · " + String(asks) + " to review";
  set_text(head_label, head);
  const PrItem &p = prs[sel];
  set_text(repo_label, short_repo(p.key));
  set_text(num_label, number_of(p.key));
  lv_obj_set_style_text_color(num_label, lv_color_hex(checks_color(p.checks)), 0);
  set_text(title_label, p.title);
  String state = p.checks == "fail" ? "checks failing" : p.checks == "run" ? "checks running" : p.checks == "ok" ? "checks passed" : "no checks";
  if (!p.mine) state = "review requested · " + state;
  else if (p.review == "approved") state += " · approved";
  else if (p.review == "changes") state += " · changes asked";
  set_text(state_label, state);
  bool armed = armed_at && millis() - armed_at < ARM_MS;
  set_text(hint_label, armed ? "tap: re-run failed jobs" : p.rerun ? "tap: open · hold: re-run" : "tap: open in browser");
  lv_obj_set_style_text_color(hint_label, lv_color_hex(armed ? C_ATTENTION : C_MUTED), 0);
  layout();
}

static void pick(int i)
{
  sel = max(0, min(i, (int)prs.size() - 1));
  sel_key = prs.empty() ? "" : prs[sel].key;
  armed_at = 0;
}

static void take_prs()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool fresh = shared.prs_version != seen_version;
  if (fresh) {
    seen_version = shared.prs_version;
    prs = shared.prs;
    if ((int)prs.size() > MAX_PRS) prs.resize(MAX_PRS);
    error = shared.prs_error;
    loaded = shared.prs_loaded;
  }
  xSemaphoreGive(shared_mux);
  if (!fresh) return;
  // A running PR that landed: a banner in any app (not the first list: that is no news).
  bool first = last_checks.empty();
  std::map<String, String> now;
  for (const PrItem &p : prs) {
    now[p.key] = p.checks;
    auto was = last_checks.find(p.key);
    if (!first && was != last_checks.end() && was->second == "run" && (p.checks == "fail" || p.checks == "ok")) {
      if (p.checks == "fail") pet_event(PET_PR_FAIL);  // a bug crawls in on Clawd (pet.cpp)
      if (p.checks == "fail" || !focus_is_on()) {
        banner_show_app(&app_prs, short_repo(p.key) + number_of(p.key) + (p.checks == "fail" ? " checks failed" : " checks passed"));
      }
    }
  }
  if (loaded) last_checks = now;
  int i = 0;
  while (i < (int)prs.size() && prs[i].key != sel_key) i++;
  pick(i < (int)prs.size() ? i : sel);
  paint();
}

static void turn(int step)
{
  if (prs.size() < 2) return;
  pick((sel + step + prs.size()) % prs.size());  // round the rim
  haptic_tick();
  paint();
}

static void send_pr(const char *action)
{
  JsonDocument cmd;
  cmd["cmd"] = "pr";
  cmd["action"] = action;
  cmd["n"] = prs[sel].key;
  push_command(cmd);
}

static void tap()
{
  if (prs.empty()) return;
  if (armed_at && millis() - armed_at < ARM_MS) {
    armed_at = 0;
    send_pr("rerun");
    haptic_click(2);
    set_text(hint_label, "re-running…");
    return;
  }
  send_pr("open");
  haptic_click();
  set_text(hint_label, "opened on the Mac");
}

static void hold()
{
  if (prs.empty() || !prs[sel].rerun) {
    haptic_buzz();
    return;
  }
  armed_at = millis();
  haptic_click();
  paint();
}

void prs_tick()
{
  take_prs();  // shown or not: the banner
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) return;
  uint32_t now = millis();
  static uint32_t last_frame = 0, last_arm = 0;
  if (armed_at && now - armed_at >= ARM_MS && now - last_arm > 200) {  // the arm ran out
    last_arm = now;
    armed_at = 0;
    paint();
  }
  if (now - last_frame < 33) return;  // running checks pulse, 30 fps
  last_frame = now;
  float pulse = 0.5f + 0.5f * sinf(now / 1400.0f * 2 * M_PI);
  for (int i = 0; i < (int)prs.size(); i++) {
    if (prs[i].checks != "run") continue;
    lv_opa_t base = i == sel ? 150 : 60;
    lv_obj_set_style_arc_opa(arcs[i], (lv_opa_t)(base + (255 - base) * pulse), LV_PART_MAIN);
  }
}

static String subtitle()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  int n = shared.prs.size(), fails = 0, asks = 0;
  for (const PrItem &p : shared.prs) {
    fails += p.checks == "fail";
    asks += !p.mine;
  }
  bool has = shared.prs_loaded;
  xSemaphoreGive(shared_mux);
  if (!has) return "your PRs and reviews";
  if (!n) return "no open PRs";
  String out = String(n) + " open";
  if (fails) out += " · " + String(fails) + " failing";
  if (asks) out += " · " + String(asks) + " review";
  return out;
}

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  armed_at = 0;
  paint();
}

static void hide()
{
  armed_at = 0;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

String prs_report()
{
  return String("n=") + prs.size() + " sel=" + sel + " loaded=" + loaded + (armed_at ? " ARMED" : "") + " | " +
         lv_label_get_text(head_label) + " | " + lv_label_get_text(repo_label) + lv_label_get_text(num_label) + " | " +
         lv_label_get_text(state_label) + " | " + lv_label_get_text(hint_label);
}

void prs_create()
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

  for (int i = 0; i < MAX_PRS; i++) {
    lv_obj_t *a = arcs[i] = lv_arc_create(page);
    lv_obj_set_size(a, 340, 340);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_style(a, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
  }
  head_label = make_label(page, &deck_14, C_MUTED, -110, 230);
  repo_label = make_label(page, &deck_bold_16, C_DETAIL, -72, 220);
  num_label = make_label(page, &deck_bold_30, C_INK, -42, 220);
  title_label = make_label(page, &deck_16, C_INK, 4, 250);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);  // two lines fit between the number and the state
  state_label = make_label(page, &deck_14, C_DETAIL, 56, 240);
  hint_label = make_label(page, &deck_14, C_MUTED, 112, 200);
  spinner = lv_spinner_create(page, 1000, 60);
  lv_obj_set_size(spinner, 28, 28);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_arc_width(spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
  lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  paint();
}

const App app_prs = {"PRs", LV_SYMBOL_GPS, 0x7FD1B9, show, hide, subtitle, turn, tap, hold, nullptr};

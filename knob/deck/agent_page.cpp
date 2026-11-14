// The agent page, as Apex's AgentView: the full last message, the Next variants with ticks (the first one, the
// recommendation, ticked from the start), one Send for the ticked ones, or the pending question's options.
// Send and an answer go at once (no undo countdown); afterwards the knob goes back to the main page, turned to
// the next waiting agent. No variants while the agent works — they belong to its previous turn.
#include "deck.h"
#include <set>

static lv_obj_t *page, *title, *meta, *state, *body, *message;
static std::vector<lv_obj_t *> rows;  // focusable rows in body order
static std::vector<int> row_kind;     // ROW_* below
static std::vector<int> row_index;    // the variant / option index

enum { ROW_VARIANT, ROW_SEND, ROW_OPTION };

static String agent_id, agent_bucket;
static AgentDetail page_detail;
static uint32_t seen_version = 0;
static String signature;           // what the rows were built from: rebuild only when it changes
static std::set<String> ticked;    // ticked variants, by text: survive a refresh of the same list
static int focus = -1;             // -1: the message
static bool loaded = false;

static String compose()
{
  std::vector<String> chosen;
  for (const String &v : page_detail.next) {
    if (ticked.count(v)) chosen.push_back(v);
  }
  if (chosen.empty()) return "";
  if (chosen.size() == 1) return chosen[0];
  String text;
  for (size_t i = 0; i < chosen.size(); i++) text += (i ? "\n" : "") + String(i + 1) + ". " + chosen[i];
  return text;
}

static void style_row(lv_obj_t *row, bool focused, bool filled)
{
  lv_obj_set_style_border_color(row, lv_color_hex(focused ? C_INK : 0x2C2D31), 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(filled ? 0x1C3A2A : 0x141518), 0);
}

static void paint()
{
  for (size_t i = 0; i < rows.size(); i++) {
    bool filled = row_kind[i] == ROW_VARIANT && ticked.count(page_detail.next[row_index[i]]);
    if (row_kind[i] == ROW_SEND) filled = compose().length() > 0;
    style_row(rows[i], (int)i == focus, filled);
    if (row_kind[i] == ROW_VARIANT) {
      lv_obj_t *mark = lv_obj_get_child(rows[i], 0);
      lv_label_set_text(mark, filled ? LV_SYMBOL_OK : "");
    }
  }
  if (focus >= 0 && focus < (int)rows.size()) lv_obj_scroll_to_view(rows[focus], LV_ANIM_ON);
  else lv_obj_scroll_to_y(body, 0, LV_ANIM_ON);
}

static lv_obj_t *add_row(const String &text, int kind, int index)
{
  lv_obj_t *row = lv_obj_create(body);
  lv_obj_set_width(row, 236);
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(row, 12, 0);
  lv_obj_set_style_border_width(row, 2, 0);
  lv_obj_set_style_pad_all(row, 8, 0);
  lv_obj_set_style_pad_left(row, kind == ROW_VARIANT ? 28 : 10, 0);
  lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  if (kind == ROW_VARIANT) {
    lv_obj_t *mark = lv_label_create(row);  // child 0: the tick
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_20, 0);  // built-in: carries LV_SYMBOL_OK
    lv_obj_set_style_text_color(mark, lv_color_hex(C_DONE), 0);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, -22, 0);
    lv_label_set_text(mark, "");
  }
  lv_obj_t *label = lv_label_create(row);
  lv_obj_set_width(label, kind == ROW_VARIANT ? 196 : 214);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(label, kind == ROW_SEND ? &deck_bold_16 : &deck_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(kind == ROW_SEND ? C_DONE : C_INK), 0);
  lv_obj_set_style_text_align(label, kind == ROW_SEND ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_text(label, text.c_str());
  rows.push_back(row);
  row_kind.push_back(kind);
  row_index.push_back(index);
  return row;
}

static lv_obj_t *status_line;  // "Working…" under the message: a spinner and grey text, not focusable

static void add_status(const char *text)
{
  status_line = lv_obj_create(body);
  lv_obj_set_size(status_line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(status_line, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(status_line, 0, 0);
  lv_obj_set_style_pad_all(status_line, 0, 0);
  lv_obj_set_style_pad_column(status_line, 8, 0);
  lv_obj_set_flex_flow(status_line, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status_line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(status_line, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_t *spin = lv_spinner_create(status_line, 1000, 60);
  lv_obj_set_size(spin, 16, 16);
  lv_obj_set_style_arc_width(spin, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spin, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spin, lv_color_hex(0x2C2D31), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spin, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
  lv_obj_clear_flag(spin, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *label = lv_label_create(status_line);
  lv_obj_set_style_text_font(label, &deck_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(C_MUTED), 0);
  lv_label_set_text(label, text);
}

static void rebuild()
{
  for (lv_obj_t *row : rows) lv_obj_del(row);
  rows.clear();
  if (status_line) lv_obj_del(status_line), status_line = nullptr;
  row_kind.clear();
  row_index.clear();

  String text;
  if (!loaded) text = "loading…";
  else if (page_detail.has_question) text = page_detail.question.length() ? page_detail.question : page_detail.message;
  else text = page_detail.message.length() ? page_detail.message : "(no message)";
  lv_label_set_text(message, text.c_str());

  bool working = agent_bucket == "working" || page_detail.bucket == "working";
  if (loaded && page_detail.has_question) {
    if (page_detail.answerable) {
      for (size_t i = 0; i < page_detail.options.size(); i++) add_row(page_detail.options[i].label, ROW_OPTION, i);
    } else {
      add_row("answer it in Orca", ROW_OPTION, -1);
    }
  } else if (loaded && working) {
    add_status("Working…");  // plain grey with a spinner, not a row: nothing to pick (user, 2026-11-14)
  } else if (loaded && !page_detail.next.empty()) {
    for (size_t i = 0; i < page_detail.next.size(); i++) add_row((i == 0 ? "• " : "") + page_detail.next[i], ROW_VARIANT, i);
    add_row("Send", ROW_SEND, 0);
  }
  // Apex opens the page scrolled to the ticks: focus the first actionable row.
  focus = rows.empty() ? -1 : 0;
  paint();
}

static void take_detail()
{
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  bool fresh = shared.detail_version != seen_version && shared.detail.id == agent_id;
  if (fresh) {
    seen_version = shared.detail_version;
    page_detail = shared.detail;
  }
  xSemaphoreGive(shared_mux);
  if (!fresh) return;
  loaded = true;
  String sig = page_detail.bucket + "|" + page_detail.message + "|" + page_detail.item_id + "|" + String(page_detail.revision);
  for (const String &v : page_detail.next) sig += "|" + v;
  if (sig == signature) return;
  bool same_variants = signature.length() && page_detail.next.size() && ticked.size();
  signature = sig;
  if (!same_variants) {
    ticked.clear();
    if (!page_detail.next.empty()) ticked.insert(page_detail.next[0]);  // the recommendation, ticked from the start
  }
  rebuild();
}

String agent_page_debug()
{
  String out = "agent=" + agent_id.substring(0, 48) + " loaded=" + String(loaded) + " rows=" + String(rows.size()) +
               " focus=" + String(focus) + " ticked=" + String(ticked.size()) + " next=" + String(page_detail.next.size()) +
               " question=" + String(page_detail.has_question);
  return out;
}

void agent_page_tick()
{
  if (agent_page_is_open()) take_detail();
}

void agent_page_open(const Worktree &w, const Agent &a)
{
  agent_id = a.id;
  agent_bucket = a.bucket;
  loaded = false;
  signature = "";
  ticked.clear();
  set_text(title, ui_title(w));
  set_text(meta, ui_meta(w, &a));
  set_text(state, a.bucket == "attention" ? "NEEDS YOU" : a.bucket == "working" ? "WORKING" : a.bucket == "done" ? "DONE" : "IDLE");
  lv_obj_set_style_text_color(state, lv_color_hex(color_of(a.bucket)), 0);
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.want_detail = a.id;
  xSemaphoreGive(shared_mux);
  rebuild();
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
}

void agent_page_close()
{
  if (!page) return;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  shared.want_detail = "";
  xSemaphoreGive(shared_mux);
}

bool agent_page_is_open() { return page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN); }

void agent_page_turn(int step)
{
  int next = focus + step;
  if (next < -1 || next >= (int)rows.size()) return;
  focus = next;
  haptic_tick();
  paint();
}

void agent_page_tap()
{
  if (focus < 0 || focus >= (int)rows.size()) return;
  int kind = row_kind[focus], index = row_index[focus];
  if (kind == ROW_VARIANT) {
    const String &v = page_detail.next[index];
    if (ticked.count(v)) ticked.erase(v);
    else ticked.insert(v);
    haptic_click();
    paint();
  } else if (kind == ROW_SEND) {
    String text = compose();
    if (!text.length()) {
      haptic_buzz(1);
      return;
    }
    JsonDocument cmd;
    cmd["cmd"] = "say";
    cmd["id"] = agent_id;
    cmd["text"] = text;
    push_command(cmd);
    haptic_click(2);
    ui_after_send(agent_id);
  } else if (kind == ROW_OPTION && index >= 0) {
    JsonDocument cmd;
    cmd["cmd"] = "answer";
    cmd["id"] = agent_id;
    cmd["itemId"] = page_detail.item_id;
    cmd["revision"] = page_detail.revision;
    cmd["optionId"] = page_detail.options[index].id;
    push_command(cmd);
    haptic_click(2);
    ui_after_send(agent_id);
  }
}

void agent_page_create()
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

  state = make_label(page, &deck_bold_16, C_IDLE, -132, 160);
  lv_obj_set_style_text_letter_space(state, 2, 0);
  title = make_label(page, &deck_bold_16, C_INK, -110, 220);
  meta = make_label(page, &deck_14, C_MUTED, -90, 230);

  // The scrolling body: the message, then the rows. Narrower than the screen: the corners are round.
  body = lv_obj_create(page);
  lv_obj_set_size(body, 256, 236);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 46);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 0, 0);
  lv_obj_set_style_pad_row(body, 8, 0);
  lv_obj_set_style_pad_bottom(body, 44, 0);  // the last line clears the round bottom edge (it clipped "Working…")
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

  message = lv_label_create(body);
  lv_obj_set_width(message, 240);
  lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(message, &deck_14, 0);
  lv_obj_set_style_text_color(message, lv_color_hex(C_DETAIL), 0);
  lv_label_set_text(message, "");
}

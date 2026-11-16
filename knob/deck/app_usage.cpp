// Claude Usage, an app in the hub (idea 5 in .work/notes/knob-app-ideas.md, on the plan limits instead of token
// counts): how much of the week is gone, how long until it resets, and what it went on.
//   outer dial: the week, in its severity's colour · inner dial: the 5 h window when open, else a per-model limit
//   ("Fable") · centre: the week's % and the time to its reset · bottom: a stacked bar of the week's breakdown
//   (Claude Code / Cowork / Chats …)
// Data: the bridge's /api/mac → claude (api.anthropic.com/api/oauth/usage, as Orca's status bar), via the M line.
#include "deck.h"

#define MAX_PARTS 4
static const uint32_t PART_COLORS[MAX_PARTS] = {0xD97757, 0x4CC3FF, 0x3DDC84, 0xB18CFF};

static lv_obj_t *page, *week_arc, *inner_arc, *title_label, *pct_label, *reset_label, *inner_label, *legend_label;
static lv_obj_t *bar, *parts[MAX_PARTS];

struct Part {
  String name;
  int percent;
};

// "Claude Code 41|Cowork 59" → parts, the long product name shortened for the round screen.
static int parse(const String &list, Part *out, int max_parts)
{
  int n = 0, from = 0;
  while (from < (int)list.length() && n < max_parts) {
    int bar_at = list.indexOf('|', from);
    String item = list.substring(from, bar_at < 0 ? list.length() : bar_at);
    int space = item.lastIndexOf(' ');
    if (space > 0) {
      out[n].name = item.substring(0, space);
      out[n].name.replace("Claude Code", "Code");
      out[n].percent = item.substring(space + 1).toInt();
      n++;
    }
    if (bar_at < 0) break;
    from = bar_at + 1;
  }
  return n;
}

static String duration(int minutes)
{
  if (minutes < 0) return "";
  if (minutes >= 1440) return String(minutes / 1440) + L("d ", " д ") + String(minutes % 1440 / 60) + L("h", " год");
  if (minutes >= 60) return String(minutes / 60) + L("h ", " год ") + String(minutes % 60) + L("m", " хв");
  return String(minutes) + L("m", " хв");
}

static void paint()
{
  if (!page) return;
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  MacStatus mac = shared.mac;
  xSemaphoreGive(shared_mux);
  bool known = mac.claude_week >= 0;
  uint32_t sev = mac.week_severity == "critical" ? 0xFF4B4B : mac.week_severity == "warning" ? C_ATTENTION : C_WORKING;
  lv_arc_set_value(week_arc, max(0, mac.claude_week));
  lv_obj_set_style_arc_color(week_arc, lv_color_hex(sev), LV_PART_INDICATOR);
  set_text(pct_label, known ? String(mac.claude_week) + "%" : "--");
  lv_obj_set_style_text_color(pct_label, lv_color_hex(known ? C_INK : C_MUTED), 0);
  set_text(reset_label, mac.week_reset_min >= 0 ? L("resets in ", "скинеться через ") + duration(mac.week_reset_min) : known ? "" : L("no data from the Mac", "немає даних від Мака"));

  // The inner dial: the 5 h window while one is open, else the first per-model limit.
  Part models[2];
  int model_count = parse(mac.models, models, 2);
  if (mac.claude_session >= 0) {
    lv_arc_set_value(inner_arc, mac.claude_session);
    lv_obj_set_style_arc_color(inner_arc, lv_color_hex(0xD97757), LV_PART_INDICATOR);
    set_text(inner_label, "5h " + String(mac.claude_session) + "% · " + duration(mac.session_reset_min));
    lv_obj_clear_flag(inner_arc, LV_OBJ_FLAG_HIDDEN);
  } else if (model_count) {
    lv_arc_set_value(inner_arc, models[0].percent);
    lv_obj_set_style_arc_color(inner_arc, lv_color_hex(0xB18CFF), LV_PART_INDICATOR);
    set_text(inner_label, models[0].name + " " + String(models[0].percent) + "%");
    lv_obj_clear_flag(inner_arc, LV_OBJ_FLAG_HIDDEN);
  } else {
    set_text(inner_label, "");
    lv_obj_add_flag(inner_arc, LV_OBJ_FLAG_HIDDEN);
  }

  // The breakdown: one stacked bar, a part per product, widths by share; the legend under it.
  Part rows[MAX_PARTS];
  int n = parse(mac.breakdown, rows, MAX_PARTS), total = 0, x = 0;
  for (int i = 0; i < n; i++) total += rows[i].percent;
  String legend;
  for (int i = 0; i < MAX_PARTS; i++) {
    if (i >= n || !total) {
      lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    int w = i == n - 1 ? 160 - x : 160 * rows[i].percent / total;
    lv_obj_clear_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(parts[i], max(2, w - 2), 8);
    lv_obj_set_pos(parts[i], x, 0);
    x += w;
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", (unsigned)PART_COLORS[i]);
    legend += String(legend.length() ? "  " : "") + "#" + hex + " " + rows[i].name + " " + String(rows[i].percent) + "#";
  }
  n && total ? lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  set_text(legend_label, legend);
}

void usage_tick()
{
  static uint32_t last = 0;
  if (!page || lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN) || millis() - last < 1000) return;
  last = millis();
  paint();
}

static String subtitle() { return claude_limits(true); }

static void show()
{
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  paint();
}

static void hide() { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); }

String usage_report()
{
  return String(lv_label_get_text(pct_label)) + " " + lv_label_get_text(reset_label) + " | " + lv_label_get_text(inner_label) +
         " | " + lv_label_get_text(legend_label);
}

static lv_obj_t *dial(int size, int width)
{
  lv_obj_t *a = lv_arc_create(page);
  lv_obj_set_size(a, size, size);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, 0, 100);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, lv_color_hex(0x1C1D21), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  return a;
}

void usage_create()
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

  week_arc = dial(330, 14);
  inner_arc = dial(292, 6);
  title_label = make_label(page, &deck_bold_16, C_MUTED, -66, 200);
  lv_obj_set_style_text_letter_space(title_label, 3, 0);
  set_text(title_label, L("CLAUDE WEEK", "CLAUDE ТИЖДЕНЬ"));
  pct_label = make_label(page, &lv_font_montserrat_48, C_INK, -18, 200);
  reset_label = make_label(page, &deck_16, C_MUTED, 22, 220);
  inner_label = make_label(page, &deck_14, 0xB18CFF, 48, 220);

  bar = lv_obj_create(page);  // the breakdown's stacked bar, inside the dial's open bottom
  lv_obj_set_size(bar, 160, 8);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, 84);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  for (int i = 0; i < MAX_PARTS; i++) {
    lv_obj_t *p = parts[i] = lv_obj_create(bar);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(PART_COLORS[i]), 0);
    lv_obj_clear_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  legend_label = make_label(page, &deck_14, C_DETAIL, 106, 200);
  lv_label_set_recolor(legend_label, true);  // each name in its part's colour
}

const App app_usage = {"Usage", LV_SYMBOL_BARS, 0xD97757, show, hide, subtitle, nullptr, nullptr, nullptr, nullptr};

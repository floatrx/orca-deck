// Dictation in orca-deck (user, 2026-11-14): a hold on an agent starts it, then the finger is free — speak, and tap
// when done («тап на початок і тап коли диктовку закінчено»; a plain tap cannot start it: in orca-deck it is
// `enter`). The relay has the audio transcribed (OpenAI gpt-4o-transcribe) and the text goes to the agent at once
// (`say`) — no confirming tap («після транскрибації … автоматично відправляти промт»); the screen shows it as SENT
// for a moment. A swipe while listening cancels: the only way to not send.
// The send plays out (user, 2026-11-14: «показувати трішки довше розпізнаний текст і з якоюсь анімацією … відправляти»):
// the transcript types itself in, a green ring closes round it (SENDING → SENT), then the text rises off the
// screen and Claude takes over. ~2.8 s; a tap skips to the end.
// .work/notes/knob-dictation.md
#include "deck.h"
#include <math.h>

#define TRANSCRIBE_WAIT_MS 30000
#define TYPE_MS_PER_CHAR 28   // the typewriter, up to TYPE_MAX_MS for a long text
#define TYPE_MAX_MS 1100
#define SWEEP_MS 900          // the ring closing
#define HOLD_MS 450           // SENT, whole
#define RISE_MS 420           // the text rising away

enum Stage { CLOSED, RECORDING, TRANSCRIBING, RESULT, FAILED };

static lv_obj_t *sweep;  // the green ring that closes while the text is sent
static uint32_t type_ms = 0;
static lv_obj_t *page, *ring, *spinner, *title_label, *state_label, *time_label, *text_label, *hint_label;
// The equalizer while listening (user, 2026-11-14: the factory firmware's spectrum look, but round): 64 bars radiate
// round the rim, mirrored left / right — low frequencies at the top, high ones meeting at the bottom — magenta →
// blue → cyan, white tips; they fall back slowly. In silence: a thin ring.
#define EQ_BARS 64
#define EQ_R0 120   // where the bars start
#define EQ_LEN 54   // the longest bar
static lv_obj_t *eq;
static lv_color_t *eq_buf;
static float eq_shown[EQ_BARS / 2];
static Stage stage = CLOSED;
static String agent_id, text;
static String cmd_template;  // dictate_begin_cmd: the command the transcript goes into (a feed app), else a `say`
static uint32_t started_at = 0, sent_at = 0, seen_version = 0;
// Staged dictation for a recording (`T dictdemo listen <text>`): the next hold runs the real screens, but the
// microphone stays off (the equalizer plays a synthetic voice), and after the tap `text` comes back as the
// transcript a moment later. Nothing is recorded, transcribed or sent. One use, then off.
static bool staged = false;
static String staged_text;
#define STAGED_TRANSCRIBE_MS 1100

static void show_stage(Stage next)
{
  stage = next;
  bool rec = next == RECORDING, wait = next == TRANSCRIBING, done = next == RESULT, failed = next == FAILED;
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);  // the equalizer replaced it; kept for a build without PSRAM
  if (eq) rec ? lv_obj_clear_flag(eq, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(eq, LV_OBJ_FLAG_HIDDEN);
  // Inside the ring of bars while listening; the usual places otherwise.
  lv_obj_align(state_label, LV_ALIGN_CENTER, 0, rec ? -62 : -120);
  lv_obj_align(title_label, LV_ALIGN_CENTER, 0, rec ? -38 : -94);
  lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, rec ? 56 : 120);
  if (rec) memset(eq_shown, 0, sizeof(eq_shown));
  wait ? lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  rec ? lv_obj_clear_flag(time_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(time_label, LV_OBJ_FLAG_HIDDEN);
  done || failed ? lv_obj_clear_flag(text_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(text_label, LV_OBJ_FLAG_HIDDEN);
  set_text(state_label, rec ? L("LISTENING", "СЛУХАЮ") : wait ? L("TRANSCRIBING", "РОЗПІЗНАЮ") : done ? L("SENT", "НАДІСЛАНО") : failed ? L("NOT SENT", "НЕ НАДІСЛАНО") : "");
  lv_obj_set_style_text_color(state_label, lv_color_hex(rec ? C_ATTENTION : done ? C_DONE : failed ? C_ATTENTION : C_WORKING), 0);
  set_text(hint_label, rec ? L("tap when done · swipe: cancel", "тап — готово · свайп — скасувати") : failed ? L("tap: close", "тап: закрити") : "");
  if (done) {  // the result plays out in dictate_tick
    set_text(text_label, "");
    set_text(state_label, L("SENDING", "НАДСИЛАЮ"));
    lv_obj_set_style_text_color(state_label, lv_color_hex(C_WORKING), 0);
    lv_obj_set_style_translate_y(text_label, 0, 0);
    lv_obj_set_style_opa(text_label, LV_OPA_COVER, 0);
    lv_arc_set_value(sweep, 0);
    lv_obj_clear_flag(sweep, LV_OBJ_FLAG_HIDDEN);
    int chars = 0;
    for (size_t i = 0; i < text.length(); i++) chars += ((uint8_t)text[i] & 0xC0) != 0x80;
    type_ms = min((uint32_t)TYPE_MAX_MS, (uint32_t)chars * TYPE_MS_PER_CHAR);
  } else if (sweep) {
    lv_obj_add_flag(sweep, LV_OBJ_FLAG_HIDDEN);
  }
}

bool dictate_is_open() { return stage != CLOSED; }

void dictate_close()
{
  if (stage == RECORDING && !staged) dictation_stop(false);
  staged = false;
  stage = CLOSED;
  if (page) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void fail(const String &why)
{
  text = why;
  show_stage(FAILED);
  set_text(text_label, why);
  haptic_buzz(1);
}

void dictate_begin_cmd(const String &title, const String &cmd_json)
{
  dictate_begin(title, "");
  cmd_template = cmd_json;
}

void dictate_begin(const String &title, const String &agent)
{
  cmd_template = "";
  agent_id = agent;
  text = "";
  set_text(title_label, title);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  seen_version = shared.transcript_version;
  xSemaphoreGive(shared_mux);
  if (!staged && !dictation_start()) {
    fail("the microphone is busy");
    return;
  }
  started_at = millis();
  haptic_click();
  show_stage(RECORDING);
}

static lv_color_t eq_color(float x)  // x 0…1 along the gradient: magenta → blue → cyan
{
  const uint32_t stops[3] = {0xE040FB, 0x5B6CFF, 0x3DE0E0};
  int i = x < 0.5f ? 0 : 1;
  float t = (x - i * 0.5f) * 2;
  return lv_color_mix(lv_color_hex(stops[i + 1]), lv_color_hex(stops[i]), (lv_opa_t)(t * 255));
}

static inline void eq_dot(int x, int y, lv_color_t c)  // a 3 × 3 px brush
{
  for (int j = y - 1; j <= y + 1; j++) {
    if (j < 0 || j >= 360) continue;
    for (int i = x - 1; i <= x + 1; i++) {
      if (i >= 0 && i < 360) eq_buf[j * 360 + i] = c;
    }
  }
}

static void staged_bands(float *out)  // a voice-like spectrum: ~4 syllables a second, formant bumps in the low bands
{
  float t = (millis() - started_at) / 1000.0f;
  float syllable = 0.25f + 0.75f * fabsf(sinf(t * M_PI * 4.2f)) * (0.65f + 0.35f * sinf(t * 1.7f + 1));
  if (t < 0.3f) syllable *= t / 0.3f;
  for (int b = 0; b < EQ_BARS / 2; b++) {
    float formants = expf(-b / 14.0f) + 0.45f * expf(-(b - 7) * (b - 7) / 8.0f) + 0.3f * expf(-(b - 15) * (b - 15) / 10.0f);
    float jitter = 0.75f + 0.25f * sinf(b * 2.1f + t * 23) * sinf(b * 0.7f + t * 7);
    out[b] = fminf(1.0f, 0.95f * syllable * formants * jitter);
  }
}

static void draw_eq()
{
  static uint32_t last = 0;
  if (!eq || millis() - last < 30) return;
  last = millis();
  float now[EQ_BARS / 2];
  if (staged) staged_bands(now);
  else dictation_bands(now);
  for (int b = 0; b < EQ_BARS / 2; b++) eq_shown[b] = fmaxf(now[b], eq_shown[b] * 0.82f);  // fast up, slow down
  memset(eq_buf, 0, 360 * 360 * sizeof(lv_color_t));  // C_BG is black
  lv_color_t white = lv_color_hex(0xFFFFFF);
  for (int i = 0; i < EQ_BARS; i++) {
    int band = i < EQ_BARS / 2 ? i : EQ_BARS - 1 - i;  // mirrored: both sides climb from the top to the bottom
    float a = (i + 0.5f) * 2 * M_PI / EQ_BARS - M_PI / 2;
    float ca = cosf(a), sa = sinf(a);
    int len = 2 + (int)(eq_shown[band] * EQ_LEN);
    lv_color_t c = eq_color((float)band / (EQ_BARS / 2 - 1));
    for (int r = 0; r < len; r++) eq_dot(180 + (EQ_R0 + r) * ca, 180 + (EQ_R0 + r) * sa, r >= len - 3 && len > 6 ? white : c);
  }
  lv_obj_invalidate(eq);
}

void dictate_tick()
{
  if (stage == RECORDING) {
    uint32_t held = millis() - started_at;
    if (!staged && !dictation_recording()) {  // the 60 s limit ended it: transcribe what there is
      sent_at = millis();
      show_stage(TRANSCRIBING);
      return;
    }
    draw_eq();
    char t[12];
    snprintf(t, sizeof(t), "%u:%02u", (unsigned)(held / 60000), (unsigned)(held / 1000 % 60));
    set_text(time_label, t);
  } else if (stage == TRANSCRIBING && staged) {
    if (millis() - sent_at >= STAGED_TRANSCRIBE_MS) {
      staged = false;
      text = staged_text;
      haptic_click(2);
      show_stage(RESULT);
      sent_at = millis();
    }
  } else if (stage == TRANSCRIBING) {
    xSemaphoreTake(shared_mux, portMAX_DELAY);
    bool fresh = shared.transcript_version != seen_version;
    String got = shared.transcript, error = shared.transcript_error;
    seen_version = shared.transcript_version;
    xSemaphoreGive(shared_mux);
    if (fresh) {
      got.trim();
      if (error.length()) fail(error);
      else if (!got.length()) fail("heard nothing");
      else {  // straight to the agent: no confirming tap (user, 2026-11-14)
        text = got;
        JsonDocument cmd;
        if (cmd_template.length() && !deserializeJson(cmd, cmd_template)) {
          cmd["text"] = text;  // a feed app's command: the app's bridge side decides what the words do
        } else {
          cmd["cmd"] = "say";
          cmd["id"] = agent_id;
          cmd["text"] = text;
        }
        push_command(cmd);
        haptic_click(2);
        show_stage(RESULT);
        sent_at = millis();
      }
    } else if (millis() - sent_at > TRANSCRIBE_WAIT_MS) {
      fail("no answer from the Mac");
    }
  } else if (stage == RESULT) {
    uint32_t t = millis() - sent_at;
    // 1. the transcript types itself in, a UTF-8 character at a time
    if (t <= type_ms + 33) {
      int chars = type_ms ? (int)(t * 1.0f / type_ms * 10000) : 10000, total = 0, cut = text.length();
      for (size_t i = 0; i < text.length(); i++) total += ((uint8_t)text[i] & 0xC0) != 0x80;
      int want = total * min(chars, 10000) / 10000, seen = 0;
      for (size_t i = 0; i < text.length(); i++) {
        if (((uint8_t)text[i] & 0xC0) != 0x80 && seen++ == want) {
          cut = i;
          break;
        }
      }
      set_text(text_label, text.substring(0, cut));
    }
    // 2. the ring closes round it; full: SENT, a click
    uint32_t s0 = type_ms;
    if (t >= s0 && t < s0 + SWEEP_MS + 50) {
      float f = fminf(1.0f, (t - s0) / (float)SWEEP_MS);
      lv_arc_set_value(sweep, (int)(1000 * (1 - (1 - f) * (1 - f))));  // ease out
      if (f >= 1.0f && strcmp(lv_label_get_text(state_label), L("SENT", "НАДІСЛАНО"))) {
        set_text(state_label, L("SENT", "НАДІСЛАНО"));
        lv_obj_set_style_text_color(state_label, lv_color_hex(C_DONE), 0);
        haptic_click();
      }
    }
    // 3. held a moment, then the text rises away and fades with the ring
    uint32_t r0 = s0 + SWEEP_MS + HOLD_MS;
    if (t >= r0) {
      float f = fminf(1.0f, (t - r0) / (float)RISE_MS);
      lv_obj_set_style_translate_y(text_label, -(int)(90 * f * f), 0);
      lv_obj_set_style_opa(text_label, (lv_opa_t)(255 * (1 - f)), 0);
      lv_obj_set_style_arc_opa(sweep, (lv_opa_t)(255 * (1 - f)), LV_PART_INDICATOR);
    }
    if (t >= r0 + RISE_MS) {  // gone: back, as after Send (the text already flew: none for the main page)
      lv_obj_set_style_arc_opa(sweep, LV_OPA_COVER, LV_PART_INDICATOR);
      String agent = agent_id;
      bool app = cmd_template.length() > 0;
      dictate_close();
      if (!app) ui_after_send(agent);  // a feed app stays on screen: its note says what happened
    }
  }
}

// Tests: the result's play-out with `t` as the transcript — nothing is sent (no agent, no `say`).
void dictate_demo(const String &t)
{
  if (t.startsWith("listen ")) {  // staged: the next hold plays the whole dictation (see `staged`)
    staged = true;
    staged_text = t.substring(7);
    return;
  }
  agent_id = "";
  text = t;
  set_text(title_label, "demo");
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  show_stage(RESULT);
  sent_at = millis();
}

void dictate_tap()
{
  if (stage == RECORDING) {  // done speaking
    if (!staged) dictation_stop(true);
    sent_at = millis();
    haptic_click();
    show_stage(TRANSCRIBING);
  } else if (stage == RESULT) {  // already sent: a tap just goes back sooner (the text flies off the main page)
    String agent = agent_id;
    bool app = cmd_template.length() > 0;
    dictate_close();
    if (!app) ui_after_send(agent, text);
  } else if (stage == FAILED) {
    dictate_close();
  }
}

void dictate_swipe()
{
  if (stage == RECORDING || stage == RESULT || stage == FAILED) {
    haptic_click();
    dictate_close();
  }
}

String dictate_report()
{
  static const char *NAMES[] = {"closed", "recording", "transcribing", "result", "failed"};
  return String("dictate=") + NAMES[stage] + " level=" + String(dictation_level()) + " text=\"" + text.substring(0, 80) + "\"";
}

void dictate_create()
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

  ring = lv_arc_create(page);  // a full ring whose width and brightness follow the voice
  lv_obj_set_size(ring, 330, 330);
  lv_obj_center(ring);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_obj_remove_style(ring, NULL, LV_PART_INDICATOR);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(ring, lv_color_hex(C_ATTENTION), LV_PART_MAIN);

  eq_buf = (lv_color_t *)ps_malloc(360 * 360 * sizeof(lv_color_t));
  if (eq_buf) {
    memset(eq_buf, 0, 360 * 360 * sizeof(lv_color_t));
    eq = lv_canvas_create(page);
    lv_canvas_set_buffer(eq, eq_buf, 360, 360, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(eq);
    lv_obj_clear_flag(eq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(eq, LV_OBJ_FLAG_HIDDEN);
  }

  spinner = lv_spinner_create(page, 1100, 70);
  lv_obj_set_size(spinner, 90, 90);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 10);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(C_WORKING), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1C1D21), LV_PART_MAIN);

  sweep = lv_arc_create(page);  // the send's ring, round the transcript
  lv_obj_set_size(sweep, 330, 330);
  lv_obj_center(sweep);
  lv_arc_set_rotation(sweep, 270);
  lv_arc_set_bg_angles(sweep, 0, 360);
  lv_arc_set_range(sweep, 0, 1000);
  lv_obj_remove_style(sweep, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(sweep, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(sweep, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(sweep, lv_color_hex(0x15161A), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sweep, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(sweep, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sweep, lv_color_hex(C_DONE), LV_PART_INDICATOR);
  lv_obj_add_flag(sweep, LV_OBJ_FLAG_HIDDEN);

  state_label = make_label(page, &deck_bold_16, C_ATTENTION, -120, 220);
  lv_obj_set_style_text_letter_space(state_label, 2, 0);
  title_label = make_label(page, &deck_16, C_MUTED, -94, 230);
  time_label = make_label(page, &lv_font_montserrat_48, C_INK, 4, 200);

  text_label = lv_label_create(page);  // the transcript, up to ~6 lines
  lv_obj_set_width(text_label, 250);
  lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(text_label, &deck_16, 0);
  lv_obj_set_style_text_color(text_label, lv_color_hex(C_INK), 0);
  lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_max_height(text_label, 150, 0);
  lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 8);
  lv_label_set_text(text_label, "");

  hint_label = make_label(page, &deck_14, C_MUTED, 120, 200);
}

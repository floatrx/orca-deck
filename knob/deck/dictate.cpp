// Dictation in orca-deck (user, 2026-11-14): a hold on an agent starts it, then the finger is free — speak, and tap
// when done («тап на початок і тап коли диктовку закінчено»; a plain tap cannot start it: in orca-deck it is
// `enter`). The relay has the audio transcribed (OpenAI gpt-4o-transcribe); the text comes back for a look — tap
// sends it to the agent (`say`), a swipe drops it. The send always waits for that tap: the sessions run with
// bypassed permissions.
// .work/notes/knob-dictation.md
#include "deck.h"
#include <math.h>

#define TRANSCRIBE_WAIT_MS 30000

enum Stage { CLOSED, RECORDING, TRANSCRIBING, RESULT, FAILED };

static lv_obj_t *page, *ring, *spinner, *title_label, *state_label, *time_label, *text_label, *hint_label;
static Stage stage = CLOSED;
static String agent_id, text;
static uint32_t started_at = 0, sent_at = 0, seen_version = 0;

static void show_stage(Stage next)
{
  stage = next;
  bool rec = next == RECORDING, wait = next == TRANSCRIBING, done = next == RESULT, failed = next == FAILED;
  rec ? lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  wait ? lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  rec ? lv_obj_clear_flag(time_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(time_label, LV_OBJ_FLAG_HIDDEN);
  done || failed ? lv_obj_clear_flag(text_label, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(text_label, LV_OBJ_FLAG_HIDDEN);
  set_text(state_label, rec ? "LISTENING" : wait ? "TRANSCRIBING" : done ? "SEND THIS?" : failed ? "NOT SENT" : "");
  lv_obj_set_style_text_color(state_label, lv_color_hex(rec ? C_ATTENTION : done ? C_DONE : failed ? C_ATTENTION : C_WORKING), 0);
  set_text(hint_label, rec ? "tap when done · swipe: cancel" : done ? "tap: send · swipe: drop" : failed ? "tap: close" : "");
  if (done) set_text(text_label, text);
}

bool dictate_is_open() { return stage != CLOSED; }

void dictate_close()
{
  if (stage == RECORDING) dictation_stop(false);
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

void dictate_begin(const String &title, const String &agent)
{
  agent_id = agent;
  text = "";
  set_text(title_label, title);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(page);
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  seen_version = shared.transcript_version;
  xSemaphoreGive(shared_mux);
  if (!dictation_start()) {
    fail("the microphone is busy");
    return;
  }
  started_at = millis();
  haptic_click();
  show_stage(RECORDING);
}

void dictate_tick()
{
  if (stage == RECORDING) {
    uint32_t held = millis() - started_at;
    if (!dictation_recording()) {  // the 60 s limit ended it: transcribe what there is
      sent_at = millis();
      show_stage(TRANSCRIBING);
      return;
    }
    // The ring breathes with the voice: −60 dBFS (silence) … −10 dBFS (loud).
    int db = dictation_level();
    float v = fmaxf(0, fminf(1, (db + 60) / 50.0f));
    lv_obj_set_style_arc_width(ring, 4 + (int)(v * 14), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, (lv_opa_t)(90 + v * 165), LV_PART_MAIN);
    char t[12];
    snprintf(t, sizeof(t), "%u:%02u", (unsigned)(held / 60000), (unsigned)(held / 1000 % 60));
    set_text(time_label, t);
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
      else {
        text = got;
        haptic_click(2);
        show_stage(RESULT);
      }
    } else if (millis() - sent_at > TRANSCRIBE_WAIT_MS) {
      fail("no answer from the Mac");
    }
  }
}

void dictate_tap()
{
  if (stage == RECORDING) {  // done speaking
    dictation_stop(true);
    sent_at = millis();
    haptic_click();
    show_stage(TRANSCRIBING);
  } else if (stage == RESULT) {
    JsonDocument cmd;
    cmd["cmd"] = "say";
    cmd["id"] = agent_id;
    cmd["text"] = text;
    push_command(cmd);
    haptic_click(2);
    String agent = agent_id;
    dictate_close();
    ui_after_send(agent);
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

  spinner = lv_spinner_create(page, 1100, 70);
  lv_obj_set_size(spinner, 90, 90);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 10);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(C_WORKING), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1C1D21), LV_PART_MAIN);

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

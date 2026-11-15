// The demo recorder (2026-11-15, for the knob tour video: «мають бути такі ж анімації як в шайбі»): the UI's real
// animations captured at an exact 30 fps, however slow the USB is. While recording, time stops: the LVGL tick and
// deck_millis() (every millis() in the deck's code, see deck.h) advance by one frame per captured frame, and the
// LVGL port task renders and sends each frame before it advances again.
//   T rec <seconds> [<t>:<verb args>|<t>:<verb args>…]   e.g. "rec 6 1.0:turn 1|2.0:turn 1|3.5:tap"
// The stream (knob/test/rec.py): REC <w> <h> <fps> <frames> · FRM <i> <base64 RLE> · EVT <ms> <kind> · REC END.
// RLE: little-endian u16 pairs (run length, RGB565 big-endian as the snapshot holds it). Haptic events are logged
// with their frame time, so the edit can put a click sound / a vibration ring on the exact frame.
#include "deck.h"
#include "mbedtls/base64.h"

#define REC_FPS 30
#define FRAME_MS (1000 / REC_FPS)

static volatile bool recording = false;
static uint32_t base_ms = 0, virtual_ms = 0, frames_left = 0, frame_no = 0, total = 0;
static std::vector<std::pair<uint32_t, String>> events;  // (virtual ms, "verb args"), in order
static uint8_t *snap_buf, *rle_buf;
static unsigned char *b64_buf;
static uint32_t snap_size;

extern "C" bool ui_recording() { return recording; }

uint32_t deck_millis() { return recording ? base_ms + virtual_ms : (millis)(); }

void rec_event(const char *kind)  // haptics.cpp: a felt event, with its frame time
{
  if (recording) Serial.printf("EVT %u %s\n", (unsigned)virtual_ms, kind);
}

void rec_start(const String &arg)
{
  float seconds = arg.toFloat();
  if (seconds <= 0 || seconds > 30) {
    Serial.println("REC failed: rec <seconds 0..30> [t:verb|…]");
    return;
  }
  events.clear();
  int space = arg.indexOf(' ');
  String script = space > 0 ? arg.substring(space + 1) : "";
  while (script.length()) {  // "1.0:turn 1|2.0:tap"
    int bar = script.indexOf('|');
    String one = bar < 0 ? script : script.substring(0, bar);
    script = bar < 0 ? "" : script.substring(bar + 1);
    int colon = one.indexOf(':');
    if (colon > 0) events.push_back({(uint32_t)(one.substring(0, colon).toFloat() * 1000), one.substring(colon + 1)});
  }
  lv_obj_t *scr = lv_scr_act();
  snap_size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
  if (!snap_buf) snap_buf = (uint8_t *)ps_malloc(snap_size);
  if (!rle_buf) rle_buf = (uint8_t *)ps_malloc(snap_size * 2 + 16);
  if (!b64_buf) b64_buf = (unsigned char *)ps_malloc(snap_size * 3 + 64);
  if (!snap_buf || !rle_buf || !b64_buf) {
    Serial.println("REC failed: no memory");
    return;
  }
  total = frames_left = (uint32_t)(seconds * REC_FPS);
  frame_no = 0;
  base_ms = (millis)();
  virtual_ms = 0;
  Serial.setTxTimeoutMs(500);  // the reader is there: wait for it
  Serial.printf("REC %d %d %d %u\n", (int)lv_obj_get_width(scr), (int)lv_obj_get_height(scr), REC_FPS, (unsigned)total);
  recording = true;
}

// The LVGL port task, with the LVGL lock held, instead of its normal pass while recording: run the events due,
// one frame of the UI, then capture and send it.
extern "C" void ui_record_step()
{
  while (!events.empty() && events.front().first <= virtual_ms) {
    ui_test_run_now(events.front().second);
    events.erase(events.begin());
  }
  lv_tick_inc(FRAME_MS);
  virtual_ms += FRAME_MS;
  lv_timer_handler();
  lv_img_dsc_t dsc;
  if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &dsc, snap_buf, snap_size) == LV_RES_OK) {
    const uint16_t *px = (const uint16_t *)snap_buf;
    uint32_t n = dsc.data_size / 2, out = 0;
    for (uint32_t i = 0; i < n;) {
      uint16_t c = px[i], run = 1;
      while (i + run < n && px[i + run] == c && run < 0xFFFF) run++;
      memcpy(rle_buf + out, &run, 2);
      memcpy(rle_buf + out + 2, &c, 2);
      out += 4;
      i += run;
    }
    size_t written = 0;
    mbedtls_base64_encode(b64_buf, snap_size * 3 + 64, &written, rle_buf, out);
    b64_buf[written] = 0;
    Serial.printf("FRM %u ", (unsigned)frame_no);
    Serial.write(b64_buf, written);
    Serial.write('\n');
  }
  frame_no++;
  if (--frames_left == 0) {
    recording = false;
    Serial.println("REC END");
    Serial.flush();
    Serial.setTxTimeoutMs(0);
  }
}

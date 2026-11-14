// The PDM microphone (MSM261D4030H1CPM: clock GPIO45, data GPIO46, L/R strapped low). The S3 receives PDM only on
// I2S0 (sound.c plays on I2S1). Two uses:
//   - dictation: while the finger is held, 100 ms blocks are cleaned (DC removed, +18 dB), IMA-ADPCM encoded (4 bit,
//     8 KB/s) and streamed to the relay as `V` lines, which transcribes them (.work/notes/knob-dictation.md);
//   - the spike, `T mic <seconds>`: PSRAM recording, dBFS per 100 ms, the samples as `MIC <base64>` lines.
#include "deck.h"
#include <math.h>
#include "driver/i2s_pdm.h"
#include "mbedtls/base64.h"

#define MIC_CLK GPIO_NUM_45
#define MIC_DATA GPIO_NUM_46
#define RATE 16000
#define MAX_SECONDS 30

#define BLOCK 1600        // 100 ms
#define GAIN 8            // +18 dB: speech at 20–30 cm reads −40 dBFS raw
#define DICTATE_MAX_S 60

static i2s_chan_handle_t rx = nullptr;
static int16_t *samples = nullptr;
static volatile bool busy = false;

static bool mic_open()
{
  if (rx) return true;
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan, nullptr, &rx) != ESP_OK) return false;
  i2s_pdm_rx_config_t cfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(RATE),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {.clk = MIC_CLK, .din = MIC_DATA, .invert_flags = {.clk_inv = false}},
  };
  if (i2s_channel_init_pdm_rx_mode(rx, &cfg) != ESP_OK) {
    i2s_del_channel(rx);
    rx = nullptr;
    return false;
  }
  return true;
}

static void dump(int n)
{
  // 3 × 1024 bytes of samples per line → 4096 base64 characters.
  static unsigned char b64[4200];
  const uint8_t *bytes = (const uint8_t *)samples;
  size_t total = n * 2;
  Serial.printf("MIC BEGIN rate=%d samples=%d\n", RATE, n);
  for (size_t at = 0; at < total; at += 3072) {
    size_t len = min((size_t)3072, total - at), out = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &out, bytes + at, len);
    b64[out] = 0;
    Serial.printf("MIC %s\n", (const char *)b64);
    vTaskDelay(1);  // let the USB FIFO drain
  }
  Serial.println("MIC END");
}

static void record_task(void *arg)
{
  int seconds = (int)(intptr_t)arg;
  int n = RATE * seconds;
  if (!mic_open()) {
    Serial.println("mic: cannot open I2S0 PDM RX");
  } else {
    i2s_channel_enable(rx);
    // The first ~100 ms after enabling is the mic's start-up thump: read it and drop it.
    static int16_t junk[1600];
    size_t got = 0;
    i2s_channel_read(rx, junk, sizeof(junk), &got, 1000);
    size_t at = 0;
    while (at < (size_t)n) {
      size_t want = min((size_t)1600, n - at) * 2;
      if (i2s_channel_read(rx, samples + at, want, &got, 1000) != ESP_OK || !got) break;
      at += got / 2;
    }
    i2s_channel_disable(rx);
    // Levels: DC offset, then RMS per 100 ms in dBFS, and the peak.
    double sum = 0;
    for (size_t i = 0; i < at; i++) sum += samples[i];
    double dc = at ? sum / at : 0;
    int peak = 0;
    String line = "mic: dbfs/100ms";
    for (size_t w = 0; w + 1600 <= at; w += 1600) {
      double acc = 0;
      for (size_t i = w; i < w + 1600; i++) {
        double v = samples[i] - dc;
        acc += v * v;
        peak = max(peak, (int)fabs(v));
      }
      double rms = sqrt(acc / 1600);
      line += " " + String(rms > 0 ? (int)lround(20 * log10(rms / 32768)) : -99);
    }
    Serial.printf("mic: %u samples, dc %.0f, peak %d (%d dBFS)\n", (unsigned)at, dc, peak,
                  peak ? (int)lround(20 * log10(peak / 32768.0)) : -99);
    Serial.println(line);
    dump(at);
  }
  busy = false;
  vTaskDelete(nullptr);
}

void mic_test(int seconds)
{
  if (busy) return;
  seconds = max(1, min(MAX_SECONDS, seconds));
  if (!samples) samples = (int16_t *)ps_malloc(RATE * MAX_SECONDS * 2);
  if (!samples) {
    Serial.println("mic: no PSRAM for the buffer");
    return;
  }
  busy = true;
  Serial.printf("mic: recording %d s\n", seconds);
  // An internal-RAM stack: the default for xTaskCreate (task stacks are internal unless asked otherwise).
  xTaskCreatePinnedToCore(record_task, "mic", 6144, (void *)(intptr_t)seconds, 5, nullptr, 0);
}

// ---- dictation ----

static volatile bool dictating = false;
static volatile bool send_on_stop = false;  // how the task ends the stream after its last block: end or cancel
static volatile int level_db = -90;  // the last block's RMS, dBFS (after gain): the UI's meter
static TaskHandle_t dictate_handle = nullptr;

// IMA ADPCM (the classic DVI table): 16-bit samples → 4-bit codes, two per byte, low nibble first.
static const int16_t STEPS[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t INDEX_SHIFT[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Adpcm {
  int predictor = 0, index = 0;
  uint8_t encode(int sample)
  {
    int step = STEPS[index], diff = sample - predictor;
    uint8_t code = 0;
    if (diff < 0) code = 8, diff = -diff;
    int delta = step >> 3;
    if (diff >= step) code |= 4, diff -= step, delta += step;
    step >>= 1;
    if (diff >= step) code |= 2, diff -= step, delta += step;
    step >>= 1;
    if (diff >= step) code |= 1, delta += step;
    predictor += (code & 8) ? -delta : delta;
    predictor = max(-32768, min(32767, predictor));
    index = max(0, min(88, index + INDEX_SHIFT[code]));
    return code;
  }
};

static void dictate_task(void *)
{
  static int16_t block[BLOCK];
  static uint8_t packed[BLOCK / 2];
  static unsigned char b64[BLOCK];
  Adpcm adpcm;
  float dc = 0;
  bool first = true;
  uint32_t until = millis() + DICTATE_MAX_S * 1000;
  push_voice("V {\"start\":true}");
  i2s_channel_enable(rx);
  size_t got = 0;
  i2s_channel_read(rx, block, sizeof(block), &got, 1000);  // the start-up thump
  while (dictating && millis() < until) {
    if (i2s_channel_read(rx, block, sizeof(block), &got, 1000) != ESP_OK || got < sizeof(block)) break;
    if (first) {  // start the DC estimate where the signal is, not at 0
      long sum = 0;
      for (int i = 0; i < BLOCK; i++) sum += block[i];
      dc = sum / (float)BLOCK;
      first = false;
    }
    double acc = 0;
    for (int i = 0; i < BLOCK; i += 2) {
      int s[2];
      for (int k = 0; k < 2; k++) {
        dc += (block[i + k] - dc) * 0.001f;  // a slow high-pass: the mic's offset drifts a little
        int v = (int)((block[i + k] - dc) * GAIN);
        v = max(-32768, min(32767, v));
        acc += (double)v * v;
        s[k] = adpcm.encode(v);
      }
      packed[i / 2] = (uint8_t)(s[0] | (s[1] << 4));
    }
    double rms = sqrt(acc / BLOCK);
    level_db = rms > 0 ? (int)lround(20 * log10(rms / 32768)) : -90;
    size_t out = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &out, packed, sizeof(packed));
    b64[out] = 0;
    push_voice(String("V ") + (const char *)b64);
  }
  i2s_channel_disable(rx);
  // After the last block, so the relay never gets audio past the end: the time limit sends what there is.
  bool limit = dictating;
  dictating = false;
  push_voice(limit || send_on_stop ? "V {\"end\":true}" : "V {\"cancel\":true}");
  dictate_handle = nullptr;
  vTaskDelete(nullptr);
}

bool dictation_start()
{
  if (dictating || dictate_handle || busy) return false;
  if (!mic_open()) return false;
  dictating = true;
  level_db = -90;
  xTaskCreatePinnedToCore(dictate_task, "dictate", 8192, nullptr, 5, &dictate_handle, 0);
  return true;
}

// The finger left (send = true) or the user cancelled. The relay transcribes on `end`.
void dictation_stop(bool send)
{
  if (!dictating) return;
  send_on_stop = send;
  dictating = false;  // the task finishes its block and ends the stream
}

bool dictation_recording() { return dictate_handle != nullptr; }

int dictation_level() { return level_db; }

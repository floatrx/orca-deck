// The PDM microphone (MSM261D4030H1CPM: clock GPIO45, data GPIO46, L/R strapped low), the first step toward
// push-to-talk dictation (.work/notes/knob-dictation.md). Spike: `T mic <seconds>` records into PSRAM, prints the
// level per 100 ms and the peak, then streams the samples over USB as `MIC <base64>` lines (16 kHz, 16-bit mono)
// so the Mac can rebuild a WAV. The S3 receives PDM only on I2S0 (sound.c plays on I2S1).
#include "deck.h"
#include <math.h>
#include "driver/i2s_pdm.h"
#include "mbedtls/base64.h"

#define MIC_CLK GPIO_NUM_45
#define MIC_DATA GPIO_NUM_46
#define RATE 16000
#define MAX_SECONDS 30

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

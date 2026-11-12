// The spike's sounds (.work/spike/orca-deck/index.html `tone()`), synthesised on the knob: an oscillator with a
// 4 ms attack and an exponential release, written to the PCM5100A DAC over I2S. Pins and slot setup come from
// the Waveshare demo `07_Audio_Test`.
#include "sound.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "sound";

#define SAMPLE_RATE 44100
#define PIN_BCLK GPIO_NUM_39
#define PIN_WS GPIO_NUM_40
#define PIN_DOUT GPIO_NUM_41
#define PIN_AUDIO_MUX GPIO_NUM_0  // CH445P select: high routes the DAC path to the S3
#define CHUNK 256

typedef enum { WAVE_SINE, WAVE_TRIANGLE } wave_t;

typedef struct {
  float freq, delay, length, peak;
  wave_t wave;
} tone_t;

static i2s_chan_handle_t tx;
static QueueHandle_t queue;
static volatile uint8_t level = 2;

static float level_gain(void)
{
  return level == 0 ? 0.0f : level == 1 ? 0.35f : 1.0f;
}

static float wave_at(wave_t wave, float phase)
{
  if (wave == WAVE_TRIANGLE) return 4.0f * fabsf(phase - floorf(phase + 0.5f)) - 1.0f;
  return sinf(2.0f * (float)M_PI * phase);
}

// Renders one tone into I2S, chunk by chunk, so a long hum never needs a big buffer.
static void play(const tone_t *t)
{
  static int16_t buf[CHUNK];
  size_t written;
  int silence = (int)(t->delay * SAMPLE_RATE);
  memset(buf, 0, sizeof(buf));
  while (silence > 0) {
    int n = silence > CHUNK ? CHUNK : silence;
    i2s_channel_write(tx, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
    silence -= n;
  }
  int total = (int)((t->length + 0.02f) * SAMPLE_RATE);
  int attack = (int)(0.004f * SAMPLE_RATE);
  // Exponential ramp from `peak` down to 0.0001 over the tone, as Web Audio's exponentialRampToValueAtTime.
  float decay = powf(0.0001f / t->peak, 1.0f / (t->length * SAMPLE_RATE));
  float gain = t->peak, phase = 0, step = t->freq / SAMPLE_RATE, scale = 32767.0f * level_gain();
  for (int done = 0; done < total;) {
    int n = total - done > CHUNK ? CHUNK : total - done;
    for (int i = 0; i < n; i++, done++) {
      float env = done < attack ? t->peak * done / attack : (gain *= decay);
      buf[i] = (int16_t)(wave_at(t->wave, phase) * env * scale);
      phase += step;
      if (phase >= 1.0f) phase -= 1.0f;
    }
    i2s_channel_write(tx, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
  }
}

static void sound_task(void *arg)
{
  tone_t t;
  for (;;) {
    if (xQueueReceive(queue, &t, portMAX_DELAY) == pdTRUE) play(&t);
  }
}

static void enqueue(float freq, float delay, float length, float peak, wave_t wave)
{
  if (!queue || level == 0) return;
  tone_t t = {freq, delay, length, peak, wave};
  xQueueSend(queue, &t, 0);  // a full queue drops the sound rather than block the UI
}

void sound_init(void)
{
  gpio_config_t mux = {
    .pin_bit_mask = 1ULL << PIN_AUDIO_MUX,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&mux);
  gpio_set_level(PIN_AUDIO_MUX, 1);

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // silence, not the last buffer, when the queue runs dry
  esp_err_t err = i2s_new_channel(&chan, &tx, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
    return;
  }
  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = PIN_BCLK,
      .ws = PIN_WS,
      .dout = PIN_DOUT,
      .din = I2S_GPIO_UNUSED,
    },
  };
  err = i2s_channel_init_std_mode(tx, &std);
  if (err == ESP_OK) err = i2s_channel_enable(tx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s init: %s", esp_err_to_name(err));
    return;
  }
  queue = xQueueCreate(8, sizeof(tone_t));
  xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 3, NULL, 1);
}

// A loud, long test tone: easy to hear and to find in a recording.
void sound_test(void) { enqueue(1000, 0, 1.0f, 1.0f, WAVE_SINE); }

int sound_ready(void) { return queue != NULL; }

void sound_set_level(uint8_t value) { level = value > 2 ? 2 : value; }

// Same parameters as the spike: tick = tone(2200, 0, 0.018, 0.05, "triangle"); buzz = tone(140, k·0.5, 0.32, 0.25).
// The spike's peaks are Web Audio gains for a laptop; the knob speaker is small, so they are raised.
void sound_tick(void) { enqueue(2200, 0, 0.018f, 0.25f, WAVE_TRIANGLE); }
void sound_click(void) { enqueue(1400, 0, 0.03f, 0.35f, WAVE_TRIANGLE); }
void sound_buzz(int times)
{
  for (int k = 0; k < times; k++) enqueue(140, k ? 0.18f : 0, 0.32f, 0.6f, WAVE_SINE);
}

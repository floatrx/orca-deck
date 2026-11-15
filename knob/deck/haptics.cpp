// Haptics (DRV2605 on the legacy I2C driver that Touch_Init installs; `Wire` aborts on core 3.x) and the
// matching sounds. Strength comes from Settings → Haptics: 0 off, 1–3 pick weaker library effects.
#include "deck.h"
#include "driver/i2c.h"
#include "sound.h"

#define DRV2605_ADDR 0x5A

// ERM/LRA library effects, strongest first: Sharp Tick 1/2/3, Strong Click 100/60/30 %, Buzz 1/2/3.
static const uint8_t TICKS[] = {24, 25, 26};
static const uint8_t CLICKS[] = {1, 2, 3};
static const uint8_t BUZZES[] = {47, 48, 49};

static uint32_t writes = 0, failures = 0, plays = 0;  // for `report`: a failed write is a silent missing buzz

static void drv_write(uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = {reg, val};
  writes++;
  if (i2c_master_write_to_device(I2C_NUM_0, DRV2605_ADDR, buf, 2, pdMS_TO_TICKS(20)) != ESP_OK) failures++;
}

uint32_t haptic_plays() { return plays; }

String haptic_report() { return "haptic level=" + String(settings.haptics) + " writes=" + String(writes) + " failed=" + String(failures); }

void haptic_init()
{
  drv_write(0x01, 0x00);  // MODE: out of standby, internal trigger
  drv_write(0x1A, 0xB6);  // FEEDBACK: LRA
  drv_write(0x03, 0x06);  // LIBRARY: LRA
  drv_write(0x1D, 0xA0);  // CONTROL3: closed-loop LRA defaults
}

// Plays `effect` `times` times, 250 ms apart (a waveform-sequencer wait between them).
static void play(const uint8_t *table, int times)
{
  plays++;  // counted even when Haptics is off: the hub asks "did this gesture answer?", not "did it buzz?"
  if (!settings.haptics) return;
  uint8_t effect = table[3 - settings.haptics];
  uint8_t reg = 0x04;
  for (int k = 0; k < times && reg < 0x0B; k++) {
    if (k) drv_write(reg++, 0x80 | 25);  // wait 25 × 10 ms
    drv_write(reg++, effect);
  }
  drv_write(reg, 0x00);
  drv_write(0x0C, 0x01);  // GO
}

void haptic_tick()
{
  rec_event("tick");
  play(TICKS, 1);
  sound_tick();
}

void haptic_click(int times)
{
  rec_event("click");
  play(CLICKS, times);
  sound_click();
}

void haptic_buzz(int times)
{
  rec_event("buzz");
  play(BUZZES, times);
  sound_buzz(times);
}

// Going home (swipe down, waking into the hub): a short smooth ramp down that lands on a click — the screen
// "drops into place". Its own shape, so it never reads as a tap or an alert (user, 2026-11-14: a plain click on
// swipe down went unnoticed).
void haptic_home()
{
  rec_event("home");
  plays++;
  if (settings.haptics) {
    drv_write(0x04, 74);  // Transition Ramp Down Short Smooth 1 (100 → 0 %)
    drv_write(0x05, CLICKS[3 - settings.haptics]);
    drv_write(0x06, 0x00);
    drv_write(0x0C, 0x01);  // GO
  }
  sound_click();
}

void haptic_sample() { haptic_click(1); }

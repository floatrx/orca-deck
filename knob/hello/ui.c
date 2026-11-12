// First firmware for the knob: a ring that follows the crown, a haptic tick per detent,
// a tap on the screen cycles the accent colour.
#include "lvgl.h"
#include "driver/i2c.h"
#include "bidi_switch_knob.h"

#define KNOB_PIN_A 8
#define KNOB_PIN_B 7

// DRV2605 haptic driver on the touch I2C bus (port 0, installed by Touch_Init).
#define DRV2605_ADDR 0x5A
#define HAPTIC_TICK 24 // "Sharp Tick 1" from the ERM/LRA effect library

static volatile int pending_steps = 0;
static int value = 50;
static int accent = 0;
static const uint32_t ACCENTS[] = {0x4FC3F7, 0x66BB6A, 0xFFB74D, 0xF06292};

static lv_obj_t *arc;
static lv_obj_t *number;
static lv_obj_t *caption;

static void drv_write(uint8_t reg, uint8_t val)
{
  uint8_t buf[2] = {reg, val};
  i2c_master_write_to_device(I2C_NUM_0, DRV2605_ADDR, buf, 2, pdMS_TO_TICKS(20));
}

static void haptic_init(void)
{
  drv_write(0x01, 0x00);  // MODE: out of standby, internal trigger
  drv_write(0x1A, 0xB6);  // FEEDBACK: LRA
  drv_write(0x03, 0x06);  // LIBRARY: LRA
  drv_write(0x1D, 0xA0);  // CONTROL3: closed-loop LRA defaults
}

static void haptic_tick(void)
{
  drv_write(0x04, HAPTIC_TICK);
  drv_write(0x05, 0x00);
  drv_write(0x0C, 0x01);  // GO
}

static void knob_left(void *arg, void *data) { pending_steps--; }
static void knob_right(void *arg, void *data) { pending_steps++; }

static void apply_accent(void)
{
  lv_color_t c = lv_color_hex(ACCENTS[accent]);
  lv_obj_set_style_arc_color(arc, c, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(caption, c, 0);
}

// Runs in the LVGL task, so it may touch widgets without the mutex.
static void poll_knob(lv_timer_t *timer)
{
  int steps = pending_steps;
  if (steps == 0) return;
  pending_steps -= steps;
  value += steps;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  lv_arc_set_value(arc, value);
  lv_label_set_text_fmt(number, "%d", value);
  haptic_tick();
}

static void on_tap(lv_event_t *e)
{
  accent = (accent + 1) % (sizeof(ACCENTS) / sizeof(ACCENTS[0]));
  apply_accent();
  haptic_tick();
}

void ui_init(void)
{
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  arc = lv_arc_create(screen);
  lv_obj_set_size(arc, 330, 330);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, value);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x222222), LV_PART_MAIN);

  number = lv_label_create(screen);
  lv_obj_set_style_text_font(number, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(number, lv_color_white(), 0);
  lv_label_set_text_fmt(number, "%d", value);
  lv_obj_center(number);

  caption = lv_label_create(screen);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_20, 0);
  lv_label_set_text(caption, "orca-deck");
  lv_obj_align(caption, LV_ALIGN_CENTER, 0, 56);

  lv_obj_add_event_cb(screen, on_tap, LV_EVENT_CLICKED, NULL);
  apply_accent();

  haptic_init();
  knob_config_t cfg = {
    .gpio_encoder_a = KNOB_PIN_A,
    .gpio_encoder_b = KNOB_PIN_B,
  };
  knob_handle_t knob = iot_knob_create(&cfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right, NULL);
  }
  lv_timer_create(poll_knob, 16, NULL);
}

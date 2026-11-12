#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "deck.h"

void setup()
{
  Serial.setRxBufferSize(32768);  // a relay state line is several KB
  Serial.begin(115200);
  heap_caps_malloc_extmem_enable(256);  // Strings and JSON go to PSRAM: internal RAM is short with BLE on
  time(nullptr);  // creates newlib's time lock now: lazily, later, it needs internal RAM that BLE has taken
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  settings_apply();  // brightness and sound level from NVS
  Serial.printf("orca-deck knob: up · internal heap %u (largest %u) · psram %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop()
{
  delay(1000);
}

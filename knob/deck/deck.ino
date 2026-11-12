#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"

void setup()
{
  Serial.setRxBufferSize(32768);  // a relay state line is several KB
  Serial.begin(115200);
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  Serial.println("orca-deck knob: up");
}

void loop()
{
  delay(1000);
}

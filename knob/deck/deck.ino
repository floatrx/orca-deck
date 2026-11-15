#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "deck.h"
#include <esp_system.h>

// Why the knob restarted, for the Mac's log (2026-11-15: it rebooted over and over off USB, where nothing reads the
// serial port). RTC memory survives a soft reset, a panic, a watchdog and a brownout, not a power cut.
RTC_NOINIT_ATTR static uint32_t rtc_magic, rtc_boots, rtc_uptime_s;
// The slowest UI pass (lv_timer_handler) of this run and the app on screen then: a long one points at the hang.
RTC_NOINIT_ATTR static uint32_t rtc_slowest_ms;
RTC_NOINIT_ATTR static char rtc_slowest_where[16];

extern "C" void ui_frame_time(uint32_t ms)
{
  if (ms <= rtc_slowest_ms) return;
  rtc_slowest_ms = ms;
  const App *app = hub_current();
  strncpy(rtc_slowest_where, buddy_is_shown() ? "screensaver" : app ? app->name : "hub", sizeof(rtc_slowest_where) - 1);
  rtc_slowest_where[sizeof(rtc_slowest_where) - 1] = 0;
}
String boot_note;

static const char *reason_name(esp_reset_reason_t r)
{
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_EXT: return "reset pin";
    default: return "unknown";
  }
}

String ui_slowest_note() { return String(rtc_slowest_ms) + " ms in " + rtc_slowest_where; }

void setup()
{
  esp_reset_reason_t reason = esp_reset_reason();
  if (rtc_magic != 0x4B4E4F42 || reason == ESP_RST_POWERON) {
    rtc_magic = 0x4B4E4F42, rtc_boots = 0, rtc_uptime_s = 0, rtc_slowest_ms = 0;
    rtc_slowest_where[0] = 0;
  }
  uint32_t prev_up = rtc_uptime_s, prev_slowest = rtc_slowest_ms;
  String prev_where = rtc_slowest_where;
  rtc_slowest_ms = 0;
  rtc_boots++;
  rtc_uptime_s = 0;
  boot_note = "boot #" + String(rtc_boots) + ": " + reason_name(reason) +
              (rtc_boots > 1 ? " after " + String(prev_up) + " s up · slowest UI pass " + String(prev_slowest) + " ms in " +
                                   prev_where : "");
  Serial.setRxBufferSize(32768);  // a relay state line is several KB
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // no reader on the USB port (a charger, a closed relay): logs drop, nothing waits
  heap_caps_malloc_extmem_enable(256);  // Strings and JSON go to PSRAM: internal RAM is short with BLE on
  time(nullptr);  // creates newlib's time lock now: lazily, later, it needs internal RAM that BLE has taken
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  settings_apply();  // brightness and sound level from NVS
  Serial.println(boot_note);
  Serial.printf("orca-deck knob: up · internal heap %u (largest %u) · psram %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop()
{
  delay(1000);
  rtc_uptime_s = millis() / 1000;  // the last second alive, read by the next boot
}

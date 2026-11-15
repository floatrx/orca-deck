// The knob's own battery. Waveshare documents no battery pin for this board; GPIO1 (ADC1 ch0, its "system voltage"
// demo) reads the system rail through a /2 divider — measured 2026-11-15: ~4.90 V on USB (the USB rail: no % then,
// it charges), 4.17 -> 4.12 V on the battery. So: above 4.5 V = on USB; else the % from a Li-ion curve, smoothed.
// The relay's log still gets one line a minute over BLE ("L bat ...").
#include "deck.h"

#define BAT_PIN 1

int battery_pin_mv()
{
  static bool ready = false;
  if (!ready) {
    analogSetPinAttenuation(BAT_PIN, ADC_11db);  // up to ~3.1 V on the pin
    ready = true;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_PIN);
  return sum / 16;
}

String battery_report()
{
  int pin = battery_pin_mv();
  return "bat pin=" + String(pin) + " mV · x2=" + String(pin * 2) + " · x3=" + String(pin * 3) + " · up " +
         String(millis() / 1000) + " s · usb " + (link_alive(LINK_USB) ? "link" : "-");
}

// A typical Li-ion discharge curve (mV -> %), linear between the points.
static const int CURVE[][2] = {{4200, 100}, {4100, 90}, {4000, 80}, {3900, 70}, {3800, 58}, {3750, 50}, {3700, 40},
                               {3650, 28}, {3600, 18}, {3500, 8}, {3300, 0}};

static int percent_of(int millivolts)
{
  if (millivolts >= CURVE[0][0]) return 100;
  for (size_t i = 1; i < sizeof(CURVE) / sizeof(CURVE[0]); i++) {
    if (millivolts >= CURVE[i][0]) {
      int hi = CURVE[i - 1][0], lo = CURVE[i][0];
      return CURVE[i][1] + (CURVE[i - 1][1] - CURVE[i][1]) * (millivolts - lo) / (hi - lo);
    }
  }
  return 0;
}

static int level = -1;  // %, smoothed; -1: on USB or not read yet
static bool on_usb = false;
static float smooth = 0;

// LVGL task, every frame: a reading every 10 s (16 samples), smoothed so the % does not wobble with the load.
void battery_tick()
{
  static uint32_t last = 0;
  if (last && millis() - last < 10000) return;
  last = millis();
  int volts_mv = battery_pin_mv() * 2;
  bool usb = volts_mv > 4500;
  if (usb != on_usb || smooth == 0) smooth = volts_mv;  // plugged / unplugged: start over
  smooth = smooth * 0.7f + volts_mv * 0.3f;
  on_usb = usb;
  level = usb ? -1 : percent_of((int)smooth);
  static bool warned = false;  // under 15 %: one banner and a buzz; again only after it was above 20 %
  if (level >= 0 && level < 15 && !warned) {
    warned = true;
    banner_show_app(&app_settings, "knob battery " + String(level) + "%");
    haptic_buzz(2);
  } else if (level > 20 || usb) {
    warned = false;
  }
}

int battery_level() { return level; }
bool battery_charging() { return on_usb; }

// "92%" with a level symbol, or a bolt on USB: for Montserrat labels (the deck fonts have no symbols).
String battery_text()
{
  if (on_usb) return LV_SYMBOL_CHARGE " USB";
  if (level < 0) return "";
  const char *sym = level > 80 ? LV_SYMBOL_BATTERY_FULL : level > 55 ? LV_SYMBOL_BATTERY_3
                    : level > 30 ? LV_SYMBOL_BATTERY_2 : level > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  return String(sym) + " " + String(level) + "%";
}

// The knob's own battery — a probe first (2026-11-15): Waveshare documents no battery pin for this board; its
// ADC demo reads "system voltage" on ADC1 channel 0 = GPIO1, and the 1.28" sibling divides the battery by 3
// (200K/100K) onto GPIO1. Until a reading on and off USB proves what GPIO1 sees, report the pin's millivolts and the
// x2 / x3 candidates; the relay's log gets one line a minute over BLE ("L bat …").
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

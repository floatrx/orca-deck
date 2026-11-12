// For the board's second chip (ESP32-U4WDH): the knob does not use it, and awake it draws ~50 mA from the
// battery all the time. This puts it into deep sleep with no wake-up source: it stays asleep until reflashed
// (or reset by the USB-C on its side, which reflashing needs anyway).
#include <esp_sleep.h>

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("second chip: deep sleep (reflash to wake)");
  Serial.flush();
  esp_deep_sleep_start();
}

void loop() {}

// USB link: relay lines over the USB-Serial-JTAG port (`uv run knob/knob-bridge-relay.py`). For debugging at the desk.
#include "deck.h"

static SemaphoreHandle_t write_mux;

void serial_send_line(const String &line)
{
  String out = line + "\n";
  xSemaphoreTake(write_mux, portMAX_DELAY);
  Serial.write((const uint8_t *)out.c_str(), out.length());  // one write, so a log line cannot split it
  xSemaphoreGive(write_mux);
}

static void serial_task(void *)
{
  String line;
  for (;;) {
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\n') {
        handle_line(line, LINK_USB);
        line = "";
      } else if (ch != '\r') {
        line += ch;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void serial_link_start()
{
  write_mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCoreWithCaps(serial_task, "usb", 12 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// USB diagnostics (2026-11-15: the USB port goes silent after some test runs — no data, and an open no longer resets
// the chip — while BLE keeps working). `T usb`: the USB-Serial-JTAG registers, who owns the PHY, and who holds
// GPIO 19 / 20 (D− / D+) in the peripheral manager (a claim there runs HWCDC::deinit, which pulls both lines low).
// The tell of the dropout is a frozen sof (the host stopped its frames) with hwcdc=0; the CRC5 / bit-stuff bits in
// raw show up in healthy sessions too. The cause was a low pulse on GPIO0 (the CH445P select) in sound_init.
#include "deck.h"
#include "esp32-hal-periman.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"

String usb_report()
{
  char line[240];
  snprintf(line, sizeof(line), "usb conf0=%08lx sof=%lu in1=%08lx out1=%08lx ena=%08lx raw=%08lx rtcusb=%08lx pin19=%d pin20=%d hwcdc=%d up %lu s",
           (unsigned long)REG_READ(USB_SERIAL_JTAG_CONF0_REG), (unsigned long)(REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF),
           (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_OUT_EP1_ST_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_ENA_REG),
           (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_RAW_REG), (unsigned long)REG_READ(RTC_CNTL_USB_CONF_REG),
           (int)perimanGetPinBusType(19), (int)perimanGetPinBusType(20), (int)(bool)Serial, (unsigned long)(millis() / 1000));
  return line;
}

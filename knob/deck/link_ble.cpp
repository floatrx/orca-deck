// BLE link: relay lines over a Nordic UART service, to KnobRelay.app on the Mac (knob/mac).
// RX (the Mac writes S / K / D / A lines), TX (the knob notifies C lines). Lines are split into ATT packets
// and joined again on '\n'. Both characteristics need an encrypted link, so the Mac pairs and bonds first.
// Once a bond exists, a new central may pair only while pairing is open (Settings → Connection → Forget).
#include "deck.h"
#include <NimBLEDevice.h>

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define DEVICE_NAME "orca-deck-knob"

static NimBLEServer *server;
static NimBLECharacteristic *tx;
static SemaphoreHandle_t rx_mux;
static std::string rx_buffer;
static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool encrypted = false, subscribed = false;
static volatile uint32_t pairing_until = 0;  // a new central may pair before this time

// No `subscribed` here: for a bonded Mac, NimBLE restores the TX subscription from the bond without calling
// onSubscribe, so the flag stayed false after a knob reboot and every command was held back.
// notify() itself fails while nobody is subscribed, and the dispatcher retries.
bool ble_connected() { return conn_handle != BLE_HS_CONN_HANDLE_NONE && encrypted; }

bool ble_send_line(const String &line)
{
  if (!ble_connected()) return false;
  String out = line + "\n";
  size_t chunk = server->getPeerMTU(conn_handle);
  chunk = chunk > 3 ? chunk - 3 : 20;
  for (size_t at = 0; at < out.length(); at += chunk) {
    size_t n = min(chunk, out.length() - at);
    // A full buffer mid-line is waited out here: a retry of the whole line would repeat this line's first chunks
    // and corrupt it (dictation streams ~50 notifications a second).
    int tries = 0;
    while (!tx->notify((const uint8_t *)out.c_str() + at, n, conn_handle)) {
      if (++tries > 40 || !ble_connected()) return at == 0 ? false : true;  // mid-line: the line is lost, not doubled
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  return true;
}

void ble_forget()
{
  NimBLEDevice::deleteAllBonds();
  if (conn_handle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(conn_handle);
  pairing_until = millis() + 120000;
  Serial.println("ble: bonds cleared, pairing open for 2 min");
}

void ble_drop()
{
  if (conn_handle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(conn_handle);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override
  {
    // A stranger may not pair once the knob has its relay — unless pairing is open.
    bool known = NimBLEDevice::isBonded(info.getIdAddress());
    bool open = NimBLEDevice::getNumBonds() == 0 || millis() < pairing_until;
    if (!known && !open) {
      Serial.printf("ble: refused %s (not bonded, pairing closed)\n", info.getAddress().toString().c_str());
      s->disconnect(info);
      return;
    }
    conn_handle = info.getConnHandle();
    encrypted = info.isEncrypted();
    subscribed = false;
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);  // 15–30 ms interval, 4 s timeout
    // The Mac starts pairing itself: it reads TX first (READ_ENC). A security request from the knob
    // (startSecurity) was ignored by macOS and timed out after 30 s (reason 534).
    Serial.printf("ble: connected %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override
  {
    if (info.getConnHandle() == conn_handle) conn_handle = BLE_HS_CONN_HANDLE_NONE;
    encrypted = subscribed = false;
    Serial.printf("ble: disconnected, reason %d\n", reason);
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override
  {
    encrypted = info.isEncrypted();
    if (!encrypted) {
      server->disconnect(info);
      return;
    }
    pairing_until = 0;
    Serial.printf("ble: encrypted, bonded=%d\n", info.isBonded());
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override
  {
    const NimBLEAttValue &v = c->getValue();
    xSemaphoreTake(rx_mux, portMAX_DELAY);
    rx_buffer.append((const char *)v.data(), v.length());
    if (rx_buffer.size() > 64 * 1024) rx_buffer.clear();  // a runaway line: drop it rather than eat the heap
    xSemaphoreGive(rx_mux);
  }
};

class TxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t value) override
  {
    subscribed = value & 1;
    encrypted = info.isEncrypted();
  }
};

// Parses whole lines outside the BLE host task: a state line is several KB of JSON.
static void ble_task(void *)
{
  for (;;) {
    std::string line;
    bool got = false;
    xSemaphoreTake(rx_mux, portMAX_DELAY);
    size_t end = rx_buffer.find('\n');
    if (end != std::string::npos) {
      line = rx_buffer.substr(0, end);
      rx_buffer.erase(0, end + 1);
      got = true;
    }
    xSemaphoreGive(rx_mux);
    if (got) {
      handle_line(String(line.c_str()), LINK_BLE);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ble_link_start()
{
  rx_mux = xSemaphoreCreateMutex();
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, Just Works, LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  NimBLEService *service = server->createService(NUS_SERVICE);
  NimBLECharacteristic *rx = service->createCharacteristic(
      NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  rx->setCallbacks(new RxCallbacks());
  tx = service->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                 NIMBLE_PROPERTY::READ_ENC);
  tx->setCallbacks(new TxCallbacks());
  service->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(NUS_SERVICE);
  adv->enableScanResponse(true);
  adv->start();
  xTaskCreatePinnedToCoreWithCaps(ble_task, "ble", 12 * 1024, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("ble: advertising as %s, %d bond(s)\n", DEVICE_NAME, NimBLEDevice::getNumBonds());
}

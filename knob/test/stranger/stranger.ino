// Security test for the knob's BLE link: a central that is NOT the paired Mac. Flash it to the board's second
// chip (ESP32-U4WDH, the USB-C side that shows a CH340 port). It finds orca-deck-knob, connects, and tries to
// subscribe to NUS_TX and to write a command line to NUS_RX. Expected: the knob drops the connection at once
// (not bonded, pairing closed), so nothing gets through. Results go to the serial log at 115200.
#include <NimBLEDevice.h>

static const NimBLEUUID SERVICE("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static const NimBLEUUID NUS_RX("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static const NimBLEUUID NUS_TX("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

void attempt()
{
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(5000, false);
  const NimBLEAdvertisedDevice *knob = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (d->getName() == "orca-deck-knob") knob = d;
  }
  if (!knob) {
    Serial.println("stranger: orca-deck-knob not found");
    return;
  }
  Serial.printf("stranger: found %s rssi %d\n", knob->getAddress().toString().c_str(), knob->getRSSI());
  NimBLEClient *client = NimBLEDevice::createClient();
  uint32_t t0 = millis();
  bool connected = client->connect(knob);
  Serial.printf("stranger: connect → %s\n", connected ? "yes" : "no");
  if (connected) {
    delay(300);
    Serial.printf("stranger: still connected after 300 ms → %s\n", client->isConnected() ? "YES (not refused)" : "no (refused)");
    NimBLERemoteService *service = client->isConnected() ? client->getService(SERVICE) : nullptr;
    if (service) {
      NimBLERemoteCharacteristic *rx = service->getCharacteristic(NUS_RX);
      NimBLERemoteCharacteristic *tx = service->getCharacteristic(NUS_TX);
      bool sub = tx && tx->subscribe(true, nullptr);
      const char line[] = "C {\"cmd\":\"enter\"}\n";
      bool wrote = rx && rx->writeValue((const uint8_t *)line, sizeof(line) - 1, true);
      Serial.printf("stranger: subscribe → %d, write with response → %d\n", sub, wrote);
    }
  }
  Serial.printf("stranger: connected at the end → %d (%lu ms)\n", client->isConnected(), millis() - t0);
  client->disconnect();
  NimBLEDevice::deleteClient(client);
}

void setup()
{
  Serial.begin(115200);
  NimBLEDevice::init("stranger");
  NimBLEDevice::setSecurityAuth(true, false, true);  // it would pair if the knob let it
}

void loop()
{
  attempt();
  delay(5000);
}

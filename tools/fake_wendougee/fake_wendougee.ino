// Prueba: la DevKitC hace de "Wendougee": busca BOOKOO_SC_U, se conecta,
// se suscribe a 0x0FFE/0xFF11 e imprime las tramas de peso BooKoo.
#include <NimBLEDevice.h>

static const NimBLEAdvertisedDevice *target = nullptr;
static volatile uint32_t n_pkts = 0;

class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    std::string n = d->getName();
    if (n.rfind("BOOKOO", 0) == 0 && !target) {
      Serial.printf("[FAKE] encontrado '%s' %s RSSI %d\n", n.c_str(), d->getAddress().toString().c_str(), d->getRSSI());
      target = d;
      NimBLEDevice::getScan()->stop();
    }
  }
};
static ScanCb scanCb;

static void onNotify(NimBLERemoteCharacteristic *, uint8_t *d, size_t len, bool) {
  n_pkts++;
  if (len >= 13 && d[0] == 0x03 && d[1] == 0x0B) {
    uint32_t cg = (d[7] << 16) | (d[8] << 8) | d[9];
    float w = cg / 100.0f * (d[6] == 0x2D ? -1 : 1);
    float f = ((d[11] << 8) | d[12]) / 100.0f * (d[10] == 0x2D ? -1 : 1);
    uint32_t ms = (d[2] << 16) | (d[3] << 8) | d[4];
    if (n_pkts % 10 == 1) Serial.printf("[FAKE] peso %.2f g  caudal %.2f g/s  timer %lu ms  bat %u  (paquete %lu)\n",
                                        w, f, (unsigned long)ms, d[13], (unsigned long)n_pkts);
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("[FAKE] Wendougee de prueba");
  NimBLEDevice::init("FakeWendougee");
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCb, false);
  s->setActiveScan(true);
  s->start(10000, false);
}

void loop() {
  static bool done = false;
  if (target && !done) {
    done = true;
    NimBLEClient *c = NimBLEDevice::createClient();
    if (!c->connect(target)) { Serial.println("[FAKE] fallo al conectar"); return; }
    Serial.println("[FAKE] conectado");
    NimBLERemoteService *svc = c->getService(NimBLEUUID((uint16_t)0x0FFE));
    if (!svc) { Serial.println("[FAKE] sin servicio 0x0FFE"); return; }
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFF11));
    if (!ch || !ch->subscribe(true, onNotify)) { Serial.println("[FAKE] no se pudo suscribir a 0xFF11"); return; }
    Serial.println("[FAKE] suscrito a 0xFF11 (peso BooKoo)");
  }
  static uint32_t last = 0;
  if (millis() - last > 5000) { last = millis(); Serial.printf("[FAKE] paquetes recibidos: %lu\n", (unsigned long)n_pkts); }
  delay(20);
}

/*
 * Timemore Dot Panel — copro_test_01 (fase 0 del coprocesador, paso 1)
 *
 * Placa: ESP32-S3-DevKitC-1 N8R8 v1.1, por el USB nativo (consola USB CDC).
 * Prueba por separado cada periferico del coprocesador y lo muestra por serie:
 *
 *  - 2 encoders KY-040 contados por HARDWARE con el periferico PCNT
 *    (ESP32Encoder): la CPU solo lee el contador, no se pierden pasos aunque
 *    este ocupada. Filtro antirrebote tambien por hardware (glitch filter).
 *    Pulsadores (SW) por INTERRUPCION (flanco) + tarea con antirrebote de 10 ms:
 *    pulsado (inmediato), clic, doble clic y pulsacion larga.
 *  - Zumbador en el GPIO 2: secuencia que sirve para saber si es activo o pasivo.
 *  - MAX31865 (SPI) + PT100 a 3 hilos, Rref 430 ohm: temperatura y fallos.
 *
 * Conexiones (SPEC.md 2.3), todo a 3V3 y GND:
 *   Encoder "Ajustes": CLK 4, DT 5, SW 6     Encoder "Accion": CLK 15, DT 16, SW 7
 *   MAX31865: SCK 12, SDI(MOSI) 11, SDO(MISO) 13, CS 10     Zumbador: 2
 *
 * Comandos por serie: 'b' repite la prueba del zumbador, 'z' pone los
 * encoders a cero, 'f' muestra el detalle de fallos del MAX31865.
 */

#include <ESP32Encoder.h>
#include <Adafruit_MAX31865.h>
#include <SPI.h>
#include "esp_timer.h"

// ---------------------------------------------------------------- pines
static const int ENC_A_CLK = 4,  ENC_A_DT = 5,  ENC_A_SW = 6;   // "Ajustes"
static const int ENC_B_CLK = 15, ENC_B_DT = 16, ENC_B_SW = 7;   // "Accion"
static const int PIN_SCK = 12, PIN_MOSI = 11, PIN_MISO = 13, PIN_CS = 10;
static const int PIN_BUZZER = 2;

static const float RTD_NOMINAL = 100.0f;   // PT100
static const float RTD_REF     = 430.0f;   // resistencia de referencia del modulo

// ---------------------------------------------------------------- encoders
ESP32Encoder encA, encB;

// Un KY-040 da 4 flancos (un ciclo completo de cuadratura) por cada "clic" mecanico.
static int32_t detents(ESP32Encoder &e) {
  int64_t c = e.getCount();
  return (int32_t)((c >= 0 ? c + 2 : c - 2) / 4);   // redondeo al clic mas cercano
}

// ---------------------------------------------------------------- pulsadores (por interrupcion)
// La ISR de cada pin SW (flanco de subida o bajada) guarda el instante del
// primer flanco y despierta a btn_task. La tarea espera DEBOUNCE_MS a que el
// contacto deje de rebotar, lee el nivel y emite el evento. La PULSACION se
// notifica en cuanto se confirma (sin esperar a soltar): sirve para que la tara
// responda al instante. Sin actividad, la tarea duerme (no hay sondeo).
struct Button {
  int pin;
  const char *name;
  bool stable;                 // nivel confirmado (true = suelto, con pull-up)
  volatile int64_t t_edge;     // primer flanco pendiente [us] (0 = ninguno)
  uint32_t t_press;            // inicio de la pulsacion [ms]
  bool long_sent;
  uint8_t clicks;              // clics pendientes de clasificar (simple/doble)
  uint32_t t_release;
};
static Button btns[2] = {
  {ENC_A_SW, "Ajustes", true, 0, 0, false, 0, 0},
  {ENC_B_SW, "Accion",  true, 0, 0, false, 0, 0},
};
static TaskHandle_t btn_task_h = NULL;
static const uint32_t DEBOUNCE_MS = 10, LONG_MS = 600, DOUBLE_MS = 300;

static void IRAM_ATTR btn_isr(void *arg) {
  int i = (int)(intptr_t)arg;
  if (btns[i].t_edge == 0) btns[i].t_edge = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  xTaskNotifyFromISR(btn_task_h, 1u << i, eSetBits, &woken);
  portYIELD_FROM_ISR(woken);
}

static void btn_task(void *) {
  for (;;) {
    // Si hay algo en curso (pulsado o clics por clasificar), despierta cada 10 ms
    // para medir la pulsacion larga y la ventana del doble clic; si no, duerme.
    bool busy = false;
    for (auto &b : btns) busy |= (!b.stable && !b.long_sent) || b.clicks;
    uint32_t bits = 0;
    xTaskNotifyWait(0, 0xFFFFFFFF, &bits, busy ? pdMS_TO_TICKS(10) : portMAX_DELAY);

    if (bits) vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));      // deja pasar el rebote
    uint32_t now = millis();
    for (int i = 0; i < 2; i++) {
      Button &b = btns[i];
      if (bits & (1u << i)) {
        int64_t t_edge = b.t_edge;
        b.t_edge = 0;
        bool level = digitalRead(b.pin);
        if (level != b.stable) {
          b.stable = level;
          uint32_t lat_us = (uint32_t)(esp_timer_get_time() - t_edge);
          if (!level) {                                     // pulsado
            b.t_press = now; b.long_sent = false;
            Serial.printf("[BTN] %s: pulsado (confirmado %u us tras el flanco)\n", b.name, lat_us);
          } else if (!b.long_sent) {                        // soltado tras pulsacion corta
            b.clicks++; b.t_release = now;
          }
        }
      }
      if (!b.stable && !b.long_sent && now - b.t_press >= LONG_MS) {
        b.long_sent = true; b.clicks = 0;
        Serial.printf("[BTN] %s: PULSACION LARGA\n", b.name);
      }
      if (b.clicks && b.stable && now - b.t_release >= DOUBLE_MS) {
        Serial.printf("[BTN] %s: %s\n", b.name, b.clicks >= 2 ? "DOBLE CLIC" : "CLIC");
        b.clicks = 0;
      }
    }
  }
}

// ---------------------------------------------------------------- zumbador
static void buzzer_test() {
  Serial.println("[BUZ] Fase 1: nivel alto fijo 1 s (un zumbador ACTIVO pita; uno PASIVO solo hace clic)");
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH); delay(1000);
  digitalWrite(PIN_BUZZER, LOW);  delay(700);
  Serial.println("[BUZ] Fase 2: tonos 500, 1000, 2000 y 4000 Hz (un PASIVO suena cada vez mas agudo)");
  ledcAttach(PIN_BUZZER, 1000, 8);
  const uint32_t f[] = {500, 1000, 2000, 4000};
  for (uint32_t hz : f) { ledcWriteTone(PIN_BUZZER, hz); delay(400); ledcWriteTone(PIN_BUZZER, 0); delay(150); }
  ledcDetach(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println("[BUZ] Fin. Dime que has oido en cada fase.");
}

// ---------------------------------------------------------------- MAX31865
Adafruit_MAX31865 rtd(PIN_CS);                 // SPI por hardware (bus FSPI)
static volatile float g_temp = NAN;
static volatile uint16_t g_raw = 0;
static volatile uint8_t g_fault = 0;
static volatile uint32_t g_samples = 0;

// Lectura en su propia tarea: readRTD() bloquea ~75 ms (conversion one-shot).
// Ni los encoders (PCNT) ni los pulsadores (interrupcion) dependen de esto.
static void temp_task(void *) {
  for (;;) {
    uint16_t raw = rtd.readRTD();
    uint8_t f = rtd.readFault();
    if (f) rtd.clearFault();
    g_raw = raw;
    g_fault = f;
    g_temp = f ? NAN : rtd.temperature(RTD_NOMINAL, RTD_REF);
    g_samples++;
    vTaskDelay(pdMS_TO_TICKS(25));             // ~10 lecturas/s en total
  }
}

static void print_fault(uint8_t f) {
  if (!f) { Serial.println("[TMP] sin fallos"); return; }
  Serial.printf("[TMP] fallo 0x%02X:", f);
  if (f & MAX31865_FAULT_HIGHTHRESH) Serial.print(" RTD alta (sonda abierta?)");
  if (f & MAX31865_FAULT_LOWTHRESH)  Serial.print(" RTD baja (cortocircuito?)");
  if (f & MAX31865_FAULT_REFINLOW)   Serial.print(" REFIN- > 0.85 Vbias");
  if (f & MAX31865_FAULT_REFINHIGH)  Serial.print(" REFIN- < 0.85 Vbias (FORCE- abierto)");
  if (f & MAX31865_FAULT_RTDINLOW)   Serial.print(" RTDIN- < 0.85 Vbias (FORCE- abierto)");
  if (f & MAX31865_FAULT_OVUV)       Serial.print(" sobre/subtension");
  Serial.println();
}

// ---------------------------------------------------------------- arduino
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // espera a la consola USB
  Serial.println("\n[COP] copro_test_01 arrancando");

  // Encoders por PCNT: pull-ups internas (los KY-040 ya traen 10k) y filtro
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encA.attachFullQuad(ENC_A_CLK, ENC_A_DT);
  encB.attachFullQuad(ENC_B_CLK, ENC_B_DT);
  encA.setFilter(1023);                        // ignora pulsos < ~12.8 us (80 MHz APB)
  encB.setFilter(1023);
  encA.clearCount();
  encB.clearCount();
  Serial.println("[ENC] encoders en PCNT (hardware), 4 flancos por clic");

  // Pulsadores por interrupcion
  xTaskCreatePinnedToCore(btn_task, "btn", 4096, NULL, 5, &btn_task_h, 1);
  for (int i = 0; i < 2; i++) {
    pinMode(btns[i].pin, INPUT_PULLUP);
    btns[i].stable = digitalRead(btns[i].pin);
    attachInterruptArg(btns[i].pin, btn_isr, (void *)(intptr_t)i, CHANGE);
  }
  Serial.println("[BTN] pulsadores por interrupcion (CHANGE) con antirrebote de 10 ms");

  // MAX31865
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (rtd.begin(MAX31865_3WIRE)) Serial.println("[TMP] MAX31865 iniciado a 3 hilos");
  else                           Serial.println("[TMP] MAX31865: begin() fallo");
  xTaskCreatePinnedToCore(temp_task, "temp", 4096, NULL, 1, NULL, 1);

  buzzer_test();
}

void loop() {
  uint32_t now = millis();

  // Encoders: se informa solo cuando cambian
  static int32_t lastA = 0, lastB = 0;
  int32_t a = detents(encA), b = detents(encB);
  if (a != lastA || b != lastB) {
    Serial.printf("[ENC] Ajustes %+ld (%+ld)   Accion %+ld (%+ld)   [clics (flancos)]\n",
                  (long)a, (long)encA.getCount(), (long)b, (long)encB.getCount());
    lastA = a; lastB = b;
  }

  // Temperatura cada segundo
  static uint32_t t_tmp = 0, last_samples = 0;
  if (now - t_tmp >= 1000) {
    t_tmp = now;
    uint32_t n = g_samples;
    float ohm = g_raw * RTD_REF / 32768.0f;
    // Sin modulo, MISO queda a 0 (o a 1): raw 0 / 32767 sin fallo -> no responde
    if (!g_fault && (g_raw == 0 || g_raw >= 32767))
      Serial.printf("[TMP] el MAX31865 no responde (raw %u): revisar cableado SPI y 3V3\n", g_raw);
    else if (g_fault) Serial.printf("[TMP] FALLO 0x%02X  raw %u (%.2f ohm)  %u lecturas/s\n",
                               g_fault, g_raw, ohm, n - last_samples);
    else         Serial.printf("[TMP] %.2f C  raw %u (%.2f ohm)  %u lecturas/s\n",
                               g_temp, g_raw, ohm, n - last_samples);
    last_samples = n;
  }

  // Comandos
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'b') buzzer_test();
    if (c == 'z') { encA.clearCount(); encB.clearCount(); Serial.println("[ENC] a cero"); }
    if (c == 'f') print_fault(g_fault);
  }
  delay(2);
}

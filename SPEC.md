# Timemore Dot Panel — Especificación del proyecto

Panel de control físico, independiente y de código abierto para la báscula **Timemore Black Mirror Dot**, que no tiene pantalla. Muestra el **peso, el tiempo y el ratio en grande** (legibles sin gafas), junto con el caudal, la **temperatura del líquido** y la curva de extracción, en una pantalla táctil de 7". Se maneja con dos encoders giratorios y la pantalla táctil. Sirve para **espresso**, **filtrado (pour-over)** y **leche**, sin apps, registro ni login. Funciona con batería y se integra opcionalmente con **Home Assistant**.

| | |
| :--- | :--- |
| **Estado** | Especificación cerrada · **hardware pedido el 2026-11-08** (entrega prevista: 15–28 de noviembre) · fase 0 pendiente |
| **Fecha** | 2026-11-07 · **revisión mayor 2026-11-08**: pantalla de 7" (modelo **7B**) + coprocesador ESP32-S3-DevKitC-1 (ver "Decisiones tomadas") |
| **Proyecto relacionado** | [BLE Scale Translator Bridge](https://github.com/jcallano/BLE-_Scale_Translator_Bridge__Wendougee_E-Bar): de él se reutiliza el driver BLE de la Dot, ya validado en hardware |

---

## 1. Objetivos y alcance

**Objetivos**
1. Leer la Dot por BLE y mostrar el peso con la menor latencia posible.
2. **Legibilidad sin gafas** (requisito principal de la interfaz): el peso, el tiempo y el ratio tienen la misma importancia y se muestran en grande, legibles a 60–100 cm por una persona con presbicia (ver 4.1).
3. Guiar shots de espresso (dosis → ratio → objetivo), preparaciones de filtrado (bloom, etapas y ritmo de vertido) y el vaporizado de leche (temperatura objetivo).
4. Medir la **temperatura del líquido** con una sonda PT100 (sensor de platino) y unirla en una sola curva de peso, caudal y temperatura. Ningún proyecto abierto lo hace hoy (ver 4.7).
5. Funcionar con batería y cargarse por USB-C.
6. Integrarse opcionalmente con **Home Assistant** por WiFi (MQTT), sin afectar a la pantalla ni al BLE.
7. **Hardware sin PCB a medida:** solo módulos estándar conectados con cables y bibliotecas de código abierto.
8. **Devolver algo a la comunidad:** publicar la temperatura en formatos que Decaid y Beanconqueror ya entienden, documentar el perfil BLE y el protocolo, y publicar el hardware completo.

**Fuera de alcance (por ahora)**
- Funcionar **a la vez** que el puente o que Decaid sobre la misma Dot. La Dot acepta **una sola conexión BLE**. El panel tendrá una opción para **liberar la báscula**; la fase 8 lo resuelve integrando el puente en el propio panel.
- Controlar la máquina de espresso. El panel avisa, pero no corta la extracción.
- Historial en la nube o cuentas de usuario (el historial va a Home Assistant, en la red local).

---

## 2. Hardware

### 2.1. Arquitectura de hardware

```
 Timemore Dot ──BLE──┐
                     ▼
        ┌────────────────────────────────────┐
        │ Waveshare ESP32-S3-Touch-LCD-7B    │  · pantalla 7" IPS 1024×600 + táctil
        │ (ESP32-S3 N16R8)                   │  · LVGL: números grandes y gráficas
        │ radio SOLO para BLE                │  · driver BLE de la Dot (del puente)
        │                                    │  · cargador de batería CS8501 → 18650
        └─────────────────┬──────────────────┘
                 I2C (GPIO 8/9), maestro → esclavo en 0x42
        ┌─────────────────┴──────────────────┐   WiFi / MQTT
        │ ESP32-S3-DevKitC-1 N8R8            │ ─────────────► Home Assistant
        │ (coprocesador)                     │
        │ · núcleo 1: 2 encoders (PCNT),     │
        │   MAX31865 + PT100 (SPI),          │
        │   zumbador (PWM), esclavo I2C      │
        │ · núcleo 0: WiFi + MQTT, ajustes   │
        │   e historial en su flash          │
        └────────────────────────────────────┘
```

**Por qué dos microcontroladores:**
- La pantalla de 7" usa un bus paralelo RGB que **ocupa casi todos los GPIO** de su ESP32-S3. Solo quedan accesibles el I2C, el RS485, el CAN (detrás de sus transceptores) y los conectores de sensor y ADC.
- **Cada radio en un chip distinto:** la S3 de la pantalla hace solo BLE (latencia medida en el puente: ~0.35 ms) y la S3 del coprocesador hace el WiFi. Así no hay convivencia WiFi/BLE en la misma radio ni competencia por la PSRAM que haga parpadear la imagen.
- Los ajustes y el historial se guardan en la **flash del coprocesador**. Las escrituras en la flash de la S3 congelan la PSRAM un instante y provocan glitches en la pantalla.

### 2.2. Lista de materiales

| # | Componente | Uso | Estado / coste aprox. |
| :---: | :--- | :--- | :--- |
| 1 | **Waveshare ESP32-S3-Touch-LCD-7B**, versión **con táctil** (variante de tienda "Touch-LCD-7B-U"): ESP32-S3-WROOM-1 **N16R8**, IPS **1024×600**, 235 cd/m², ángulo de 170°, táctil capacitivo GT911, expansor de E/S con **brillo regulable** y **lectura de la tensión de la batería**, cargador CS8501, **salida de 5 V** por PH2.0. Incluye cables PH2.0 y tornillería. | Interfaz, BLE con la Dot y carga de la batería | ✅ **Pedido** (tienda oficial de Waveshare, OMR 15.93 + 6.81 de envío) |
| 2 | **Espressif ESP32-S3-DevKitC-1** (ESP32-S3-WROOM-1 **N8R8**, revisión v1.1) | Coprocesador: encoders, PT100, zumbador, WiFi/MQTT, almacenamiento | Disponible (la que había quedado libre) |
| 3 | **2× encoder KY-040 / EC11** con pulsador, y perillas | "Ajustes" y "Acción" | En la caja |
| 4 | **Módulo zumbador del kit Arduino** (serigrafía "5V"; pasivo o activo por identificar) | Avisos sonoros | En la caja |
| 5 | **Módulo MAX31865 estándar** (resistencia de referencia de **430 Ω**, para PT100), configurado a **3 hilos** con sus puentes de soldadura | Convertidor de la sonda | ✅ Pedido (OMR 1.76) |
| 6 | **Sonda PT100 de 3 hilos, 2 × 100 mm** (vaina de acero inox 304/316 de 2 mm de diámetro y 100 mm de largo), cable de silicona o PTFE de 1–1.5 m | Temperatura del líquido. La vaina de 2 mm da la respuesta más rápida (~1–2 s en agua). Con 100 mm la unión con el cable queda fuera del líquido. | ✅ Pedidas **2** (una de repuesto) |
| 7 | **Conector GX16 de 3 pines** (hembra de panel + macho de cable) | Conectar y desconectar la sonda para lavarla; contactos fiables (importante en una PT100) | ✅ Pedido |
| 8 | **Batería 18650 Li-ion 2500 mAh** en una **caja empotrable con tapa para 1 × 18650**, con cable al conector **PH2.0** de la placa de 7" | Alimentación; se cambia abriendo la tapa | Batería disponible; ✅ cajas pedidas |
| 8b | **Segunda 18650 de repuesto** + **cargador externo** | Se carga fuera mientras la otra está en uso | Por comprar (cargador opcional si ya hay uno) |
| 8c | **Placa de protección 1S** (DW01 + MOSFET, ≥ 3 A), si las celdas no traen protección propia | Protección contra cortocircuito y sobredescarga de la celda | ~0.5 USD |
| 9 | **Interruptor basculante** de panel (≥ 3 A), en serie con el positivo de la batería | Corte total para almacenamiento. Los deslizantes pequeños (SS12D00, 0.5 A) no aguantan los ~0.7 A del sistema. | ✅ Pedido (anotar su tamaño para el hueco de la caja) |
| 10 | **Kit de cables PH2.0** (2, 3, 4, 5 y 6 pines, 15 cm, 26 AWG) | Batería → placa (2 pines), I2C → coprocesador (4 pines), 5 V → coprocesador, conectores de sensor | ✅ Pedido (OMR 3.52) |
| 11 | Caja impresa en **PETG** (Creality K1 Max) | Carcasa | Filamento |
| 12 | **Insertos roscados de latón M2 / M2.5 / M3** (kit surtido) + tornillos Allen M2.5 y M3 | Roscas metálicas en la caja impresa: placa de 7", MAX31865, tapa trasera | Por comprar (~6–10 USD) |
| 13 | **Soldador Aneng SL108** con punta para insertos | Montaje de los insertos y soldaduras (pines del MAX31865, conector GX16) | ✅ Comprado |
| — | Cables dupont hembra-hembra, tubo termorretráctil | Cableado del coprocesador | En la caja |

**Coste total aproximado:** ~60–75 USD.

**⚠️ Polaridad del cable PH2.0 de la batería:** el orden rojo/negro de estos cables no es estándar. Comprobar contra la serigrafía +/− de la placa antes de conectar.

**Ya no forman parte del diseño** (decisiones del 2026-11-08): OLED de 2.42", 1.54" y 1.3", TFT de 1.47", módulo TP4056, diodo Schottky, divisor de batería y Raspberry Pi Pico 2 W.

### 2.3. Conexiones

**Placa de 7" ESP32-S3-Touch-LCD-7B (lo accesible según la wiki de Waveshare):**

| Interfaz | Conector | GPIO | Uso en el panel |
| :--- | :--- | :--- | :--- |
| **I2C** | PH2.0 | **SDA 8 / SCL 9** | Enlace con el coprocesador (esclavo en **0x42**). Compartido con el táctil (GT911, 0x5D) y el expansor de E/S. **Direcciones ocupadas: 0x24 y 0x5D.** |
| **Salida de 5 V** | PH2.0 | — | **Alimentación del coprocesador** (DevKitC, pin 5V) |
| Sensor | PH2.0 a pines de 2.54 mm | Por confirmar | Opcional: línea de "datos listos" desde el coprocesador |
| ADC | PH2.0 | Por confirmar | Sin usar (la batería ya se mide por el expansor) |
| Batería | PH2.0 | — | 18650 a través del interruptor |
| USB-C (nativo) | — | 19 / 20 | Programación y alimentación |
| USB-C (USB-UART) | — | — | Programación, consola y carga de la batería |
| Ocupados por la pantalla | — | 0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38–42, 45–48 | No disponibles |
| Ocupados por el táctil | — | 4 (IRQ), 8, 9 | El I2C es compartido |
| RS485 (15/16), CAN (19/20, compartido con el USB), tarjeta SD (11/12/13) | PH2.0 / ranura | — | Sin usar |

**Coprocesador ESP32-S3-DevKitC-1 N8R8 v1.1 (asignación propuesta; se confirma en la fase 0):**

| Función | Señal | GPIO | Notas |
| :--- | :--- | :---: | :--- |
| **I2C esclavo** (hacia la placa de 7") | SDA / SCL | **8 / 9** | Dirección 0x42 |
| **Encoder "Ajustes"** | CLK / DT / SW | **4 / 5 / 6** | Contado por hardware (PCNT) |
| **Encoder "Acción"** | CLK / DT / SW | **15 / 16 / 7** | Contado por hardware (PCNT) |
| **MAX31865** (SPI) | SCK / MOSI / MISO / CS | **12 / 11 / 13 / 10** | SPI modo 1 |
| **Zumbador** | PWM (LEDC) | **2** | |
| Línea "datos listos" (opcional) | Salida | **14** → conector de sensor de la placa de 7" | |
| Alimentación | 5V / GND | — | Desde la **salida de 5 V (PH2.0)** de la placa de 7"; GND común |
| **Evitar** | — | 0, 3, 45, 46 (strapping); 19, 20 (USB); 35–37 (PSRAM); 38 (LED RGB); 43, 44 (consola) | |
| Libres | — | 1, 17, 18, 21, 39–42, 47, 48 | |

Los encoders KY-040 y el MAX31865 se alimentan a **3.3 V** (pin 3V3 de la DevKitC). Todo se conecta con cables dupont a las cabeceras, sin soldar.

### 2.4. Alimentación

- **La batería se conecta a la placa de 7"** (conector PH2.0). Su chip **CS8501** carga a **580 mA** por el USB-C y eleva la tensión a 5 V para todo el sistema.
- **Batería intercambiable:** una 18650 va en una **caja empotrable con tapa** y la otra se carga en un **cargador externo**. El cambio se hace abriendo la tapa, en segundos, así que la autonomía práctica es ilimitada. El cargador de la placa (USB-C) sigue disponible para cargar la celda instalada.
- **Al cambiar la batería, respetar la polaridad marcada en la caja (+/−):** una celda al revés puede dañar la placa. Usar celdas con protección propia o una placa de protección 1S.
- **Se descartó poner dos celdas en paralelo** (5000 mAh): exigía celdas emparejadas, igualar sus tensiones y añadir una placa de protección, y la carga a 580 mA tardaba ~9–10 h. El cambio manual es más simple y seguro.
- Waveshare **recomienda celdas de menos de 2000 mAh.** La 18650 de 2500 mAh carga igual con el cargador de la placa, en más tiempo (~5 h). Se verificará en la fase 0 que no se calienta ni corta.
- **Consumo estimado:** placa de 7" ~350 mA a 5 V (dato de Waveshare para la 7B); coprocesador (DevKitC) ~40–50 mA sin WiFi y ~70–100 mA de media con WiFi (menos en modem-sleep). Desde la batería, ~550–650 mA → **~3.5–4 h de uso activo por celda**, más con el brillo bajado.
- **% de batería:** la 7B lee la tensión de la batería por su expansor de E/S, sin cableado adicional.
- **Ahorro:**
  - atenuar y apagar la pantalla por inactividad;
  - WiFi en ahorro de energía (el coprocesador envía a Home Assistant por lotes);
  - reposo profundo de ambos micros tras 10 min sin uso.
  - Con eso la autonomía real es de varios días de uso doméstico.
- **Interruptor** en la línea de la batería para guardarlo sin consumo.
- **Por verificar en la fase 0:** cuánta corriente da la salida de 5 V (la DevKitC con WiFi pide ~100 mA, con picos de ~300 mA) y cómo despertar la S3 (interrupción del táctil en GPIO 4, o la línea del coprocesador).

### 2.5. Diseño exterior

- **Forma:** consola tipo **cuña**, con el frente inclinado ~20° para leerla de pie junto a la máquina.
- **Medidas aproximadas:** la placa de 7" con cristal táctil mide **193 × 111 mm**. Con marco y encoders, el frente queda en **~240 × 125 mm** (encoders a la derecha) o **~205 × 160 mm** (encoders debajo). Se decide en la fase 6.
- **Detrás de la pantalla:** el coprocesador (DevKitC, 69 × 25 mm), el MAX31865, la caja empotrable de la 18650 (accesible desde fuera para cambiarla, con ventilación) y el interruptor.
- **En el lateral:** el USB-C de la placa (carga y programación), el conector GX16 de la sonda (agujero de 16 mm) y el interruptor basculante (hueco rectangular según el modelo recibido), lejos de la bandeja de goteo.
- **Material:** **PETG** en la **Creality K1 Max** (ABS como alternativa). No usar PLA: junto a la máquina hay calor y vapor, y el PLA se deforma a ~55 °C.
- **Fijaciones:**
  - **insertos roscados de latón** fundidos en el PETG con el soldador (Aneng SL108 con punta para insertos, ~220–240 °C) para la placa de 7", el MAX31865 y la tapa trasera (M3). Los agujeros se diseñan según la ficha del kit de insertos (p. ej. ~4.0 mm para un M3 de 4.2 mm de diámetro exterior);
  - los encoders, el GX16 y el interruptor se fijan con su propia tuerca o a presión, sin insertos.
- **Entregables:** fuentes editables (FreeCAD u OpenSCAD) y STL en el repo.

---

## 3. Arquitectura de software

### 3.1. Bibliotecas (todas de código abierto)

**Placa de 7" (ESP32-S3):**

| Biblioteca | Licencia | Uso |
| :--- | :--- | :--- |
| Arduino-ESP32 3.x | LGPL-2.1 | Plataforma (opciones: Flash 16 MB, **PSRAM OPI 8 MB**, tabla de particiones grande si la interfaz lo pide). La wiki de la 7B usa el paquete de placas "esp32-XIP-3.1.1": validar en la fase 0 si basta con el core 3.3.x instalado. |
| [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) + [ESP32_IO_Expander](https://github.com/esp-arduino-libs/ESP32_IO_Expander) | Apache-2.0 | Driver de la pantalla RGB, el táctil y el expansor de E/S (brillo, batería) |
| [LVGL](https://lvgl.io/) 8.4 | MIT | Interfaz: fuentes grandes, gráficas (`lv_chart`), botones |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.x | Apache-2.0 | Cliente BLE de la Dot (y periférico en las fases 7 y 8) |

**Coprocesador (ESP32-S3-DevKitC-1):**

| Biblioteca | Licencia | Uso |
| :--- | :--- | :--- |
| Arduino-ESP32 3.x | LGPL-2.1 | Plataforma (la misma que la placa de 7"): WiFi, esclavo I2C (`Wire.begin(0x42)`, `onRequest`/`onReceive`, `slaveWrite`), FreeRTOS con dos núcleos |
| [ESP32Encoder](https://github.com/madhephaestus/ESP32Encoder) | Abierta | Encoders por el periférico **PCNT** (contador de cuadratura por hardware, sin pérdida de pasos) |
| [Adafruit MAX31865](https://github.com/adafruit/Adafruit_MAX31865) | Abierta (Adafruit) | Lectura de la PT100 (Callendar–Van Dusen) |
| Cliente MQTT (PubSubClient o arduino-mqtt) + autodescubrimiento de Home Assistant (p. ej. [ArduinoHA](https://github.com/dawidchyrzynski/arduino-home-assistant)) | MIT / abiertas | Integración con Home Assistant |
| NVS / LittleFS | — | Ajustes, recetas e historial en la flash del coprocesador |

**ESPHome no sirve para el coprocesador** (aunque es la vía habitual entre ESP32 y Home Assistant): no puede funcionar como esclavo I2C.

### 3.2. Tareas

**Placa de 7":**

| Núcleo | Tarea | Responsabilidad |
| :---: | :--- | :--- |
| 0 | `nimble_host` (stack) | Callbacks BLE: publica cada muestra de la Dot con su marca de tiempo. |
| 0/1 | `scale_link` | Escaneo, conexión, secuencia de inicio de la Dot, sondeo cada 10 s, tara física, liberar la báscula. |
| 1 | `ui` (LVGL) | Dibuja la pantalla, procesa el táctil y los eventos de los encoders, y lleva el estado de cada modo. |
| 1 | `link` | Maestro I2C: intercambia datos con el coprocesador cada 10–20 ms. |

**Coprocesador:**

| Núcleo | Tarea | Responsabilidad |
| :---: | :--- | :--- |
| 1 | Tiempo real | Lectura del PCNT de los encoders y detección de clic, doble clic y pulsación larga; lectura del MAX31865 a ~10 Hz; zumbador; **preparar el bloque de estado** del esclavo I2C. **Nunca lo bloquea el WiFi.** |
| 0 | Red y almacenamiento | WiFi (su pila corre en este núcleo), MQTT y autodescubrimiento de Home Assistant, envío del resumen de cada preparación; ajustes, recetas e historial en la flash. |

**Esclavo I2C en el ESP32:** el coprocesador mantiene el bloque de estado **ya cargado** en el búfer de transmisión (`slaveWrite`) y lo actualiza tras cada cambio. Así la placa de 7" lo lee en cuanto quiere, sin esperar a que el coprocesador procese la petición.

### 3.3. Protocolo I2C entre la placa de 7" y el coprocesador (borrador)

- **Esclavo:** el coprocesador en **0x42**, a 400 kHz. El maestro (la S3) lee y escribe cada 10–20 ms. Si se cablea la línea "datos listos", la S3 lee en cuanto hay eventos.
- **Lectura de estado** (~20 bytes, con CRC-8 al final):

  | Campo | Tipo | Contenido |
  | :--- | :--- | :--- |
  | `version` | uint8 | Versión del protocolo |
  | `flags` | uint8 | Sonda conectada, WiFi conectado, MQTT conectado, error de la sonda |
  | `encA_delta`, `encB_delta` | int8 cada uno | Pasos acumulados desde la última lectura (no se pierden aunque la lectura se retrase) |
  | `buttons` | uint8 | Clic, doble clic y pulsación larga de cada encoder; se mantienen hasta leerlos |
  | `temp_c_x100` | int16 | Temperatura de la PT100 × 100 |
  | `temp_seq` | uint16 | Número de muestra (para saber si es nueva) |
  | `settings_rev` | uint16 | Cambia cuando hay ajustes nuevos desde Home Assistant |

- **Escrituras de la S3 al coprocesador:**
  - **zumbador** (id de tono y duración);
  - **telemetría** para Home Assistant: peso × 10, caudal × 100, tiempo en ms, ratio × 100 y estado/modo;
  - **resumen de preparación** al terminar;
  - **lectura y escritura de ajustes y recetas.**
- El protocolo se documentará en el repo (`docs/I2C_PROTOCOL.md`) para que otros puedan reutilizar el coprocesador.

### 3.4. Driver de la Dot (reutilizado del puente)

Validado en hardware el 2026-11-07. Se basa en las tramas de de1app, [PR #345](https://github.com/decentespresso/de1app/pull/345):
- Servicio `0xFFF0`: notificaciones en `0xFFF1`, comandos en `0xFFF2`.
- Peso: tipo `0x01` en el byte 2, `int16` big-endian en los bytes 8–9, en 0.1 g.
- Secuencia de inicio de la app oficial (dos pasadas), sondeo cada 10 s y tara `A55A030D000200000071`.
- Bonding Just-Works, MAC guardada y reconexión automática.
- Caudal: EMA calculado con la hora real de llegada de cada muestra.

### 3.5. Estabilidad de la pantalla RGB (medidas preventivas)

- **Bounce buffers** en la RAM interna para el refresco RGB, evitando que el acceso a la PSRAM desplace la imagen.
- **Sin WiFi en la S3** (lo hace el coprocesador).
- **Sin escrituras en la flash de la S3 durante el uso:** los ajustes viven en el coprocesador.
- Los búferes de dibujo de LVGL, en la RAM interna cuando sea posible.

---

## 4. Interfaz de usuario

### 4.1. Requisito de legibilidad sin gafas

El usuario tiene presbicia y debe poder leer la información **sin gafas a 60–100 cm**, de pie junto a la máquina. Reglas obligatorias:

| Regla | Valor |
| :--- | :--- |
| **Datos principales: peso, tiempo y ratio (misma importancia)** | Dígitos de **≥ 20–25 mm** de alto |
| Datos secundarios (caudal, temperatura, objetivo) | **≥ 8–10 mm** |
| Texto mínimo (ejes, menús, etiquetas) | **≥ 5 mm** |
| Contraste | Fondo negro; colores saturados. Nada de gris sobre negro. |
| Colores de las curvas | Peso: blanco · Caudal: cian · Temperatura: naranja. Trazos de ≥ 3 px. |
| Señales que no exigen leer | Pitido al llegar al objetivo; **color de fondo** (ámbar: acercándose; verde: objetivo; rojo: pasado); barras de progreso gruesas |
| Menús | 3–4 opciones por pantalla, botones táctiles grandes (≥ 15 mm) |

En la pantalla de 7" de la 7B (~154 × 86 mm de área útil, 1024×600, ~0.15 mm por píxel), 25 mm equivalen a ~166 px de alto de dígito.

### 4.2. Distribución de la pantalla

**Durante la preparación:**
```
┌──────────────────────────────────────────────────────┐
│    36.8 g          0:27           1:2.04             │  ← 3 números de ~25 mm
│  ─────────────────────────────────────────────────── │
│  caudal 1.8 g/s    93.4 °C        objetivo 36.0 g    │  ← secundarios, ~10 mm
│  ╭─── peso ─── caudal ─── temperatura ─────────╮     │  ← franja de gráfica
│  ╰─────────────────────────────────────────────╯     │
└──────────────────────────────────────────────────────┘
```

**Al terminar:** la gráfica pasa a pantalla completa con los ejes grandes y el resumen (dosis, rendimiento, tiempo, ratio final, temperatura media).

### 4.3. Controles

| Control | Girar | Clic | Doble clic | Pulsación larga (> 1 s) |
| :--- | :--- | :--- | :--- | :--- |
| **Encoder izquierdo "Ajustes"** | Cambia el modo (en el menú) o el valor del campo activo | Pasa al siguiente campo (dosis → ratio → objetivo) | — | Abre o cierra el menú de modos |
| **Encoder derecho "Acción"** | Ajuste fino del objetivo **durante** la preparación (±0.1 g) | **Tara** | Inicia o para el timer | Reinicia el timer |
| **Pantalla táctil** | — | Botones grandes del menú; tocar la gráfica para cambiar la escala | — | — |

Los encoders son el control principal durante la preparación, porque funcionan con las manos mojadas u ocupadas. El táctil sirve para menús y para revisar la gráfica.

### 4.4. Modos

**Manual**
- Peso, tiempo y ratio en grande; caudal y temperatura (si hay sonda); timer con el encoder derecho.

**Espresso**
- Ajustes: dosis (g), ratio (1:1.5–1:3) y peso objetivo calculado. Se pueden fijar a mano.
- El timer arranca **solo** con la primera gota (el peso sube más de 0.3 g tras la tara).
- **Ratio en vivo** (peso ÷ dosis) junto al objetivo.
- **Aviso al llegar al objetivo:** pitido y fondo verde. Usa una compensación configurable (por defecto 0.5 s × caudal actual) para avisar *antes* según lo que seguirá cayendo.
- El timer se detiene solo cuando el caudal baja de ~0.2 g/s.
- Temperatura final en la taza (dato informativo).

**Filtrado (pour-over)**
- Ajustes: dosis, ratio (1:15–1:17) → agua total. También agua y tiempo del bloom, y número de etapas.
- **Bloom:** objetivo de agua y cuenta atrás, con pitido al terminar.
- **Etapas:** objetivo acumulado de cada vertido y pitido al alcanzarlo.
- **Banda de caudal** (p. ej. 4–6 g/s) y **banda de temperatura** de la mezcla, con aviso si cae.
- Tiempo total y objetivo de tiempo final.

**Leche**
- Temperatura en grande, objetivo ajustable (55–65 °C) y **pitido anticipado** según la velocidad de subida (°C/s): la leche sube 2–3 °C más tras cortar el vapor.

**Liberar báscula** (en el menú)
- Desconecta el panel de la Dot para que el puente o Decaid puedan conectarse. Se reconecta con un toque.

### 4.5. Sonidos

| Evento | Sonido |
| :--- | :--- |
| Tara, clic de confirmación | Clic corto (2 kHz, 20 ms) |
| Fin del bloom / etapa alcanzada | Dos pitidos (2.5 kHz) |
| Objetivo de espresso o de leche alcanzado | Pitido largo (3 kHz, 300 ms) |
| Batería baja | Tres pitidos graves |
| Se puede silenciar desde el menú | — |

Si el zumbador del kit resulta ser activo, todos los eventos usan su tono fijo con distintos patrones de duración.

### 4.6. Energía

- **Atenuado** a los 2 min sin cambios de peso ni uso de los controles, y **pantalla apagada** a los 5 min. El brillo es regulable en la 7B (expansor de E/S), con un ajuste de brillo en el menú.
- **Reposo profundo** a los 10 min: BLE y WiFi apagados; despertar con un clic de encoder (vía el coprocesador) o un toque en la pantalla. Se reconecta a la Dot en ~1–2 s.
- La batería (% y aviso) se muestra en la esquina de la pantalla.

### 4.7. Temperatura del líquido

**Valor por uso:**

| Uso | Valor | Motivo |
| :--- | :--- | :--- |
| **Filtrado: temperatura de la mezcla (slurry)** | Alto | Es la temperatura real de extracción. Explica diferencias entre preparaciones (precalentado, dripper, ritmo de vertido). |
| **Vaporizado de leche** | Alto | Pitido anticipado al llegar al objetivo (modo Leche). |
| Agua del hervidor | Medio | Confirma la temperatura de salida (92–96 °C). |
| Espresso en la taza | Bajo | Se mide después de extraer y depende de la taza; queda como registro. |

**Panorama de la comunidad** (revisado el 2026-11-08):
- Beanconqueror integra la Dot y 13 termómetros BLE comerciales, pero como dispositivos separados.
- Las básculas abiertas (CaffePeso, beeb/coffee-scale-app) no miden temperatura.
- Coffee Sensor (ESPROFILE) es comercial y mide el grupo E61, no el líquido.
- **Ningún proyecto abierto combina báscula, temperatura sumergida del líquido y una curva conjunta en un dispositivo autónomo.**

### 4.8. Home Assistant

- **MQTT con autodescubrimiento:** Home Assistant crea solo las entidades.
- **Entidades en vivo** (1 Hz, solo durante una preparación): peso, caudal, tiempo, ratio, temperatura, modo y estado de la conexión con la Dot.
- **Resumen de cada preparación:** modo, dosis, rendimiento, ratio final, tiempo, temperatura media y máxima, y caudal medio.
- **Ajustes desde Home Assistant** (opcional): recetas y objetivos.
- Todo lo hace el coprocesador; la placa de 7" solo le pasa los datos por I2C.

---

## 5. Desarrollo por fases

Cada fase termina con un firmware que funciona, probado con la Dot real, y con un commit.

### Fase 0 — Puesta en marcha del hardware
- **Placa de 7" (7B):** ejemplo de Waveshare con LVGL; táctil; regulación del brillo y lectura de la batería por el expansor; medir la corriente que da la salida de 5 V con la DevKitC conectada; identificar el GPIO del conector de sensor; confirmar que el conector de batería es PH2.0 (o HY2.0) y su polaridad.
- **Coprocesador (DevKitC):** encoders por PCNT; MAX31865 estándar (430 Ω) + PT100 a 3 hilos (puentes de soldadura configurados); zumbador (identificar si es pasivo o activo); esclavo I2C en 0x42 con el bloque de estado precargado, medido con el WiFi activo.
- **Enlace:** la placa de 7" lee el estado del coprocesador por I2C y lo muestra.
- **Energía:** 18650 en su caja empotrable; medir el consumo real, comprobar la carga de la celda de 2500 mAh con el cargador de la placa y el cambio de batería.
- **Criterio de aceptación:**
  - la pantalla muestra un número de 25 mm legible sin gafas a 80 cm;
  - los 2 encoders se leen sin saltos ni rebotes a través del enlace I2C;
  - la temperatura se lee con la sonda en agua;
  - el zumbador suena;
  - consumo medido y documentado.

### Fase 1 — Modo manual con la Dot
- Portar el driver de la Dot del puente a la placa de 7" (NimBLE, tareas `scale_link` y muestras con marca de tiempo).
- Pantalla de preparación con peso, tiempo y ratio en grande, caudal y gráfica de peso y caudal.
- Tara (encoder derecho, clic); timer (doble clic).
- **Criterio de aceptación:**
  - el peso coincide con objetos de peso conocido;
  - la pantalla refleja la muestra en menos de 50 ms desde que llega;
  - **sin desplazamientos ni parpadeos de la imagen** durante 30 min con la Dot transmitiendo;
  - la tara física funciona: tarar con un objeto y retirarlo da un peso negativo.

### Fase 2 — Modo espresso
- Menú de modos y ajustes (encoder izquierdo y táctil).
- Timer automático con la primera gota, ratio en vivo, aviso con pitido, fondo de color y compensación, y parada automática.
- **Criterio de aceptación:** en un shot real, el aviso llega a ±1 g del objetivo con la compensación por defecto.

### Fase 3 — Modo filtrado
- Bloom con cuenta atrás, etapas con objetivos acumulados, banda de caudal y avisos sonoros.
- **Criterio de aceptación:** una receta de 2 etapas se guía de principio a fin sin tocar nada salvo el inicio.

### Fase 4 — Temperatura del líquido y modo Leche
- Temperatura en pantalla y como tercera curva; banda de temperatura en filtrado; **modo Leche** con pitido anticipado.
- **Medición experimental:**
  - tiempo de respuesta de la sonda: pasar de agua a 20 °C a agua a 90 °C y medir el tiempo hasta el 63 % y el 90 % del salto;
  - precisión contra referencias: agua hirviendo corregida por altitud y agua con hielo.
- **Criterio de aceptación:**
  - error ≤ 0.5 °C contra las referencias;
  - tiempo de respuesta medido y documentado;
  - el aviso del modo Leche llega a ±2 °C del objetivo.

### Fase 5 — Home Assistant
- WiFi y MQTT en el coprocesador, autodescubrimiento, entidades en vivo y resumen de cada preparación.
- **Criterio de aceptación:**
  - las entidades aparecen solas en Home Assistant;
  - el resumen llega al terminar cada preparación;
  - **ni la latencia del BLE ni la estabilidad de la imagen cambian** con el WiFi activo (medido con la instrumentación de latencia).

### Fase 6 — Energía, persistencia y caja
- Atenuado, apagado y reposo profundo de ambos micros; despertar por encoder o táctil; avisos de batería baja.
- Ajustes, recetas e historial en la flash del coprocesador (NVS o LittleFS).
- Diseño de la caja en PETG (K1 Max): fuentes editables y STL en el repo.
- **Criterio de aceptación:**
  - el panel despierta y reconecta en menos de 3 s;
  - autonomía medida con uso típico documentada.

### Fase 7 — Compartir con la comunidad
- Publicar la temperatura como periférico BLE con el perfil **"Decent Temp"** de Decaid: servicio `0001c155-5f7c-ce0b-3342-a58cf0def2d9`, característica `0002c155-5f7c-ce0b-3342-a58cf0def2d9`, `int16` / 100 en °C.
- Publicarla en un perfil que Beanconqueror ya soporte, o aportar un driver nuevo (pull request).
- Documentar el perfil BLE del panel y el protocolo I2C; publicar el hardware completo.
- **Criterio de aceptación:** Decaid y Beanconqueror muestran la temperatura del panel en vivo.

### Fase 8 (opcional) — Puente integrado
- Añadir al panel la emulación BooKoo hacia Decaid (la función del puente). Así el panel muestra el peso **mientras** Decaid controla la parada por peso, y desaparece la limitación de una sola conexión a la Dot.

### Fase 9 (opcional) — Boya inalámbrica con supercondensador
- Segundo dispositivo del ecosistema, con su propio repo:
  - flotador sellado (electrónica y antena fuera del líquido) con un vástago de acero que lleva el sensor;
  - XIAO nRF52840 y sensor TMP117;
  - supercondensador de 1 F / 5.5 V y carga por contactos pogo magnéticos (o Qi).
- Coste estimado: 26–40 USD en componentes por unidad (50–80 USD contando prototipos). Autonomía estimada: 30–50 min por carga.
- **Condición para empezar:** que la fase 4 demuestre que la temperatura aporta en el uso diario.

---

## 6. Decisiones pendientes

Todas se resuelven con pruebas físicas en la **fase 0**, salvo la 5.

| # | Pregunta | Cómo se resuelve | Afecta a |
| :---: | :--- | :--- | :--- |
| 1 | ¿Cuánta corriente da la salida de 5 V (PH2.0) de la 7B? ¿Basta para la DevKitC con WiFi (~100 mA, picos de ~300 mA)? | Medición | Alimentación (2.4) |
| 2 | ¿Qué GPIO lleva el conector de sensor? ¿Sirve para la línea "datos listos"? | Esquema de la placa | Protocolo I2C (3.3) |
| 3 | ¿El zumbador del kit es pasivo o activo? | Mirar el dorso o alimentarlo sin señal | Sonidos (4.5) |
| 4 | ¿La 18650 de 2500 mAh carga bien con el CS8501 (Waveshare recomienda < 2000 mAh)? | Carga completa vigilando la temperatura | Alimentación (2.4) |
| 5 | ¿Encoders a la derecha o debajo de la pantalla? | Maqueta impresa | Caja (2.5) |

### Decisiones tomadas

| Fecha | Decisión | Motivo |
| :--- | :--- | :--- |
| 2026-11-08 | **Pantalla principal: Waveshare ESP32-S3-Touch-LCD-7B, con táctil** (7" IPS 1024×600, ESP32-S3 N16R8), comprada en la tienda oficial de Waveshare | Requisito de **leer sin gafas**: peso, tiempo y ratio con la misma importancia y dígitos de ~25 mm, más una gráfica amplia. Frente al modelo sin "B": más resolución, **brillo regulable**, **lectura de la batería integrada**, **salida de 5 V** para el coprocesador, solo 0x24 y 0x5D ocupadas en el I2C y menos consumo (350 frente a 450 mA). Su brillo es menor (235 frente a 345 cd/m²), suficiente en interior. |
| 2026-11-08 | **Coprocesador: ESP32-S3-DevKitC-1 N8R8** (la que ya estaba disponible) por I2C, como esclavo en 0x42, para encoders, PT100, zumbador, WiFi/Home Assistant y almacenamiento | La pantalla RGB ocupa casi todos los GPIO de la S3 de la placa de 7". Un expansor I2C no da SPI para el MAX31865 ni lee bien los encoders sin línea de interrupción. La DevKitC tiene más de 30 GPIO, **PCNT** (encoders por hardware), dos núcleos, SPI y su propia radio: **el WiFi sale de la S3 de la pantalla**. Se monta sin soldar, ya está disponible y comparte ecosistema (Arduino-ESP32) con la placa de 7". El XIAO ESP32-S3 queda como alternativa compacta (requiere soldar los pads de detrás). El XIAO ESP32-C3 se descarta: 11 pines, sin PCNT y un solo núcleo. |
| 2026-11-08 | **Home Assistant por MQTT con autodescubrimiento**, desde el coprocesador | Integración estándar y local; aislada de la pantalla y del BLE. ESPHome no sirve porque no puede ser esclavo I2C. |
| 2026-11-08 | **Temperatura con sonda PT100 de 3 hilos, 2 × 100 mm, con cable + MAX31865 estándar (430 Ω)** | Barato (10–18 USD) y preciso (±0.5 °C según el módulo; mejor tras calibrar). La vaina de 2 mm da la respuesta más rápida (~1–2 s) y los 100 mm dejan la unión con el cable fuera del líquido. El MAX31865 que se encuentra habitualmente trae la resistencia de 430 Ω (PT100); a 3 hilos compensa el cable, así que la PT100 rinde igual que una PT1000. El infrarrojo mide solo la superficie y lo falsea el vapor; la boya inalámbrica queda como fase 9. |
| 2026-11-08 | **Publicar la temperatura en formatos que las apps ya entienden** ("Decent Temp" para Decaid; perfil compatible con Beanconqueror) | Devolver valor a la comunidad sin exigir cambios en esas apps. |
| 2026-11-08 | **Interfaz con LVGL** (sustituye a U8g2 + LovyanGFX) | Con 1024×600 y táctil, LVGL da fuentes grandes, gráficas y botones listos; es lo que usan los ejemplos oficiales de la placa. |
| 2026-11-08 | **Conector GX16 de 3 pines** para la sonda, **interruptor basculante** de ≥ 3 A y **kit de cables PH2.0** | GX16: robusto y con buenos contactos (en una PT100 unas décimas de ohmio ya se notan). Basculante: los deslizantes pequeños solo aguantan 0.5 A. PH2.0: el conector de batería, I2C, 5 V y sensor de la placa. |
| 2026-11-08 | **Insertos roscados de latón** en la caja impresa, colocados con el **soldador Aneng SL108** con punta para insertos | Roscas metálicas que aguantan abrir la caja muchas veces (para cambiar la batería o hacer ajustes). |
| 2026-11-07 | **Dos encoders**: "Ajustes" y "Acción" (ahora conectados al coprocesador) | La tara y el timer quedan a un clic durante la extracción, sin navegar por menús, incluso con las manos mojadas. |
| 2026-11-07 | **Zumbador: el módulo piezo del kit Arduino** (ahora en el coprocesador) | Ya está en la caja. |
| 2026-11-07 | **Caja impresa en 3D** en la **Creality K1 Max**, en **PETG** | Impresora cerrada: PETG sin problemas (y ABS si hiciera falta). |
| 2026-11-08 | **Batería 18650 de 2500 mAh intercambiable:** una en una caja empotrable con tapa para 1 × 18650 (conectada a la placa de 7") y otra de repuesto en un cargador externo | Autonomía práctica ilimitada cambiando la celda en segundos, sin los riesgos del paralelo (celdas emparejadas, igualar tensiones, carga de ~9–10 h). |

**Decisiones sustituidas** (se conservan como historial):

| Fecha | Decisión original | Sustituida el | Por qué |
| :--- | :--- | :--- | :--- |
| 2026-11-07 | Placa ESP32-S3-DevKitC-1 (WROOM-1 N8R8, v1.1) como **placa principal** con las pantallas | 2026-11-08 | La placa de 7" trae su propia ESP32-S3. La DevKitC se reutiliza como **coprocesador**. |
| 2026-11-08 | Coprocesador Raspberry Pi Pico 2 W (PIO para los encoders, WiFi propio) | 2026-11-08 | El usuario ya tiene placas ESP32. La DevKitC N8R8 hace lo mismo (PCNT en lugar de PIO), sin comprar nada, sin soldar y en el mismo ecosistema que la placa de 7". |
| 2026-11-07 | Cuatro pantallas: OLED 2.42" (peso), OLED 1.54" (tiempo y ratio), OLED 1.3" (modo), TFT 1.47" (curva) | 2026-11-08 | No cumplían el requisito de leer sin gafas: la 1.3", la 1.54" y la TFT de 1.47" dan textos de 2–6 mm, y tres datos principales en grande más una gráfica no cabían. |
| 2026-11-07 | TP4056 + Schottky + divisor para la batería | 2026-11-08 | La placa de 7" integra el cargador (CS8501) y el elevador a 5 V. |
| 2026-11-08 | Sonda PT1000 + MAX31865 en versión PT1000 (4.3 kΩ) | 2026-11-08 | El módulo MAX31865 disponible trae la resistencia de 430 Ω (PT100). La PT100 a 3 hilos rinde igual y evita cambiar resistencias SMD o buscar un módulo especial. |
| 2026-11-08 | Dos 18650 en paralelo (5000 mAh) | 2026-11-08 | Se prefirió una celda intercambiable con repuesto: más simple y seguro. |
| 2026-11-08 | Placa Waveshare ESP32-S3-Touch-LCD-7 (sin "B", 800×480), de un revendedor | 2026-11-08 | Se compró la **7B** en la tienda oficial: más resolución, brillo regulable, medición de batería, salida de 5 V para el coprocesador, menos direcciones I2C ocupadas y menos consumo. |

---

## 7. Riesgos

| Riesgo | Mitigación |
| :--- | :--- |
| Desplazamiento o parpadeo de la imagen RGB por la competencia por la PSRAM | Bounce buffers; WiFi fuera de la S3; sin escrituras en la flash de la S3 durante el uso. Criterio explícito en la fase 1. |
| Choque de direcciones I2C con el táctil y el expansor | El coprocesador usa 0x42; en la 7B solo están ocupadas 0x24 y 0x5D. Se confirma con un escáner I2C en la fase 0. |
| Pérdida de pasos de los encoders | Conteo por hardware (PCNT) en el coprocesador; pasos acumulados hasta que la placa de 7" los lee. |
| Respuesta lenta del esclavo I2C del ESP32 mientras el WiFi está activo | Bloque de estado precargado con `slaveWrite`; la tarea de tiempo real en el núcleo 1 y el WiFi en el 0. Se mide en la fase 0. |
| Corriente insuficiente para el coprocesador desde la salida de 5 V de la placa | Medir en la fase 0. Si no basta, alimentarlo desde el conector de batería con un pequeño elevador a 5 V, o reducir su consumo (WiFi en modem-sleep). |
| Autonomía corta (~3.5–4 h activas por celda) | Batería intercambiable con repuesto cargado; brillo regulable, atenuado, apagado y reposo profundo; envíos a Home Assistant por lotes. |
| Conector de batería HY2.0 en lugar de PH2.0 | Mismo paso de 2.0 mm, normalmente compatibles; la placa trae sus propios cables. Verificar el encaje y la **polaridad** antes de conectar. |
| Brillo de 235 cd/m² escaso con luz fuerte | Ubicar el panel sin luz solar directa; brillo al máximo durante la preparación. |
| La celda de 2500 mAh excede la recomendación de Waveshare (< 2000 mAh) | Verificar la carga en la fase 0; si da problemas, usar una celda de ≤ 2000 mAh. |
| La Dot solo acepta una conexión | Opción de liberar la báscula; la fase 8 lo resuelve integrando el puente. |
| Comprar una sonda PT1000 para un MAX31865 de 430 Ω (o al revés) | Sonda **PT100** de 3 hilos con el MAX31865 estándar (430 Ω); verificar ambos al recibirlos (fase 0). |
| Higiene de la sonda en contacto con alimentos | Vaina de acero inox 316, conector de quita y pon para lavarla, cable de silicona apto para alimentos. |
| El calor y el vapor de la máquina deforman la caja | PETG o ABS; ubicarla lejos de la salida de vapor. |
| Batería de litio junto a una fuente de calor | Portapilas ventilado, cargador con protección y nunca encima de la máquina. |
| Recibir la variante sin táctil | Verificar al recibirla que sea la versión con táctil (Touch-LCD-7B). |

---

## 8. Referencias

- Placa de 7" (7B): <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B>
- ESP32-S3-DevKitC-1 (coprocesador): <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/> · ESP32Encoder (PCNT): <https://github.com/madhephaestus/ESP32Encoder>
- Driver de la Dot en de1app: <https://github.com/decentespresso/de1app/pull/345>
- Driver de la Dot en Beanconqueror: <https://github.com/graphefruit/Beanconqueror/blob/develop/src/classes/devices/timemoreDotScale.ts>
- Puente BLE (origen del driver reutilizado): <https://github.com/jcallano/BLE-_Scale_Translator_Bridge__Wendougee_E-Bar>
- LVGL: <https://lvgl.io/> · ESP32_Display_Panel: <https://github.com/esp-arduino-libs/ESP32_Display_Panel> · NimBLE-Arduino: <https://github.com/h2zero/NimBLE-Arduino>
- MAX31865 + PT1000/PT100 (tutorial DigiKey): <https://www.digikey.ca/fr/maker/tutorials/2021/getting-started-with-high-precision-temperature-sensing-using-a-max31865-and-pt1000> · Biblioteca: <https://github.com/adafruit/Adafruit_MAX31865>
- Home Assistant, MQTT Discovery: <https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery> · ArduinoHA: <https://github.com/dawidchyrzynski/arduino-home-assistant>
- Termómetros BLE soportados por Beanconqueror: <https://github.com/graphefruit/Beanconqueror/tree/develop/src/classes/devices>
- Sensor "Decent Temp" en Decaid: <https://github.com/decentespresso/decaid/blob/main/lib/src/models/device/impl/decent_temp/temperature.dart>
- Básculas abiertas de referencia: CaffePeso <https://www.home-barista.com/repairs/caffepeso-open-source-espresso-scale-t103525.html> · beeb/coffee-scale-app <https://github.com/beeb/coffee-scale-app>

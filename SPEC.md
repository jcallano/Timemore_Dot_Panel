# Timemore Dot Panel — Especificación del proyecto

Panel de control físico, independiente y de código abierto para la báscula **Timemore Black Mirror Dot**, que no tiene pantalla. Muestra el peso, el tiempo, el ratio, el caudal y la curva de extracción en varias pantallas, y se maneja con dos encoders giratorios. Sirve para **espresso** y para **filtrado (pour-over)**, sin apps, registro ni login. Funciona con batería recargable por USB-C.

| | |
| :--- | :--- |
| **Estado** | Especificación (fase 0 pendiente) |
| **Fecha** | 2026-11-07 |
| **Proyecto relacionado** | [BLE Scale Translator Bridge](https://github.com/jcallano/BLE-_Scale_Translator_Bridge__Wendougee_E-Bar): de él se reutiliza el driver BLE de la Dot, ya validado en hardware |

---

## 1. Objetivos y alcance

**Objetivos**
1. Leer la Dot por BLE y mostrar el peso con la menor latencia posible.
2. Repartir la información en varias pantallas según su importancia: el peso, muy grande; tiempo y ratio; modo y estado; la curva.
3. Guiar shots de espresso (dosis → ratio → objetivo) y preparaciones de filtrado (bloom, etapas y ritmo de vertido).
4. Funcionar con batería y cargarse con cualquier cargador USB-C.
5. Usar hardware fácil de conseguir (la mayoría ya está en la caja de componentes) y bibliotecas de código abierto.

**Fuera de alcance (por ahora)**
- Funcionar **a la vez** que el puente o que Decaid. La Dot acepta **una sola conexión BLE**. El panel es un dispositivo aparte, en su propia placa, así que el puente (ESP32-S3-Zero) sigue intacto, pero solo uno de los dos puede estar conectado a la Dot en cada momento. El panel tendrá una opción para **liberar la báscula**.
- Controlar la máquina de espresso. El panel avisa, pero no corta la extracción.
- Historial de shots en la nube, cuentas de usuario o apps móviles.

---

## 2. Hardware

### 2.1. Lista de materiales

| # | Componente | Uso | Interfaz | Estado |
| :---: | :--- | :--- | :--- | :--- |
| 1 | **Espressif ESP32-S3-DevKitC-1** con módulo **ESP32-S3-WROOM-1 N8R8** (8 MB de flash quad + 8 MB de PSRAM octal) | Microcontrolador: BLE, pantallas y control | — | Disponible |
| 2 | **OLED 2.42"** 128×64, SSD1309 | **Peso**, con números enormes | I2C *(confirmar)* | En la caja |
| 3 | **OLED 1.54"** 128×64, SSD1309 | **Tiempo y ratio en vivo** | I2C o SPI *(confirmar 4/7 pines)* | En la caja |
| 4 | **OLED 1.3"** 128×64, SH1106/SSD1306 | **Modo, dosis, objetivo, estado y batería** | I2C o SPI *(confirmar 4/7 pines)* | En la caja |
| 5 | **TFT 1.47"** 172×320, ST7789, a color | **Curva de peso y caudal** | SPI | En la caja |
| 6 | **2× encoder KY-040 / EC11** con pulsador | Ajustes (izquierdo) y acción (derecho) | GPIO | En la caja |
| 7 | **Módulo zumbador piezo del kit Arduino** (serigrafía "5V"; tipo pasivo o activo por identificar en la fase 0) | Avisos sonoros: tonos distintos si es pasivo, un solo tono si es activo | GPIO (PWM o ON/OFF) | En la caja |
| 8 | **Batería 18650 Li-ion 2500 mAh** + portapilas | Alimentación | — | Disponible |
| 9 | **Módulo TP4056 USB-C con protección** (6 pines: B+, B−, OUT+, OUT−; chips DW01A + 8205A) | Carga y protección de la batería | — | ~0.30–0.50 USD |
| 10 | **Diodo Schottky SS34** (o 1N5819) | Evita que la batería alimente "hacia atrás" el USB de la DevKitC | — | ~0.05 USD |
| 11 | **Interruptor deslizante** | Corte total de energía (almacenamiento) | — | ~0.10 USD |
| 12 | **2× resistencia 220 kΩ + condensador 100 nF** | Divisor para medir la batería | ADC | ~0.05 USD |
| 13 | **Resistencia 100 Ω** | En serie con el piezo | — | ~0.01 USD |

### 2.2. Distribución de las pantallas y controles (vista frontal)

```
      Vista frontal (~150 × 85 mm)
 ┌────────────────────────────────────────────┐
 │ ┌──────────────────────┐ ┌──────────────┐  │
 │ │  2.42" OLED  (PESO)  │ │ 1.54" OLED   │  │
 │ │       36.8 g         │ │ 0:27  1:1.84 │  │
 │ └──────────────────────┘ └──────────────┘  │
 │ ┌────────────┐ ┌─────────────┐             │
 │ │1.3" (MODO) │ │1.47" TFT    │   (◎)  (◎)  │
 │ │ESPRESSO 18g│ │ ╭─curva─╮   │ Ajust Acción│
 │ └────────────┘ └─────────────┘             │
 └────────────────────────────────────────────┘
```

### 2.3. Conexiones (ESP32-S3-DevKitC-1)

Pines evitados a propósito:
- **Strapping:** GPIO 0, 3, 45 y 46.
- **USB nativo:** GPIO 19 y 20.
- **Consola UART0:** GPIO 43 y 44.
- **PSRAM octal:** GPIO 35, 36 y 37. **Reservados en el N8R8**: no salen o no deben usarse.
- **LED RGB integrado:** GPIO 38 (la placa es la **revisión v1.1**). Se puede usar como LED de estado.

En el N8R8 la PSRAM octal funciona a 3.3 V, así que GPIO 47 y 48 trabajan a 3.3 V y quedan libres como reserva. En el arranque, las opciones del IDE deben ser **Flash Mode QIO 80 MHz**, **Flash Size 8 MB** y **PSRAM: OPI PSRAM**.

Las OLED por I2C suelen venir en `0x3C`, y algunas pueden cambiarse a `0x3D` con un puente de soldadura. Por eso se usan los **dos controladores I2C** del ESP32-S3.

| Función | Señal | GPIO | Notas |
| :--- | :--- | :---: | :--- |
| **I2C-0** (OLED 2.42" `0x3C` + OLED 1.54" `0x3D`) | SDA / SCL | **8 / 9** | 400 kHz–1 MHz |
| **I2C-1** (OLED 1.3" `0x3C`) | SDA / SCL | **17 / 18** | |
| **TFT 1.47"** (SPI) | SCLK / MOSI | **12 / 11** | |
| | CS / DC / RST | **10 / 13 / 14** | |
| | BLK (luz de fondo) | **21** | PWM para el brillo y la atenuación |
| **Encoder izquierdo "Ajustes"** | CLK / DT / SW | **4 / 5 / 6** | SW es un pin RTC: despierta del reposo profundo |
| **Encoder derecho "Acción"** | CLK / DT / SW | **15 / 16 / 7** | SW es un pin RTC: despierta del reposo profundo |
| **Piezo** | PWM (LEDC) | **2** | con 100 Ω en serie, a GND |
| **Batería** (divisor 220k/220k + 100 nF) | ADC1_CH0 | **1** | lee VBAT/2 (≤ 2.1 V) |
| **LED RGB integrado** (WS2812) | Datos | **38** | Opcional: estado de BLE, carga o aviso |
| **Libres para ampliaciones** | — | 39, 40, 41, 42, 47, 48 | |

Todos los módulos se alimentan a **3.3 V**, incluidos los encoders KY-040. Así sus resistencias pull-up no meten 5 V en los GPIO.

> Si alguna OLED es de 7 pines (SPI), comparte SCLK y MOSI con la TFT y usa su propio CS (GPIO 39–42). Ese bus I2C quedaría libre.

### 2.4. Alimentación

```
USB-C ─► TP4056 (con protección) ─OUT+─► Interruptor ─► Schottky SS34 ─► pin 5V de la DevKitC
                │                                                         (regulador 3.3 V integrado)
              18650 ── B+ ── divisor 220k/220k + 100 nF ──► GPIO 1 (ADC)
```

- **Carga:** con cualquier cargador o puerto **USB-C** conectado al TP4056, a 1 A. Una 18650 de 2500 mAh se carga en ~3 h. El LED del módulo indica si está cargando (rojo) o llena (azul/verde).
- **Protección:** el TP4056 con DW01A corta por sobrecarga (4.2 V), sobredescarga (~2.5 V) y cortocircuito.
- **Autonomía:** el consumo estimado es de 150–200 mA (BLE ~85 mA, 3 OLED, luz de la TFT y el chip USB-serie de la DevKitC). Eso da **~12–16 h de uso activo**. Con el apagado automático (sección 4.4) se traduce en semanas de uso doméstico.
- **Tensión útil:** el regulador de la DevKitC necesita ~3.5 V a la entrada, así que se aprovecha ~85–90 % de la capacidad. El firmware mostrará un **aviso de batería baja a 3.55 V** y apagará las pantallas a **3.45 V**.
- **Diodo Schottky:** impide que la batería empuje corriente hacia el USB de la DevKitC. Permite **programar por USB con la batería conectada** sin riesgo.
- **Limitaciones conocidas:**
  - El TP4056 no gestiona bien cargar y alimentar a la vez: con el panel encendido mientras carga, la carga puede no llegar a terminar. Recomendación: **cargar con el interruptor apagado**. Mejora opcional: un MOSFET-P para compartir carga.
  - Con la placa en reposo profundo siguen consumiendo el LED de encendido y el chip USB-serie (del orden de mA). Para guardarlo mucho tiempo, usar el **interruptor**.
- **Pendiente:** confirmar en el esquema de la DevKitC-1 que el pin 5V conecta con la entrada del regulador. Es lo habitual en esta placa.

### 2.5. Diseño exterior

- **Forma:** consola tipo **cuña**, con el frente inclinado ~20° para leerla de pie junto a la máquina.
- **Medidas aproximadas:** **150 × 85 mm** de frente y **~45 mm** de profundidad. La DevKitC (~69 × 25 mm) y el portapilas 18650 (~77 × 21 mm) van detrás del panel frontal, uno junto al otro.
- **Encoders** a la derecha, separados ~30 mm, con perillas EC11. Se pueden girar con el pulgar y el índice sin tapar las pantallas.
- **USB-C de carga** (TP4056) e **interruptor** en el lateral, lejos de la bandeja de goteo. Opcional: ventana para ver el LED de carga.
- **Material:**
  - Impresión 3D en **PETG** en la **Creality K1 Max** (cerrada; ABS como alternativa si hiciera falta más resistencia al calor). No usar PLA: junto a la máquina hay calor y vapor, y el PLA se deforma a ~55 °C.
- **Montaje de las pantallas:** cada módulo atornillado por detrás del frontal (M2/M2.5) con la ventana al tamaño del área activa. El orden de las capas (frontal → pantallas → soporte de PCB) se define en la fase 4.

---

## 3. Arquitectura de software

### 3.1. Bibliotecas (todas de código abierto)

| Biblioteca | Licencia | Uso |
| :--- | :--- | :--- |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.x | Apache-2.0 | Cliente BLE de la Dot |
| [U8g2](https://github.com/olikraus/u8g2) | BSD-2 | Las tres OLED (SSD1309, SH1106, SSD1306) con fuentes numéricas grandes |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | FreeBSD | TFT ST7789 172×320 y la gráfica |
| Arduino-ESP32 3.3.x | LGPL-2.1 | Plataforma |

LVGL se descarta: con OLED monocromas y una sola gráfica, U8g2 + LovyanGFX es más simple y ligero.

### 3.2. Tareas y núcleos (mismo patrón que el puente)

| Núcleo | Tarea | Responsabilidad |
| :---: | :--- | :--- |
| 0 | `nimble_host` (stack) | Callbacks BLE: publica cada muestra con su marca de tiempo. |
| 1 | `scale_link` | Escaneo, conexión, secuencia de inicio de la Dot, sondeo cada 10 s, tara física, liberar la báscula. |
| 1 | `ui` | Lee los encoders (por interrupciones y máquina de estados), lleva el estado de cada modo, redibuja las pantallas y hace sonar el piezo. |
| 1 | `power` (en `ui` o `loop`) | Mide la batería, gestiona la atenuación y el apagado de las pantallas y el reposo profundo. |

**Frecuencias de refresco:**
- Peso (2.42"): 10–20 Hz, al ritmo de la Dot (10 Hz).
- Tiempo y ratio (1.54"): 10 Hz.
- Modo, estado y batería (1.3"): 2 Hz o al cambiar.
- Curva (TFT): 10 Hz, añadiendo un punto por muestra (no se redibuja entera).

### 3.3. Driver de la Dot (reutilizado del puente)

Validado en hardware el 2026-11-07. Se basa en las tramas de de1app, [PR #345](https://github.com/decentespresso/de1app/pull/345):
- Servicio `0xFFF0`: notificaciones en `0xFFF1`, comandos en `0xFFF2`.
- Peso: tipo `0x01` en el byte 2, `int16` big-endian en los bytes 8–9, en 0.1 g.
- Secuencia de inicio de la app oficial (dos pasadas), sondeo cada 10 s y tara `A55A030D000200000071`.
- Bonding Just-Works, MAC guardada en NVS y reconexión automática.
- Caudal: EMA calculado con la hora real de llegada de cada muestra.

---

## 4. Interfaz de usuario

### 4.1. Controles

| Control | Girar | Clic | Doble clic | Pulsación larga (> 1 s) |
| :--- | :--- | :--- | :--- | :--- |
| **Encoder izquierdo "Ajustes"** | Cambia el modo (en el menú) o el valor del campo activo | Pasa al siguiente campo (dosis → ratio → objetivo) | — | Abre o cierra el menú de modos |
| **Encoder derecho "Acción"** | Ajuste fino del objetivo **durante** el shot (±0.1 g) | **Tara** | Inicia o para el timer | Reinicia el timer |

**Despertar:** cualquier clic de un encoder despierta el panel del reposo profundo.

### 4.2. Modos

**Manual**
- Peso, tiempo y caudal. Timer con el encoder derecho.

**Espresso**
- Ajustes: dosis (g), ratio (1:1.5–1:3) y peso objetivo calculado (dosis × ratio). Se pueden fijar a mano.
- El timer arranca **solo** con la primera gota (el peso sube más de 0.3 g tras la tara).
- Se muestra el **ratio en vivo** (peso ÷ dosis) junto al objetivo.
- **Aviso al llegar al objetivo:** pitido, y la pantalla se invierte o parpadea. Usa una compensación del retardo configurable (por defecto 0.5 s × caudal actual), para avisar *antes* según lo que seguirá cayendo.
- El timer se detiene solo cuando el caudal baja de ~0.2 g/s.

**Filtrado (pour-over)**
- Ajustes: dosis, ratio (1:15–1:17) → agua total. También agua y tiempo del bloom, y número de etapas.
- **Bloom:** objetivo de agua y cuenta atrás, con pitido al terminar.
- **Etapas:** objetivo acumulado de cada vertido y pitido al alcanzarlo.
- **Banda de caudal:** indica si se vierte demasiado rápido o despacio respecto a un rango (p. ej. 4–6 g/s).
- Tiempo total y objetivo de tiempo final.

**Liberar báscula** (en el menú)
- Desconecta el panel de la Dot para que el puente o Decaid puedan conectarse. Se reconecta con un clic.

### 4.3. Sonidos (piezo)

| Evento | Sonido |
| :--- | :--- |
| Tara, clic de confirmación | Clic corto (2 kHz, 20 ms) |
| Fin del bloom / etapa alcanzada | Dos pitidos (2.5 kHz) |
| Objetivo de espresso alcanzado | Pitido largo (3 kHz, 300 ms) |
| Batería baja | Tres pitidos graves |
| Se puede silenciar desde el menú | — |

### 4.4. Energía y protección de las OLED

- **Atenuado** a los 2 min sin cambios de peso ni uso de los encoders, y **pantallas apagadas** a los 5 min.
- **Reposo profundo** a los 10 min: BLE desconectado y despertar con un clic de encoder. Se reconecta a la Dot en ~1–2 s.
- Desplazamiento de 1–2 píxeles del contenido estático cada pocos minutos, contra el quemado.
- La batería (% y aviso) se muestra en la OLED de 1.3".

---

## 5. Desarrollo por fases

Cada fase termina con un firmware que funciona, probado con la Dot real, y con un commit.

### Fase 0 — Identificación del hardware y montaje en protoboard
- Revisar cada OLED (4 o 7 pines, puente de dirección), escanear los dos buses I2C.
- Probar cada pantalla con un sketch mínimo de U8g2 o LovyanGFX, los dos encoders, el piezo y la lectura de la batería por ADC.
- Montar el circuito de alimentación (TP4056 → interruptor → Schottky → 5V) y medir el consumo real.
- **Entregable:** tabla de conexiones definitiva (sección 2.3) y consumo medido (sección 2.4).
- **Criterio de aceptación:**
  - las 4 pantallas muestran texto a la vez;
  - los 2 encoders se leen sin saltos ni rebotes;
  - el zumbador suena y queda identificado como pasivo o activo;
  - el % de batería coincide con la tensión medida con multímetro (±0.05 V).

### Fase 1 — Modo manual (núcleo funcional)
- Integrar el driver de la Dot del puente: conexión, inicio, sondeo y tara.
- Peso en la 2.42", tiempo y caudal en la 1.54", y estado de la conexión y batería en la 1.3".
- Curva en tiempo real en la TFT.
- Tara con el clic del encoder derecho; timer con doble clic.
- **Criterio de aceptación:**
  - el peso en pantalla coincide con objetos de peso conocido;
  - la pantalla refleja la muestra en menos de 50 ms desde que llega (medido por serial);
  - la tara física funciona: tarar con un objeto y retirarlo da un peso negativo.

### Fase 2 — Modo espresso
- Menú de modos y ajustes con el encoder izquierdo (dosis, ratio, objetivo).
- Timer automático con la primera gota, ratio en vivo, aviso de objetivo con pitido y compensación, y parada automática.
- **Criterio de aceptación:** en un shot real, el aviso llega a ±1 g del objetivo con la compensación por defecto.

### Fase 3 — Modo filtrado
- Bloom con cuenta atrás, etapas de vertido con objetivos acumulados, banda de caudal y avisos sonoros.
- **Criterio de aceptación:** una receta de 2 etapas se guía de principio a fin sin tocar nada salvo el inicio.

### Fase 4 — Energía, persistencia y caja
- Atenuado, apagado y reposo profundo; despertar por encoder; avisos de batería baja.
- Guardar en NVS los últimos ajustes y recetas (p. ej. 3 de espresso y 3 de filtrado) y la preferencia de sonido.
- Opción de liberar la báscula.
- Diseño de la caja para impresión 3D en PETG (K1 Max): archivos fuente editables (FreeCAD u OpenSCAD) y STL en el repo.
- **Criterio de aceptación:**
  - el panel pasa a reposo profundo y despierta y reconecta en menos de 3 s;
  - la autonomía medida con uso típico es de al menos 10 h activas.

### Fase 5 (opcional) — Panel y puente en el mismo dispositivo
- Añadir al panel la función de puente (emulación BooKoo hacia Decaid). Así el panel muestra el peso **mientras** Decaid controla la parada por peso, y desaparece la limitación de una sola conexión a la Dot. La DevKitC tiene recursos de sobra para ambos roles.

---

## 6. Decisiones pendientes

| # | Pregunta | Afecta a |
| :---: | :--- | :--- |
Las tres se resuelven con pruebas físicas en la **fase 0**.

| # | Pregunta | Cómo se resuelve | Afecta a |
| :---: | :--- | :--- | :--- |
| 1 | ¿Las OLED de 1.54" y 1.3" son de 4 pines (I2C) o de 7 (SPI)? | Contar los pines de cada módulo | Conexiones (2.3) |
| 2 | ¿Qué dirección I2C tiene cada OLED y cuáles pueden cambiarse a `0x3D`? | Sketch escáner I2C + revisar el puente `0x78`/`0x7A` del dorso | Distribución de los buses I2C |
| 3 | ¿El zumbador del kit es pasivo o activo? | Mirar el dorso (PCB visible = pasivo; resina = activo) o alimentarlo sin señal (pitido continuo = activo) | Sonidos (4.3) |

### Decisiones tomadas

| Fecha | Decisión | Motivo |
| :--- | :--- | :--- |
| 2026-11-07 | Módulo **ESP32-S3-WROOM-1 N8R8** (8 MB de flash + 8 MB de PSRAM octal), placa **revisión v1.1** (LED RGB en GPIO 38) | Es el que tiene la placa disponible. GPIO 35–37 reservados por la PSRAM; la PSRAM sirve para el histórico de la curva y el búfer de la TFT. |
| 2026-11-07 | Placa: **ESP32-S3-DevKitC-1** (no la ESP32-S3-Zero ni el XIAO ESP32-S3) | Más de 30 GPIO, recorrido a largo plazo, permite la fase 5. El XIAO base cabía con 0 pines de reserva y su cargador (50–100 mA) es lento para una 18650. |
| 2026-11-07 | **Dos encoders**: "Ajustes" y "Acción" | La tara y el timer quedan a un clic durante la extracción, sin navegar por menús. |
| 2026-11-07 | **Batería 18650 de 2500 mAh + TP4056 USB-C con protección** | Batería universal y reemplazable; módulo de carga barato y fácil de encontrar; carga por USB-C a 1 A. |
| 2026-11-07 | **Zumbador: el módulo piezo del kit Arduino**, alimentado a 3.3 V | Ya está en la caja. Funciona a 3.3 V (algo menos de volumen). Si es pasivo, tonos distintos por PWM; si es activo, un solo tono. |
| 2026-11-07 | **Caja impresa en 3D** en la **Creality K1 Max**, en **PETG** | Impresora cerrada: PETG sin problemas (y ABS si hiciera falta más resistencia al calor). La caja (~150 × 85 × 45 mm) cabe de sobra en su volumen de 300 mm. |
| 2026-11-07 | **Cuatro pantallas**: OLED 2.42" (peso), OLED 1.54" (tiempo y ratio), OLED 1.3" (modo y estado), TFT 1.47" (curva) | Aprovecha los módulos de la caja; separa la información por importancia. |

---

## 7. Riesgos

| Riesgo | Mitigación |
| :--- | :--- |
| Choque de direcciones I2C | Dos buses I2C o cambiar a `0x3D` con el puente de soldadura. |
| Quemado de las OLED | Atenuado, apagado y desplazamiento de píxeles (fase 4). |
| Rebotes del encoder KY-040 | Decodificación por máquina de estados con interrupciones y antirrebote por software. |
| La Dot solo acepta una conexión | Opción de liberar la báscula; la fase 5 lo resuelve integrando el puente. |
| El TP4056 no gestiona bien cargar y alimentar a la vez | Cargar con el interruptor apagado; MOSFET-P opcional para compartir carga. |
| Consumo en reposo de la DevKitC (LED de encendido y chip USB-serie) | Interruptor de corte; opcionalmente, quitar el LED de encendido. |
| El calor y el vapor de la máquina deforman la caja | PETG o ABS; ubicarla lejos de la salida de vapor. |
| Batería de litio junto a una fuente de calor | Módulo con protección, portapilas ventilado y nunca encima de la máquina. |

---

## 8. Referencias

- Driver de la Dot en de1app: <https://github.com/decentespresso/de1app/pull/345>
- Driver de la Dot en Beanconqueror: <https://github.com/graphefruit/Beanconqueror/blob/develop/src/classes/devices/timemoreDotScale.ts>
- Puente BLE (origen del driver reutilizado): <https://github.com/jcallano/BLE-_Scale_Translator_Bridge__Wendougee_E-Bar>
- ESP32-S3-DevKitC-1 (guía de usuario y esquemas): <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/>
- U8g2: <https://github.com/olikraus/u8g2> · LovyanGFX: <https://github.com/lovyan03/LovyanGFX> · NimBLE-Arduino: <https://github.com/h2zero/NimBLE-Arduino>

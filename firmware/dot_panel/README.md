# dot_panel: firmware de la placa de 7"

Firmware principal de la Waveshare ESP32-S3-Touch-LCD-7B. Une dos piezas:
- la **interfaz** (LVGL 8.4), que aplica las reglas de rendimiento de `ui_test_01`;
- el **puente BLE** del proyecto hermano, portado a esta placa.

**Estado:** v0.1.0 (2026-11-22). Compila, arranca y se probó en hardware **sin la Dot**, con datos simulados y con un cliente que imita a la Wendougee. **Falta la prueba con la Dot real y con la Wendougee real.**

## Modos

Se eligen con el botón `MENÚ`.

| Modo | Qué hace | Puente BooKoo |
| :--- | :--- | :--- |
| **ESPRESSO** | Receta: dosis y ratio, de los que sale el objetivo. El timer arranca solo con la primera gota (más de 0,3 g) y se para cuando el caudal baja de 0,2 g/s. El marco de color avisa con antelación usando 0,5 s de caudal: ámbar al acercarse, verde en el objetivo, rojo al pasarse. | Apagado |
| **POUR OVER** | Receta: dosis, ratio (agua total), agua y tiempo del bloom. El timer arranca con el primer vertido (más de 2 g). Durante el bloom hay cuenta atrás y aviso con el agua del bloom; después, aviso con el agua total. | Apagado |
| **WENDOUGEE** | El panel se anuncia como **`BOOKOO_SC_U XXXXXX`** y reenvía la Dot a la máquina, como el puente. Muestra peso, caudal y ratio. Si la máquina maneja el timer, el panel lo sigue. | **Activo** |

**Pantalla principal:**
- **Números:** peso, tiempo y ratio (con el rótulo `RATIO 1:`). Si el peso pasa de 100 g, se muestra sin decimales.
- **Secundarios:** caudal, temperatura (`--.-` hasta que haya coprocesador) y objetivo o bloom.
- **Gráfica:** peso y caudal mientras corre el timer. Cuando se llena, se comprime a la mitad.
- **Estado:**
  - `DOT`: verde si está conectada, ámbar si la busca y rojo si está liberada;
  - `WEN`: solo en el modo WENDOUGEE; verde si la máquina está conectada.
- **Botones:**
  - `TARA`: tara inmediata por software más la tara física en la Dot;
  - `INICIO` / `PARAR` / `CERO`: controla el timer;
  - `RECETA`: abre los parámetros del modo;
  - `MENÚ`.
- Tocar la gráfica muestra u oculta la línea de depuración.

**AJUSTES:**
- **Pantalla:** brillo, diseño A (tres números en fila) o B (peso grande) y depuración.
- **Báscula:** estado, liberar o reconectar la Dot (para que otra app pueda conectarse) y olvidarla (hay que tocar dos veces para confirmar).
- **Timer:** inicio y parada automáticos.
- **Actualizar:** versión del firmware. La OTA por BLE está pendiente (ver SPEC).

Los ajustes se guardan en la NVS solo al salir de una pantalla de ajustes, y solo si cambiaron.

## Arquitectura

| Archivo | Contenido |
| :--- | :--- |
| `dot_panel.ino` | Interfaz, modos, recetas, timer y ajustes |
| `ble_bridge.cpp/.h` | Puente BLE: el `Bascula_Bridge_ESP32S3.ino` del repo hermano (commit `f649bba`, validado en hardware) con los cambios marcados `PANEL:` y una API para la interfaz |
| `ui_types.h` | Tipos de la interfaz (Arduino genera los prototipos antes de los `struct` del `.ino`) |
| Drivers | Los de `ui_test_01`: dibujo en SRAM interna y un solo framebuffer |

- Las tareas del puente no cambian: `bridge_tx` (núcleo 0) y `scale_link` (núcleo 1).
- La interfaz lee las muestras con un temporizador de LVGL de 10 ms, que compara el número de secuencia.
- **Cambios del puerto:**
  - sin LED RGB ni botón BOOT, porque **en la 7B los GPIO 0 y 21 son líneas de datos de la pantalla**;
  - `setup()` pasa a ser `bridge_begin()` y `loop()` pasa a ser `bridge_poll()`;
  - el anuncio BooKoo solo está activo en el modo WENDOUGEE;
  - se añaden "liberar" y "reconectar" la báscula.

**Memoria:** el pool de LVGL (128 KB) se movió a PSRAM con `LV_MEM_POOL_ALLOC` en el `lv_conf.h` global. Con el pool en la RAM interna, NimBLE no arrancaba por falta de memoria. Con el BLE activo quedan **~105 KB de RAM interna libres**, y el refresco se mantiene en ~12–30 ms.

## Compilar y probar

FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default,UploadSpeed=230400`. Se sube por COM16.

**Puerto serie a 921600 baudios.** Comandos:

| Comando | Acción |
| :--- | :--- |
| `D` / `d` | Captura del framebuffer (pantalla completa o franja inferior), que recibe [`tools/fbdump.ps1`](../../tools/fbdump.ps1) |
| `C` | Comprueba los recortes en todas las pantallas, en los dos diseños, en cada modo y con los textos del peor caso. Avisa de lo que se sale de la pantalla, queda bajo el marco de alerta, no cabe en su etiqueta o botón o se solapa con otro elemento. Debe terminar en `fin: 0 problema(s)` |
| `S` | Activa o desactiva los datos simulados (sin Dot) |
| `M` | Pasa al modo siguiente |
| `n` | Pasa a la pantalla siguiente |
| `?` `l` `t` `r` `s` `p` | Comandos del puente: estado, enlaces y latencia, tara, olvidar báscula, escanear, pausar |
| `w` `q` `x` `R` | Medición de la cola de la Dot y de la latencia completa: volcado de muestras, respuestas, toques y píxeles (`w`); sonda (`q`); liberar o reconectar (`x`); reinicio (`R`). Ver [`docs/DOT_COLA_BLE.md`](../../docs/DOT_COLA_BLE.md) |

**Trato de la Dot:** MTU 247, lectura de varias tramas por notificación, sin secuencia de inicio, un sondeo cada 25 s y tara solo por software. Sin el MTU grande, el retraso del peso crece hasta ~2,4 s. Ver [`docs/DOT_COLA_BLE.md`](../../docs/DOT_COLA_BLE.md).

Cada 5 s imprime `[PANEL]` con el tiempo de refresco, la **latencia muestra → pantalla** (desde que llega la trama BLE de la Dot hasta que está en el framebuffer) y la RAM libre.

**Probar el modo WENDOUGEE sin la máquina:** carga [`tools/fake_wendougee`](../../tools/fake_wendougee/fake_wendougee.ino) en otra ESP32-S3 (por ejemplo, la DevKitC).
- Busca `BOOKOO_SC_U`, se conecta, se suscribe a 0xFF11 e imprime las tramas.
- Probado el 2026-11-22: conecta, recibe tramas BooKoo válidas y el panel muestra `WEN` en verde.

## Pendiente

- **Probar con la Dot real:**
  - el peso frente a pesos conocidos;
  - la tara física;
  - **la latencia muestra → pantalla, que debe ser menor de 50 ms** (criterio de la fase 1);
  - 30 minutos sin desplazamientos de imagen ni parpadeos.
- **Probar con la Wendougee real.** El nombre BooKoo del panel es distinto del que tenía el puente, así que hay que volver a elegir la báscula en la máquina.
- **Pitidos y temperatura,** que llegarán con el coprocesador por I2C.
- **OTA por BLE,** desde una página web con Web Bluetooth (ver SPEC).

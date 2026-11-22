# ui_test_01: prueba de interfaz en la Waveshare ESP32-S3-Touch-LCD-7B

Esta prueba sirve para decidir dos cosas antes de escribir el firmware definitivo:

- si los números se leen **sin gafas**;
- si **LVGL 8.4 tiene rendimiento suficiente** en esta placa.

Simula en bucle un espresso (18 g → 36 g) a 10 Hz, el mismo ritmo al que la Dot envía el peso.

**Estado:** v0.6, validada en hardware el 2026-11-22.
- La pantalla se lee bien, sin parpadeo y sin tearing apreciable.
- Cada refresco tarda unos 30 ms.
- La latencia del peso (dato nuevo → escrito en el framebuffer) es de unos 30 ms de media. El panel tarda entre 0 y 30 ms más en mostrarlo.

## Qué muestra

- **Peso, tiempo y ratio** con la fuente Bebas Neue, cuyos dígitos tienen todos el mismo ancho. Hay dos layouts; el botón `A / B` cambia entre ellos:
  - **A:** los tres números en una fila, con dígitos de 140 px (unos 21 mm);
  - **B:** el peso con dígitos de 224 px (unos 34 mm), y el tiempo y el ratio a unos 21 mm.
- **Caudal** en cian y **temperatura** en naranja, a unos 10 mm, y el **objetivo**.
- **Gráfica** de peso (blanco), caudal (cian) y temperatura (naranja), con trazo de 4 px.
- **Aviso por color** con un marco de 14 px: ámbar al acercarse al objetivo, verde en el objetivo y rojo al pasarse.
- **Botones:**
  - `INICIO` reinicia la simulación;
  - `A / B` cambia de layout;
  - `BR -` y `BR +` regulan el brillo a través del expansor de E/S.
- **Línea de depuración** sobre la gráfica, que se muestra u oculta tocando la gráfica:
  - tiempo de refresco y refrescos por segundo;
  - CPU de LVGL;
  - batería, leída por el ADC del expansor;
  - brillo.

## Compilar y flashear

Requisitos:
- Arduino IDE 2.x o `arduino-cli`;
- core **esp32 3.3.12**;
- biblioteca **lvgl 8.4.0**.

Hay que poner el interruptor UART1/UART2 de la placa en **UART1** y conectar el USB-C marcado "UART" (chip CH343).

```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default,UploadSpeed=230400" .
arduino-cli upload -p COM16 --fqbn "<la misma>" .
```

- **Velocidad de subida:** a 921600 baudios la subida falla; a 230400 funciona. Si no entra sola en modo de descarga: mantener BOOT, pulsar RESET y soltar BOOT.
- **`lv_conf.h`:** se usa el de Waveshare (está en la carpeta `libraries` de Arduino) con **un solo cambio**: `LV_MEM_SIZE` pasa de 48 KB a **128 KB**.
- **Puerto serie de la aplicación:** **921600 baudios**. Cada segundo imprime dos líneas:
  - `[PERF]`: refresco, CPU, memoria y batería;
  - `[DET]`: píxeles por refresco, tiempo de copia, tiempo de dibujo y latencia.

### Capturas de pantalla

Enviando `d` (franja de la gráfica) o `D` (pantalla completa) por el puerto serie, el firmware vuelca el framebuffer. [`tools/fbdump.ps1`](../../tools/fbdump.ps1) lo recibe y lo convierte a PNG:

```
powershell -File tools\fbdump.ps1 -Port COM16 -Cmd D -OutDir capturas
```

## Lo que se aprendió (v0.1 → v0.6)

| Versión | Cambio | Refresco | Resultado |
| :--- | :--- | :--- | :--- |
| 0.1 | Ejemplo de Waveshare: modo antiparpadeo 3 (dos framebuffers en PSRAM, LVGL dibuja directamente sobre ellos), `lv_chart`, fondo de color | 75–120 ms | Lento. Cambiar el color de fondo redibuja la pantalla entera (200–250 ms). |
| 0.2 | La gráfica pasa a un `lv_canvas` que solo invalida el segmento nuevo; el aviso es un marco en vez del fondo | sin cambios | **El coste no depende del área dibujada.** |
| 0.3 | Panel RGB creado en el núcleo 0 | ~80 ms | **Mucho parpadeo.** Descartado. |
| 0.4 | Instrumentación y volcado del framebuffer | — | El cuello de botella es **dibujar en la PSRAM** (~1,5 Mpx/s), porque compite con la lectura del bounce buffer (60 MB/s). Además apareció una **gráfica fantasma**: el canvas crecía con su hija y `lv_img` repetía la imagen en mosaico. Se corrige con tamaño fijo. |
| 0.5 | Dibujo en un **buffer de 1024 × 30 px en SRAM interna** (60 KB) y un solo framebuffer | ~45 ms | El doble de rápido, sin parpadeo y sin tearing visible. |
| 0.6 | **Números dígito a dígito** (solo se redibuja la cifra que cambia), secundarios a ~3 Hz, refresco de LVGL cada 15 ms | **~30 ms** | Latencia del peso de ~30 ms; 10 actualizaciones por segundo, sin perder ninguna. |

**Reglas para el firmware definitivo:**
- dibujar en SRAM interna, nunca directamente en la PSRAM;
- no repintar áreas grandes: nada de cambiar el fondo completo ni de `lv_chart` con muchos puntos;
- actualizar los números por dígitos;
- dar **tamaño fijo** a los objetos que contienen imágenes.

## Licencias

- **Fuente Bebas Neue** (Dharma Type): SIL Open Font License 1.1 (ver [FONT_LICENSE_OFL.txt](FONT_LICENSE_OFL.txt)). Los archivos `font_bebas_*.c` se generaron con [lv_font_conv](https://github.com/lvgl/lv_font_conv):
  ```
  npx lv_font_conv --font BebasNeue-Regular.ttf --size 320 --bpp 4 --format lvgl --symbols "0123456789.:- " --no-compress -o font_bebas_320.c --lv-font-name font_bebas_320
  ```
  - También a tamaño 200 con los mismos símbolos.
  - A tamaño 96 y 56 con `-r 0x20-0x7E -r 0xB0` (ASCII completo y el símbolo de grado).
  - En todos los casos hay que cambiar después `#include "lvgl/lvgl.h"` por `#include "lvgl.h"`.
- **Drivers** (`gt911`, `i2c`, `io_extension`, `lvgl_port`, `rgb_lcd_port`, `touch`): son del ejemplo oficial de Waveshare (14_LVGL_BTN), con los cambios indicados en la cabecera de `ui_test_01.ino`.

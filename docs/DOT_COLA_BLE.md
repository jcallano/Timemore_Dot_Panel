# La cola de salida de la Timemore Dot: cómo tratarla

Referencia para cualquier firmware que hable con la Dot por BLE (este panel, el puente y la versión OLED).
Se investigó el 2026-11-23 con la Dot real, el panel de 7" y la app oficial de Timemore en Android.

## Resumen: las reglas

1. **Negociar un MTU grande con la Dot (247)**, pedido **después** de suscribirse a `0xFFF1`, no durante `connect()`.
2. **Leer todas las tramas `A5 5A` de cada notificación** y usar la de peso más reciente.
3. **Mandarle a la Dot los mínimos comandos posibles:**
   - sin secuencia de inicio al conectar;
   - un único sondeo ligero cada 25 s (consulta de batería `A5 5A 02 05 00 00 00 00`), imprescindible para que no se apague;
   - **tara solo por software** (nada de `A5 5A 03 0D …`).
4. **Solo es peso la trama `A5 5A 01 01 …`.** `A5 5A 01 05 …` es el aviso de batería que la Dot envía sola cada ~30 s.

Si se rompe la regla 1, el retraso del peso crece con el tiempo hasta ~2,4 s, aunque se cumplan las demás.

## El problema

**Síntoma:** recién conectada, la Dot responde rápido; con los minutos, el peso llega cada vez más tarde, hasta 2–3 s
después de poner o quitar un objeto. Lo mismo pasaba en la versión OLED. La app oficial no lo sufre.

**Causa:**
- La Dot envía **como máximo una notificación cada 100 ms** y la saca de una cola de salida.
- En esa cola entran las tramas de peso (10 por segundo), **las respuestas a cada comando nuestro** y **un aviso de
  batería espontáneo cada ~30 s**.
- Con el **MTU por defecto (23 bytes → 20 útiles)** cabe una sola trama de peso (17 bytes) por notificación. Cada trama
  extra ocupa un hueco y retrasa todas las siguientes **100 ms, para siempre**: la cola solo crece, hasta llenarse (~2,4 s).
- **La Dot junta varias tramas en una notificación cuando caben.** Se vio con MTU 23 (`A5 5A 02 05 … A5 5A 02 02 …`,
  19 bytes). La app de Android negocia MTU grande y paquetes de 251 bytes, y así la Dot vacía la cola en cuanto se forma.
- El puente heredado de de1app y la versión OLED conectaban con `exchangeMTU = false` (para evitar el error 0x3E/574 en los
  primeros eventos de conexión) y nunca negociaban el MTU después.

**Por qué no se veía como un filtro:** el peso no llegaba suavizado, sino **desplazado en el tiempo**. Un escalón de
0 a 63 g llegaba completo en ~300 ms (lo que tarda la taza en asentarse), pero 2 s tarde. Eso es una cola (FIFO), no un filtro.

## Mediciones clave

| Situación | Retraso medido | Cómo se midió |
| :--- | :--- | :--- |
| Puente heredado tras 20 min (secuencia de inicio, sondeo de de1app cada 10 s, tara física) | **2,4 s**, estable (cola llena) | Tara enviada → primera trama con 0 g |
| Igual, toque en pantalla al apoyar la taza | 1,8–2,3 s | Toque → primera trama con peso |
| Respuestas a comandos desde el arranque | 0,9 → 1,3 → 1,5 → 1,7 → 1,8 s en 30 s | Comando → su respuesta |
| Sin inicio ni sondeo | 0,33 → 0,44 → 0,59 s | Idem; además la Dot **se apagó sola a los ~70 s** |
| Modo OLED (sondeo de batería cada 25 s, sin inicio) | +0,2 s cada 25–30 s | Sondeo → respuesta; toque → peso coincidía (±0,1 s) |
| Intervalo 52,5 ms (el de la app oficial) | crece igual | Sondas cada 5 s |
| 2 min sin enviar nada | +0,43 s (≈ 4 tramas extra × 0,1 s) | Confirma "cada trama extra = +100 ms" |
| **MTU 247 + lectura de varias tramas** | **~0,1 s siempre**, incluso con 15 comandos en 4 s | La Dot juntó 19 tramas extra y volvió a ~0,1 s |

**Descartado:**
- cambiar el intervalo de conexión;
- reconectar limpiamente, incluso con 8 s de pausa o con el motivo 0x15 ("apagado"): la cola se conserva;
- desactivar y reactivar las notificaciones: empeora (+0,27 s).

Solo vaciaban la cola un **corte brusco** (reinicio del ESP32, la Dot lo detecta por timeout) o **apagar la Dot**.

**La app oficial**, vista con el resumen HCI de Android (`dumpsys bluetooth_manager`, sección `BTSNOOP_LOG_SUMMARY`):
- pide un intervalo de 30–60 ms y acaba en 52,5 ms;
- negocia paquetes de 251 bytes (Data Length Extension);
- sin cifrado en esa sesión.

El resumen no incluye el contenido ATT. El registro completo de Samsung no se pudo sacar sin root.

## Implementación en este proyecto (`firmware/dot_panel/ble_bridge.cpp`)

| Qué | Dónde |
| :--- | :--- |
| MTU preferido 247 | `NimBLEDevice::setMTU(247)` tras `NimBLEDevice::init()` |
| Intercambio de MTU tras suscribirse | `scale_link`, paso 5 "Ajuste diferido del enlace" (`exchangeMTU()` junto a `updateConnParams()`) |
| Varias tramas por notificación | Al inicio de `onTimemoreNotifyCallback()`: recorre las tramas, registra las que no son peso y procesa la última de peso |
| Solo `01 01` es peso | `parseTimemorePayload()`, caso 0 |
| Sin secuencia de inicio | `DOT_SEND_INIT_SEQUENCE = false` |
| Sondeo ligero cada 25 s | `TIMEMORE_KEEPALIVE_INTERVAL_MS 25000`, `DOT_LIGHT_KEEPALIVE = true` |
| Tara solo por software | `DOT_HW_TARE = false` en `executeTimemoreTare()` |
| Redibujado al llegar cada muestra | `bridge_set_sample_hook()` (aviso desde `bridge_tx`) → `lvgl_port_wake()` → `on_sample_wake()` en la tarea de LVGL, que procesa la muestra y llama a `lv_refr_now()` |

La línea `[LINK]` del puerto serie muestra `mtu=247` y `tramas juntadas N/M`. Que N crezca de vez en cuando es buena
señal: es la Dot vaciando la cola.

## Cómo medir (herramientas del firmware, puerto serie a 921600)

| Comando | Qué hace |
| :--- | :--- |
| `w` | Activa o desactiva el volcado. Registra cada muestra (hora de llegada, peso y trama cruda), cada respuesta de la Dot (`RX RESPUESTA`), cada comando enviado (`TX`), cada **toque de pantalla** (`TOQUE`) y cada cambio de **píxeles del número del peso** en el framebuffer |
| `q` | Sonda: un comando ligero (`02 08`). **El tiempo hasta su respuesta es el tamaño de la cola**, sin tocar la báscula |
| `l` | Línea `[LINK]`: intervalo, PHY, RSSI, MTU y tramas juntadas |
| `x` | Liberar / reconectar la Dot (desconexión limpia) |
| `R` | Reiniciar el ESP32 (corte brusco: vacía la cola de la Dot) |

Las hipótesis descartadas se probaron con comandos que ya se retiraron del firmware: cambio de intervalo,
resuscripción, desconexión con otros motivos e interruptores de la secuencia de inicio y del sondeo. Si hiciera
falta repetirlas, están en el historial de git del 2026-11-23. Las opciones de trato de la Dot quedan como
constantes al principio de la sección `PANEL` de `ble_bridge.cpp` (`DOT_SEND_INIT_SEQUENCE`, `DOT_LIGHT_KEEPALIVE`,
`DOT_HW_TARE`, `DOT_BIG_MTU`).

**Prueba de la cola** (sin tocar nada): `w`, varias `q` seguidas y otra `q` unos segundos después. Con la corrección,
todas las respuestas llegan en ≤ 0,15 s.

**Prueba de latencia completa** (acción física → pantalla): con `w` activo, tocar la pantalla (fuera de los botones)
en el mismo instante en que se apoya o se levanta el objeto. Se comparan tres marcas:
1. `TOQUE`: el momento físico (con ±0,1–0,2 s de error humano; hacer 8–10 repeticiones);
2. la primera trama con el peso nuevo: retraso de la Dot y del enlace;
3. `PIXELES DEL PESO CAMBIAN`: el número ya está en el framebuffer (50–100 ms después de la trama).

## Lecciones para no volver a morder la cola

- **Ante un retraso que crece con el tiempo, medir la cola con un comando y su respuesta** antes de tocar la
  interfaz: no hace falta mover la báscula, y el resultado es objetivo.
- Un retraso constante que **no suaviza** los escalones es una cola, no un filtro.
- Cualquier comando nuevo que se le mande a la Dot (timer, tara física, consultas) es una trama más en su cola.
  Con MTU grande se absorbe, pero conviene seguir mandando lo mínimo.
- Si en el futuro hiciera falta ver el tráfico ATT de la app oficial, se necesita el registro HCI completo del
  teléfono (en Samsung requiere que la opción de desarrollador quede activa y reiniciar, o root). El resumen de
  `dumpsys` solo trae eventos de control.

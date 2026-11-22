/*
 * Timemore Dot Panel — ui_test_01, version 0.6 (validada en hardware el 2026-11-22)
 *
 * Placa: Waveshare ESP32-S3-Touch-LCD-7B (1024x600, GT911).
 * Objetivo: validar en el hardware real la legibilidad sin gafas y el
 * rendimiento de LVGL 8.4 antes de escribir el firmware definitivo.
 *
 *  - Simula un espresso (18 g -> 36 g) a 10 Hz, como la Dot, en bucle.
 *  - Peso, tiempo y ratio en grande (fuente Bebas Neue, OFL, digitos tabulares).
 *  - Caudal (cian), temperatura (naranja) y objetivo como secundarios.
 *  - Grafica: peso blanco, caudal cian, temperatura naranja, trazo de 4 px.
 *  - Aviso por color: marco de 14 px ambar (acercandose), verde (objetivo),
 *    rojo (pasado).
 *  - Botones: INICIO (reinicia), A/B (cambia de layout), BR- / BR+ (brillo).
 *  - Tocar la grafica muestra u oculta la linea de depuracion.
 *
 *  Layout A: los tres numeros en una fila, digitos de 140 px (~21 mm).
 *  Layout B: peso con digitos de 224 px (~34 mm); tiempo y ratio a ~21 mm.
 *
 * Cambios de la v0.2 (la v0.1 tardaba 75-120 ms por refresco):
 *  - La grafica ya no es un lv_chart (que se redibuja entero en cada punto):
 *    es un lv_canvas en PSRAM en el que se dibuja solo el segmento nuevo y se
 *    invalida solo su recuadro.
 *  - El aviso de color es un marco (4 franjas) en vez del fondo completo, para
 *    no redibujar los 614.400 px de la pantalla en cada cambio de estado.
 *  - Rotulos colocados segun las metricas reales de las fuentes (no se solapan).
 * v0.3: panel RGB creado en el nucleo 0 -> mucho parpadeo. DESCARTADO.
 * v0.4: el canvas crecia con su hija (linea de depuracion) y lv_img repetia la
 *       imagen en mosaico: grafica fantasma detras de los botones. Corregido
 *       con tamano fijo; la linea de depuracion es hija de la pantalla.
 *       Volcado del framebuffer por serie (comandos 'd' y 'D', tools/fbdump.ps1).
 * v0.5: LVGL dibuja en un buffer de SRAM interna (1024x30) y copia al unico
 *       framebuffer en PSRAM (sin el modo antiparpadeo 3 de Waveshare).
 *       Dibujar directamente en la PSRAM era el cuello de botella (~1,5 Mpx/s).
 * v0.6: numeros "por digitos" (una celda por caracter), secundarios a ~3 Hz,
 *       refresco de LVGL cada 15 ms y espera minima de su tarea de 5 ms.
 *       Resultado: ~30 ms por refresco, latencia peso->framebuffer ~30 ms.
 *
 * Drivers (gt911, i2c, io_extension, lvgl_port, rgb_lcd_port, touch):
 * copiados del ejemplo oficial de Waveshare (14_LVGL_BTN). Cambios locales:
 * lvgl_port.h (dibujo en SRAM, sin antiparpadeo, espera minima 5 ms),
 * rgb_lcd_port.h (1 framebuffer) y lvgl_port.cpp (monitor_cb e instrumentacion).
 */

#include "lvgl_port.h"
#include "io_extension.h"
#include "rgb_lcd_port.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"

LV_FONT_DECLARE(font_bebas_320);  // solo 0-9 . : - espacio; linea 230 px
LV_FONT_DECLARE(font_bebas_200);  // solo 0-9 . : - espacio; linea 144 px
LV_FONT_DECLARE(font_bebas_96);   // ASCII + grado; linea 103 px
LV_FONT_DECLARE(font_bebas_56);   // ASCII + grado; linea 60 px

extern volatile uint32_t g_refr_count, g_refr_ms_sum, g_refr_ms_max, g_refr_px_sum,
                         g_wait_us_sum, g_bmp_us_sum;
// Latencia: momento en que cambia el peso -> fin del refresco que lo escribe
extern volatile int64_t g_lat_t0;
extern volatile uint32_t g_lat_us_sum, g_lat_us_max, g_lat_n;

// ---------------------------------------------------------------- colores
#define C_BLACK   0x000000
#define C_WHITE   0xFFFFFF
#define C_CYAN    0x00E5FF
#define C_ORANGE  0xFF8C00
#define C_AMBER   0xFFA000
#define C_GREEN   0x00C853
#define C_RED     0xFF1744
#define C_BTN     0x1E3A8A
#define C_GRID    0x303030
#define C_FRAME   0x606060

// ---------------------------------------------------------------- geometria
static const int SCR_W = 1024, SCR_H = 600;
static const int BORDER = 14;          // ancho del marco de aviso
static const int CV_X = 20, CV_W = 720;
static const int CV_H_A = 288, CV_H_B = 194;
static const int CV_Y_A = 298, CV_Y_B = 392;

// ---------------------------------------------------------------- simulacion
static const float DOSE   = 18.0f;
static const float TARGET = 36.0f;
static const float PRE_S  = 3.0f;      // segundos en espera antes de empezar
static const int   N_PTS  = 250;       // 50 s a 5 Hz

struct Sim {
  uint32_t tick;        // ticks de 100 ms desde el reinicio
  float w;              // peso "real"
  bool stopped;         // la bomba se ha parado (goteo)
  float stop_t, flow_at_stop;
  bool done;
  float done_t;
} sim;

// Historial de la grafica, escalado a 0..1000
static int16_t hist[3][N_PTS];
static int hist_n = 0;
static const uint32_t SER_COLOR[3] = {C_ORANGE, C_CYAN, C_WHITE};  // T, caudal, peso (el peso encima)

// ---------------------------------------------------------------- objetos UI
static lv_obj_t *scr;
static lv_obj_t *cap_w, *cap_t, *cap_r;           // rotulos
// Numeros principales "por digitos": una celda (label) por caracter, para que al
// cambiar un digito solo se redibuje esa celda (v0.6). Los digitos de Bebas Neue
// son tabulares, asi que mientras no cambie el patron (p. ej. "36.8" -> "36.9")
// las celdas no se mueven.
#define SEG_MAX 7
struct Seg {
  lv_obj_t *cell[SEG_MAX];
  char txt[SEG_MAX + 1];
  const lv_font_t *font;
  int x, y, w;                                     // columna en la que se centra
};
static Seg seg_w, seg_t, seg_r;
static lv_obj_t *lbl_flow, *lbl_temp, *lbl_tgt;   // secundarios
static lv_obj_t *canvas, *lbl_dbg;
static lv_obj_t *frame[4];
static lv_color_t *cbuf;                           // buffer del canvas (PSRAM)
static int cv_h = CV_H_A;
static char cache_f[24], cache_T[24];
static int layout = 0;                              // 0 = A, 1 = B
static const uint8_t BR_LEVELS[] = {10, 25, 40, 60, 80, 100};
static int br_idx = 5;
static uint32_t cur_alert = C_BLACK;
static volatile uint32_t g_text_updates = 0;        // cambios del numero de peso

static float noise(float amp) {
  return ((esp_random() % 2001) / 1000.0f - 1.0f) * amp;
}

// Cambia el texto solo si ha cambiado (evita redibujos innecesarios).
static bool set_text(lv_obj_t *l, char *cache, size_t n, const char *txt) {
  if (strncmp(cache, txt, n) == 0) return false;
  strncpy(cache, txt, n - 1);
  cache[n - 1] = 0;
  lv_label_set_text(l, txt);
  return true;
}

// ---------------------------------------------------------------- numeros por digitos
static void seg_create(Seg *s) {
  for (int i = 0; i < SEG_MAX; i++) {
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_color(l, lv_color_hex(C_WHITE), 0);
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    s->cell[i] = l;
  }
  s->txt[0] = 0;
  s->font = &font_bebas_200;
}

static inline bool is_dig(char c) { return c >= '0' && c <= '9'; }

// Recoloca todas las celdas (cambio de longitud, de patron o de layout).
static void seg_layout(Seg *s, const char *txt) {
  int n = strlen(txt);
  if (n > SEG_MAX) n = SEG_MAX;
  int total = 0;
  for (int i = 0; i < n; i++) total += lv_font_get_glyph_width(s->font, (uint8_t)txt[i], 0);
  int x = s->x + (s->w - total) / 2;
  for (int i = 0; i < SEG_MAX; i++) {
    lv_obj_t *c = s->cell[i];
    if (i < n) {
      char b[2] = {txt[i], 0};
      lv_obj_set_style_text_font(c, s->font, 0);
      lv_label_set_text(c, b);
      lv_obj_set_pos(c, x, s->y);
      lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
      x += lv_font_get_glyph_width(s->font, (uint8_t)txt[i], 0);
    } else {
      lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
  }
  memcpy(s->txt, txt, n);
  s->txt[n] = 0;
}

// Cambia el texto; solo redibuja las celdas cuyos caracteres cambian.
static bool seg_set(Seg *s, const char *txt) {
  int n = strlen(txt);
  if (n > SEG_MAX) n = SEG_MAX;
  if ((int)strlen(s->txt) == n && strncmp(s->txt, txt, n) == 0) return false;
  bool same = ((int)strlen(s->txt) == n);
  for (int i = 0; same && i < n; i++)
    if (is_dig(s->txt[i]) != is_dig(txt[i]) || (!is_dig(txt[i]) && s->txt[i] != txt[i])) same = false;
  if (!same) { seg_layout(s, txt); return true; }
  for (int i = 0; i < n; i++) {
    if (s->txt[i] != txt[i]) {
      char b[2] = {txt[i], 0};
      lv_label_set_text(s->cell[i], b);
      s->txt[i] = txt[i];
    }
  }
  return true;
}

static void seg_place(Seg *s, const lv_font_t *f, int x, int y, int w) {
  s->font = f; s->x = x; s->y = y; s->w = w;
  char t[SEG_MAX + 1];
  strcpy(t, s->txt);
  seg_layout(s, t);
}

static void set_alert(uint32_t c) {
  if (c == cur_alert) return;
  cur_alert = c;
  for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(frame[i], lv_color_hex(c), 0);
}

static void set_brightness(uint8_t pct) {
  // El PWM del expansor va invertido: 0 = brillo maximo (el driver limita a 97).
  IO_EXTENSION_Pwm_Output(100 - pct);
}

// ---------------------------------------------------------------- grafica en canvas
static inline int pt_x(int i) { return 2 + (i * (CV_W - 5)) / (N_PTS - 1); }
static inline int pt_y(int v) {
  if (v < 0) v = 0;
  if (v > 1000) v = 1000;
  return 2 + (cv_h - 5) - (v * (cv_h - 5)) / 1000;
}

static inline void put_px(int x, int y, lv_color_t c) {
  if (x < 0 || y < 0 || x >= CV_W || y >= cv_h) return;
  cbuf[y * CV_W + x] = c;
}

// Pincel cuadrado de 4x4 px
static inline void brush(int x, int y, lv_color_t c) {
  for (int dy = -1; dy <= 2; dy++)
    for (int dx = -1; dx <= 2; dx++) put_px(x + dx, y + dy, c);
}

// Bresenham con pincel de 4 px
static void draw_seg(int x0, int y0, int x1, int y1, lv_color_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    brush(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void draw_grid() {
  lv_color_t bg = lv_color_hex(C_BLACK), g = lv_color_hex(C_GRID), fr = lv_color_hex(C_FRAME);
  for (int i = 0; i < CV_W * cv_h; i++) cbuf[i] = bg;
  for (int k = 1; k < 5; k++) {                       // 4 lineas horizontales
    int y = (k * cv_h) / 5;
    for (int x = 0; x < CV_W; x++) cbuf[y * CV_W + x] = g;
  }
  for (int k = 1; k < 10; k++) {                      // 9 verticales (cada 5 s)
    int x = (k * CV_W) / 10;
    for (int y = 0; y < cv_h; y++) cbuf[y * CV_W + x] = g;
  }
  for (int x = 0; x < CV_W; x++) {                    // borde de 2 px
    cbuf[x] = cbuf[CV_W + x] = fr;
    cbuf[(cv_h - 1) * CV_W + x] = cbuf[(cv_h - 2) * CV_W + x] = fr;
  }
  for (int y = 0; y < cv_h; y++) {
    cbuf[y * CV_W] = cbuf[y * CV_W + 1] = fr;
    cbuf[y * CV_W + CV_W - 1] = cbuf[y * CV_W + CV_W - 2] = fr;
  }
}

// Dibuja el tramo que acaba en el punto i y devuelve su recuadro (rel. al canvas)
static void draw_point(int i, lv_area_t *box) {
  int x1 = pt_x(i), x0 = i > 0 ? pt_x(i - 1) : x1;
  int ymin = cv_h, ymax = 0;
  for (int s = 0; s < 3; s++) {
    int y1 = pt_y(hist[s][i]), y0 = i > 0 ? pt_y(hist[s][i - 1]) : y1;
    draw_seg(x0, y0, x1, y1, lv_color_hex(SER_COLOR[s]));
    ymin = min(ymin, min(y0, y1));
    ymax = max(ymax, max(y0, y1));
  }
  box->x1 = x0 - 1; box->x2 = x1 + 2;
  box->y1 = ymin - 1; box->y2 = ymax + 2;
}

static void redraw_chart() {
  lv_area_t b;
  draw_grid();
  for (int i = 0; i < hist_n; i++) draw_point(i, &b);
  lv_obj_invalidate(canvas);
}

static void chart_add(int16_t T, int16_t f, int16_t w) {
  if (hist_n >= N_PTS) return;
  int i = hist_n++;
  hist[0][i] = T; hist[1][i] = f; hist[2][i] = w;
  lv_area_t b;
  draw_point(i, &b);
  // Invalida solo el recuadro del tramo nuevo (coordenadas absolutas)
  lv_area_t abs_b;
  abs_b.x1 = canvas->coords.x1 + b.x1; abs_b.x2 = canvas->coords.x1 + b.x2;
  abs_b.y1 = canvas->coords.y1 + b.y1; abs_b.y2 = canvas->coords.y1 + b.y2;
  lv_obj_invalidate_area(canvas, &abs_b);
}

// ---------------------------------------------------------------- layouts
static void place(lv_obj_t *o, const lv_font_t *f, int x, int y, int w) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_width(o, w);
}

static void apply_layout() {
  // Metricas: bebas_56 linea 60 (mayusculas en y+10..y+49), bebas_96 linea 103
  // (y+17..y+84), bebas_200 linea 144 (y+2..y+142), bebas_320 linea 230.
  if (layout == 0) {
    // A: tres columnas de 360 / 300 / 364 px
    lv_obj_clear_flag(cap_t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cap_r, LV_OBJ_FLAG_HIDDEN);
    place(cap_w, &font_bebas_56, 0,   6, 360);
    place(cap_t, &font_bebas_56, 360, 6, 300);
    place(cap_r, &font_bebas_56, 660, 6, 364);
    seg_place(&seg_w, &font_bebas_200, 0,   66, 360);
    seg_place(&seg_t, &font_bebas_200, 360, 66, 300);
    seg_place(&seg_r, &font_bebas_200, 660, 66, 364);
    place(lbl_flow, &font_bebas_96, 0,   206, 360);
    place(lbl_temp, &font_bebas_96, 360, 206, 300);
    place(lbl_tgt,  &font_bebas_96, 660, 206, 364);
    cv_h = CV_H_A;
    lv_obj_set_pos(canvas, CV_X, CV_Y_A);
  } else {
    // B: peso enorme a la izquierda, tiempo y ratio apilados a la derecha
    lv_obj_add_flag(cap_t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cap_r, LV_OBJ_FLAG_HIDDEN);
    place(cap_w, &font_bebas_56, 14, 6, 600);
    seg_place(&seg_w, &font_bebas_320, 14, 62, 600);
    seg_place(&seg_t, &font_bebas_200, 616, 10,  394);
    seg_place(&seg_r, &font_bebas_200, 616, 150, 394);
    place(lbl_flow, &font_bebas_96, 0,   288, 360);
    place(lbl_temp, &font_bebas_96, 360, 288, 300);
    place(lbl_tgt,  &font_bebas_96, 660, 288, 364);
    cv_h = CV_H_B;
    lv_obj_set_pos(canvas, CV_X, CV_Y_B);
  }
  lv_canvas_set_buffer(canvas, cbuf, CV_W, cv_h, LV_IMG_CF_TRUE_COLOR);
  // Tamano fijo: con LV_SIZE_CONTENT el objeto crecia con sus hijos y lv_img
  // repetia la imagen en mosaico (grafica fantasma detras de los botones, v0.4).
  lv_obj_set_size(canvas, CV_W, cv_h);
  lv_obj_set_pos(lbl_dbg, CV_X + 4, (layout ? CV_Y_B : CV_Y_A) + 4);
  lv_obj_update_layout(scr);
  redraw_chart();
}

// ---------------------------------------------------------------- simulacion
static void sim_reset() {
  memset(&sim, 0, sizeof(sim));
  hist_n = 0;
  redraw_chart();
}

static void sim_timer_cb(lv_timer_t *t) {
  (void)t;
  sim.tick++;
  float tt = sim.tick * 0.1f - PRE_S;  // tiempo de extraccion [s]
  char buf[32];

  if (tt < 0) {
    seg_set(&seg_w, "0.0");
    seg_set(&seg_t, "0:00");
    seg_set(&seg_r, "1:0.0");
    set_text(lbl_flow, cache_f, sizeof(cache_f), "0.0 G/S");
    set_alert(C_BLACK);
    return;
  }

  float f;
  if (!sim.stopped) {
    if (tt < 7)       f = 0;                                // preinfusion
    else if (tt < 12) f = 2.0f * (tt - 7) / 5.0f;           // subida
    else              f = 2.0f - 0.5f * fminf(1.0f, (tt - 12) / 18.0f);
    if (sim.w >= TARGET - 0.6f) {                           // parada por peso
      sim.stopped = true; sim.stop_t = tt; sim.flow_at_stop = f;
    }
  } else {
    f = sim.flow_at_stop * expf(-(tt - sim.stop_t) / 1.0f); // goteo
    if (f < 0.05f && !sim.done) { sim.done = true; sim.done_t = tt; }
  }
  if (sim.done) f = 0;
  sim.w += f * 0.1f;

  float w = sim.w + noise(0.05f);
  float fl = fmaxf(0, f + noise(0.05f));
  float T = 86.0f + 7.0f * expf(-tt / 20.0f) + noise(0.05f);
  float shown_t = sim.done ? sim.done_t : tt;

  snprintf(buf, sizeof(buf), "%.1f", w);
  if (seg_set(&seg_w, buf)) {
    g_text_updates++;
    if (g_lat_t0 == 0) g_lat_t0 = esp_timer_get_time();   // medida de latencia
  }
  int s = (int)shown_t;
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  seg_set(&seg_t, buf);
  snprintf(buf, sizeof(buf), "1:%.1f", fmaxf(0, w) / DOSE);
  seg_set(&seg_r, buf);
  // Secundarios a ~3 Hz: a 10 Hz el ruido los hace ilegibles y cuesta redibujos
  if (sim.tick % 3 == 0) {
    snprintf(buf, sizeof(buf), "%.1f G/S", fl);
    set_text(lbl_flow, cache_f, sizeof(cache_f), buf);
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0" "C", T);
    set_text(lbl_temp, cache_T, sizeof(cache_T), buf);
  }

  // Aviso por color (marco)
  if (w > TARGET + 0.5f)        set_alert(C_RED);
  else if (w >= TARGET - 0.5f)  set_alert(C_GREEN);
  else if (w >= TARGET - 4.0f)  set_alert(C_AMBER);
  else                          set_alert(C_BLACK);

  // Grafica a 5 Hz, escalada a 0..1000
  if ((sim.tick % 2) == 0 && !sim.done) {
    chart_add((int16_t)((T - 80.0f) / 20.0f * 1000),
              (int16_t)(fl / 5.0f * 1000),
              (int16_t)(fmaxf(0, w) / (TARGET * 1.25f) * 1000));
  }

  // Tras 8 s con el resultado en pantalla, se repite (prueba de 10 min).
  if (sim.done && tt - sim.done_t > 8.0f) sim_reset();
}

// ---------------------------------------------------------------- eventos
static void btn_cb(lv_event_t *e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  switch (id) {
    case 0: sim_reset(); break;
    case 1: layout ^= 1; apply_layout(); break;
    case 2: if (br_idx > 0) br_idx--; set_brightness(BR_LEVELS[br_idx]); break;
    case 3: if (br_idx < 5) br_idx++; set_brightness(BR_LEVELS[br_idx]); break;
  }
  Serial.printf("[UI] boton %d  layout %c  brillo %u%%\n", (int)id,
                layout ? 'B' : 'A', BR_LEVELS[br_idx]);
}

static void canvas_cb(lv_event_t *e) {
  (void)e;
  if (lv_obj_has_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- construccion
static lv_obj_t *mk_label(uint32_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(scr);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_label_set_text(l, txt);
  return l;
}

static void mk_button(int x, int y, const char *txt, intptr_t id) {
  lv_obj_t *b = lv_btn_create(scr);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, 120, 100);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(C_WHITE), 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)id);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &font_bebas_56, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(C_WHITE), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
}

static lv_obj_t *mk_rect(int x, int y, int w, int h) {
  lv_obj_t *r = lv_obj_create(scr);
  lv_obj_remove_style_all(r);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_size(r, w, h);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
  return r;
}

static void build_ui() {
  scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  cap_w = mk_label(C_WHITE, "PESO  (G)");
  cap_t = mk_label(C_WHITE, "TIEMPO");
  cap_r = mk_label(C_WHITE, "RATIO");
  seg_create(&seg_w);
  seg_create(&seg_t);
  seg_create(&seg_r);
  lbl_flow = mk_label(C_CYAN, "0.0 G/S");
  lbl_temp = mk_label(C_ORANGE, "--.-\xC2\xB0" "C");
  lbl_tgt  = mk_label(C_WHITE, "OBJ 36.0");

  // Grafica: canvas RGB565 en PSRAM, del tamano maximo (layout A)
  cbuf = (lv_color_t *)heap_caps_malloc(CV_W * CV_H_A * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  canvas = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas, cbuf, CV_W, CV_H_A, LV_IMG_CF_TRUE_COLOR);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, canvas_cb, LV_EVENT_CLICKED, NULL);

  // Linea de depuracion sobre la grafica (tocar la grafica la oculta)
  lbl_dbg = lv_label_create(scr);       // hija de la pantalla, NO del canvas
  lv_label_set_long_mode(lbl_dbg, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lbl_dbg, CV_W - 8);
  lv_obj_set_style_text_font(lbl_dbg, &font_bebas_56, 0);
  lv_obj_set_style_text_color(lbl_dbg, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_bg_color(lbl_dbg, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(lbl_dbg, LV_OPA_COVER, 0);
  lv_label_set_text(lbl_dbg, "...");

  mk_button(750, 380, "INICIO", 0);
  mk_button(880, 380, "A / B", 1);
  mk_button(750, 482, "BR -", 2);
  mk_button(880, 482, "BR +", 3);

  // Marco de aviso: 4 franjas (al cambiar de color solo se redibujan ellas)
  frame[0] = mk_rect(0, 0, SCR_W, BORDER);
  frame[1] = mk_rect(0, SCR_H - BORDER, SCR_W, BORDER);
  frame[2] = mk_rect(0, BORDER, BORDER, SCR_H - 2 * BORDER);
  frame[3] = mk_rect(SCR_W - BORDER, BORDER, BORDER, SCR_H - 2 * BORDER);

  apply_layout();
  sim_reset();
  lv_timer_create(sim_timer_cb, 100, NULL);
}

// ---------------------------------------------------------------- arduino
// El panel RGB usa bounce buffers: una interrupcion copia ~60 MB/s de la PSRAM
// y se instala en el nucleo que crea el panel. Se crea desde una tarea en el
// nucleo 0 para que esa carga no compita con LVGL (nucleo 1).
#define RGB_PANEL_CORE 1   // en el nucleo 0 hubo mucho parpadeo (v0.3): descartado
static esp_lcd_panel_handle_t g_panel = NULL;
static void panel_init_task(void *arg) {
  g_panel = waveshare_esp32_s3_rgb_lcd_init();
  Serial.printf("[UI] panel RGB creado en el nucleo %d\n", xPortGetCoreID());
  xTaskNotifyGive((TaskHandle_t)arg);
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(921600);   // 921600 para que el volcado del framebuffer sea rapido
  delay(200);
  Serial.println("\n[UI] ui_test_01 v0.6 arrancando");

  static esp_lcd_touch_handle_t tp_handle = NULL;
  tp_handle = touch_gt911_init();                 // inicia tambien el expansor
  xTaskCreatePinnedToCore(panel_init_task, "rgb_init", 8192,
                          xTaskGetCurrentTaskHandle(), 5, NULL, RGB_PANEL_CORE);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  esp_lcd_panel_handle_t panel_handle = g_panel;
  wavesahre_rgb_lcd_bl_on();
  set_brightness(BR_LEVELS[br_idx]);
  ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));

  if (lvgl_port_lock(-1)) {
    build_ui();
    // Refresco de LVGL cada 15 ms (por defecto 30): menos espera antes de dibujar
    lv_timer_set_period(_lv_disp_get_refr_timer(lv_disp_get_default()), 15);
    lvgl_port_unlock();
  }
  Serial.println("[UI] interfaz creada");
}

// Volcado de una zona de los framebuffers por el puerto serie (comando 'd' / 'D').
// Sirve para ver lo que hay realmente en memoria frente a lo que muestra el panel.
static void fb_dump(int x, int y, int w, int h) {
  void *fb[2] = {NULL, NULL};
  const int nfb = EXAMPLE_LCD_RGB_BUFFER_NUMS >= 2 ? 2 : 1;
  if (nfb == 2) esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, &fb[0], &fb[1]);
  else          esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb[0]);
  if (!lvgl_port_lock(1000)) return;      // congela LVGL durante el volcado
  for (int k = 0; k < nfb; k++) {
    Serial.printf("\nFBDUMP %d %d %d %d %d\n", k, x, y, w, h);
    for (int r = 0; r < h; r++)
      Serial.write((const uint8_t *)fb[k] + ((size_t)(y + r) * SCR_W + x) * 2, w * 2);
    Serial.flush();
  }
  Serial.print("\nFBEND\n");
  lvgl_port_unlock();
}

void loop() {
  if (Serial.available()) {
    int ch = Serial.read();
    if (ch == 'd') fb_dump(0, 280, SCR_W, 320);   // grafica + botones
    if (ch == 'D') fb_dump(0, 0, SCR_W, SCR_H);    // pantalla completa
  }
  static uint32_t last = 0;
  if (millis() - last < 1000) { delay(20); return; }
  uint32_t dt = millis() - last;
  last = millis();

  // Bateria: media de 10 lecturas del ADC del expansor (divisor 3:1, 3.3 V, 10 bits)
  float raw = 0;
  for (int i = 0; i < 10; i++) { raw += IO_EXTENSION_Adc_Input(); delay(5); }
  raw /= 10.0f;
  float vbat = raw * 3.0f * 3.3f / 1023.0f;

  uint32_t c = g_refr_count, s = g_refr_ms_sum, m = g_refr_ms_max, tu = g_text_updates;
  uint32_t px = g_refr_px_sum, wus = g_wait_us_sum, bus = g_bmp_us_sum;
  g_refr_px_sum = 0; g_wait_us_sum = 0; g_bmp_us_sum = 0;
  uint32_t ln = g_lat_n, lsum = g_lat_us_sum, lmax = g_lat_us_max;
  g_lat_n = 0; g_lat_us_sum = 0; g_lat_us_max = 0;
  g_refr_count = 0; g_refr_ms_sum = 0; g_refr_ms_max = 0; g_text_updates = 0;
  uint32_t avg = c ? s / c : 0;
  float rps = c * 1000.0f / dt;

  uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  if (lvgl_port_lock(100)) {
    uint8_t cpu = 100 - lv_timer_get_idle();
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    char buf[96];
    snprintf(buf, sizeof(buf), "%u MS MX %u  %.0f/S  CPU %u%%  %.2fV  BR %u",
             (unsigned)avg, (unsigned)m, rps, cpu, vbat, BR_LEVELS[br_idx]);
    lv_label_set_text(lbl_dbg, buf);
    lvgl_port_unlock();
    Serial.printf("[DET] por refresco: %u px, copia al framebuffer %u us, dibujo %d us | "
                  "latencia peso->framebuffer media %u ms, max %u ms (n=%u)\n",
                  c ? (unsigned)(px / c) : 0, c ? (unsigned)(bus / c) : 0,
                  c ? (int)(s * 1000 / c) - (int)((wus + bus) / c) : 0,
                  ln ? (unsigned)(lsum / ln / 1000) : 0, (unsigned)(lmax / 1000), (unsigned)ln);
    Serial.printf("[PERF] refresco medio %u ms, max %u ms, %.1f refrescos/s, peso %u cambios/s, "
                  "CPU LVGL %u%% | lv_mem libre %u B (frag %u%%) | heap int %u KB, PSRAM %u KB | "
                  "bat raw %.0f = %.2f V | layout %c\n",
                  (unsigned)avg, (unsigned)m, rps, (unsigned)tu, cpu, (unsigned)mon.free_size,
                  mon.frag_pct, free_int / 1024, free_ps / 1024, raw, vbat,
                  layout ? 'B' : 'A');
  }
}

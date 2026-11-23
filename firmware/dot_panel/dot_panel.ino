/*
 * Timemore Dot Panel — firmware de la placa de 7" (Waveshare ESP32-S3-Touch-LCD-7B)
 *
 * Interfaz con LVGL 8.4 + puente BLE (ble_bridge.cpp, portado del puente validado).
 *
 * Modos (MENU):
 *  - ESPRESSO:  dosis, ratio y objetivo. Timer automatico con la primera gota
 *               y parada cuando cesa el caudal. Aviso con marco de color
 *               anticipado segun el caudal. Puente BooKoo apagado.
 *  - POUR OVER: dosis, ratio (agua total), bloom (agua y tiempo, con cuenta
 *               atras). Puente apagado.
 *  - WENDOUGEE: el panel se anuncia como bascula BooKoo ("BOOKOO_SC_U XXXXXX")
 *               y reenvia la Dot a la maquina; el panel muestra peso, caudal y
 *               ratio. El timer sigue al de la maquina si esta lo maneja.
 * AJUSTES: pantalla (brillo, diseno A/B, depuracion), bascula (liberar,
 * reconectar, olvidar), timer (inicio y parada automaticos), actualizacion.
 *
 * Reglas de rendimiento (SPEC 3.5): dibujo en SRAM interna, numeros por
 * digitos, grafica en lv_canvas incremental de tamano fijo, avisos con marco.
 *
 * Serie (921600): d/D captura del framebuffer (tools/fbdump.ps1), S datos
 * simulados on/off, y los comandos del puente: ? l t r s p.
 */

#include "lvgl_port.h"
#include "io_extension.h"
#include "rgb_lcd_port.h"
#include "ble_bridge.h"
#include "ui_types.h"
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"

#define FW_VERSION "0.1.0"

LV_FONT_DECLARE(font_bebas_320);  // 0-9 . : - espacio; linea 230 px
LV_FONT_DECLARE(font_bebas_200);  // 0-9 . : - espacio; linea 144 px
LV_FONT_DECLARE(font_bebas_96);   // Latin-1; linea ~103 px
LV_FONT_DECLARE(font_bebas_56);   // Latin-1; linea ~60 px

extern volatile uint32_t g_refr_count, g_refr_ms_sum, g_refr_ms_max, g_refr_px_sum,
                         g_wait_us_sum, g_bmp_us_sum;
extern volatile int64_t g_lat_t0;
extern volatile uint32_t g_lat_us_sum, g_lat_us_max, g_lat_n;

// ================================================================ colores y geometria
#define C_BLACK   0x000000
#define C_WHITE   0xFFFFFF
#define C_CYAN    0x00E5FF
#define C_ORANGE  0xFF8C00
#define C_AMBER   0xFFA000
#define C_GREEN   0x00C853
#define C_RED     0xFF1744
#define C_YELLOW  0xFFFF00
#define C_BTN     0x1E3A8A
#define C_BTN_ON  0x00703C
#define C_GRID    0x303030
#define C_FRAME   0x606060

static const int SCR_W = 1024, SCR_H = 600;
static const int BORDER = 14;
static const int CV_X = 20, CV_W = 720;
static const int CV_H_A = 288, CV_H_B = 194;
static const int CV_Y_A = 298, CV_Y_B = 392;
static const int N_PTS = 250;

// ================================================================ configuracion (NVS)
enum Mode : uint8_t { M_ESP = 0, M_POUR = 1, M_WEN = 2 };
static const char *MODE_NAME[] = {"ESPRESSO", "POUR OVER", "WENDOUGEE"};

struct Cfg {
  uint8_t version;
  uint8_t mode;
  float esp_dose, esp_ratio;
  float pour_dose, pour_ratio, pour_bloom_g;
  float pour_bloom_s;
  float wen_dose, wen_ratio;
  uint8_t bright, layout, debug, auto_start, auto_stop;
};
static const uint8_t CFG_VERSION = 1;
static Cfg cfg;
static bool cfg_dirty = false;
static Preferences prefs;

static void cfg_defaults() {
  cfg = Cfg{CFG_VERSION, M_ESP, 18.0f, 2.0f, 15.0f, 16.0f, 30.0f, 30.0f, 18.0f, 2.0f,
            100, 0, 1, 1, 1};
}
static void cfg_load() {
  cfg_defaults();
  prefs.begin("panel", true);
  Cfg tmp;
  size_t n = prefs.getBytes("cfg", &tmp, sizeof(tmp));
  prefs.end();
  if (n == sizeof(tmp) && tmp.version == CFG_VERSION && tmp.mode <= M_WEN) cfg = tmp;
}
// Escritura en flash solo al salir de una pantalla de ajustes y si hubo cambios.
static void cfg_save() {
  if (!cfg_dirty) return;
  cfg_dirty = false;
  prefs.begin("panel", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  Serial.println("[CFG] ajustes guardados");
}

static float mode_dose()  { return cfg.mode == M_ESP ? cfg.esp_dose  : cfg.mode == M_POUR ? cfg.pour_dose  : cfg.wen_dose; }
static float mode_ratio() { return cfg.mode == M_ESP ? cfg.esp_ratio : cfg.mode == M_POUR ? cfg.pour_ratio : cfg.wen_ratio; }
static float mode_target(){ return mode_dose() * mode_ratio(); }

// ================================================================ estado de la preparacion
enum TState : uint8_t { T_ZERO, T_RUN, T_STOP };
static TState t_state = T_ZERO;
static uint32_t t_start_ms = 0, t_acc_ms = 0;
static bool armed = true;                    // listo para el inicio automatico
static uint32_t low_flow_since = 0;          // para la parada automatica
static float cur_w = 0, cur_flow = 0;
static bool have_weight = false;
static uint32_t sample_count = 0;            // muestras desde que arranco el timer
static bool g_sim = false;                   // datos simulados (comando 'S')

static uint32_t timer_ms() { return t_acc_ms + (t_state == T_RUN ? millis() - t_start_ms : 0); }

// ================================================================ objetos UI
static lv_obj_t *scr_main, *scr_menu, *scr_settings, *scr_recipe, *scr_display, *scr_scale,
                *scr_timer, *scr_update;
static lv_obj_t *cap_w, *cap_t, *cap_r;
static lv_obj_t *lbl_flow, *lbl_temp, *lbl_tgt;
static lv_obj_t *lbl_dot, *lbl_app, *lbl_dbg, *lbl_timer_btn;
static lv_obj_t *canvas;
static lv_obj_t *frame[4];
static lv_color_t *cbuf;
static int cv_h = CV_H_A;
static char cache_f[24], cache_T[24], cache_tgt[24], cache_dot[8], cache_app[8];
static uint32_t cur_alert = C_BLACK, cur_dot_col = 0, cur_app_col = 0;

static Seg seg_w, seg_t, seg_r;   // tipo en ui_types.h

// ================================================================ utilidades de texto
static bool set_text(lv_obj_t *l, char *cache, size_t n, const char *txt) {
  if (strncmp(cache, txt, n) == 0) return false;
  strncpy(cache, txt, n - 1);
  cache[n - 1] = 0;
  lv_label_set_text(l, txt);
  return true;
}

// ---- Digitos pre-renderizados -------------------------------------------------
// Al arrancar se dibuja cada caracter de DIG_CHARS una sola vez (blanco sobre
// negro, con su suavizado) en imagenes RGB565 en PSRAM. Las celdas de los
// numeros son lv_img: cambiar un digito es copiar un bloque de pixeles en vez de
// rasterizar un glifo de 200-320 px. Mismo aspecto que la etiqueta: misma
// fuente, misma altura de linea y mismo avance horizontal.
static DigSet dig200, dig320;
static void dig_build(DigSet *d, const lv_font_t *f) {
  d->font = f;
  int h = lv_font_get_line_height(f);
  lv_obj_t *cv = lv_canvas_create(lv_scr_act());
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.font = f;
  ld.color = lv_color_hex(C_WHITE);
  for (int i = 0; i < DIG_N; i++) {
    char ch = DIG_CHARS[i];
    int w = lv_font_get_glyph_width(f, (uint8_t)ch, 0);
    if (w < 1) w = 1;
    size_t sz = (size_t)w * h * sizeof(lv_color_t);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    lv_canvas_set_buffer(cv, buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(cv, lv_color_hex(C_BLACK), LV_OPA_COVER);
    char t[2] = {ch, 0};
    lv_canvas_draw_text(cv, 0, 0, w + 4, &ld, t);
    lv_img_dsc_t *im = &d->img[i];
    memset(im, 0, sizeof(*im));
    im->header.always_zero = 0;
    im->header.cf = LV_IMG_CF_TRUE_COLOR;
    im->header.w = w;
    im->header.h = h;
    im->data_size = sz;
    im->data = buf;
  }
  lv_obj_del(cv);
}
static const lv_img_dsc_t *dig_img(const lv_font_t *f, char ch) {
  DigSet *d = (f == &font_bebas_320) ? &dig320 : &dig200;
  const char *p = strchr(DIG_CHARS, ch);
  return &d->img[p ? (p - DIG_CHARS) : 12];   // desconocido -> '-'
}

static void seg_create(Seg *s, lv_obj_t *parent) {
  for (int i = 0; i < SEG_MAX; i++) {
    lv_obj_t *c = lv_img_create(parent);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    s->cell[i] = c;
  }
  s->txt[0] = 0;
  s->font = &font_bebas_200;
}
static inline bool is_dig(char c) { return c >= '0' && c <= '9'; }
static void seg_layout(Seg *s, const char *txt) {
  int n = strlen(txt);
  if (n > SEG_MAX) n = SEG_MAX;
  int total = 0;
  for (int i = 0; i < n; i++) total += lv_font_get_glyph_width(s->font, (uint8_t)txt[i], 0);
  int x = s->x + (s->w - total) / 2;
  for (int i = 0; i < SEG_MAX; i++) {
    lv_obj_t *c = s->cell[i];
    if (i < n) {
      lv_img_set_src(c, dig_img(s->font, txt[i]));
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
static bool seg_set(Seg *s, const char *txt) {
  int n = strlen(txt);
  if (n > SEG_MAX) n = SEG_MAX;
  if ((int)strlen(s->txt) == n && strncmp(s->txt, txt, n) == 0) return false;
  bool same = ((int)strlen(s->txt) == n);
  for (int i = 0; same && i < n; i++)
    if (is_dig(s->txt[i]) != is_dig(txt[i]) || (!is_dig(txt[i]) && s->txt[i] != txt[i])) same = false;
  if (!same) { seg_layout(s, txt); return true; }
  for (int i = 0; i < n; i++)
    if (s->txt[i] != txt[i]) {
      lv_img_set_src(s->cell[i], dig_img(s->font, txt[i]));
      s->txt[i] = txt[i];
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

static void set_brightness(uint8_t pct) { IO_EXTENSION_Pwm_Output(100 - pct); }

// ================================================================ grafica (canvas)
static int16_t hist[3][N_PTS];   // 0 = temperatura (sin sonda: -1), 1 = caudal, 2 = peso
static int hist_n = 0;
static int decim = 1;            // 1 punto cada 'decim' muestras pares (5 Hz / decim)
static const uint32_t SER_COLOR[3] = {C_ORANGE, C_CYAN, C_WHITE};

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
static inline void brush(int x, int y, lv_color_t c) {
  for (int dy = -1; dy <= 2; dy++)
    for (int dx = -1; dx <= 2; dx++) put_px(x + dx, y + dy, c);
}
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
  for (int k = 1; k < 5; k++) { int y = (k * cv_h) / 5; for (int x = 0; x < CV_W; x++) cbuf[y * CV_W + x] = g; }
  for (int k = 1; k < 10; k++) { int x = (k * CV_W) / 10; for (int y = 0; y < cv_h; y++) cbuf[y * CV_W + x] = g; }
  for (int x = 0; x < CV_W; x++) {
    cbuf[x] = cbuf[CV_W + x] = fr;
    cbuf[(cv_h - 1) * CV_W + x] = cbuf[(cv_h - 2) * CV_W + x] = fr;
  }
  for (int y = 0; y < cv_h; y++) {
    cbuf[y * CV_W] = cbuf[y * CV_W + 1] = fr;
    cbuf[y * CV_W + CV_W - 1] = cbuf[y * CV_W + CV_W - 2] = fr;
  }
}
static void draw_point(int i, lv_area_t *box) {
  int x1 = pt_x(i), x0 = i > 0 ? pt_x(i - 1) : x1;
  int ymin = cv_h, ymax = 0;
  for (int s = 0; s < 3; s++) {
    if (hist[s][i] < 0 || (i > 0 && hist[s][i - 1] < 0)) continue;   // serie sin datos
    int y1 = pt_y(hist[s][i]), y0 = i > 0 ? pt_y(hist[s][i - 1]) : y1;
    draw_seg(x0, y0, x1, y1, lv_color_hex(SER_COLOR[s]));
    ymin = min(ymin, min(y0, y1));
    ymax = max(ymax, max(y0, y1));
  }
  if (ymin > ymax) { ymin = 0; ymax = 0; }
  box->x1 = x0 - 1; box->x2 = x1 + 2;
  box->y1 = ymin - 1; box->y2 = ymax + 2;
}
static void redraw_chart() {
  lv_area_t b;
  draw_grid();
  for (int i = 0; i < hist_n; i++) draw_point(i, &b);
  lv_obj_invalidate(canvas);
}
static void chart_clear() { hist_n = 0; decim = 1; redraw_chart(); }
static void chart_add(int16_t T, int16_t f, int16_t w) {
  if (hist_n >= N_PTS) {
    // Lleno: se comprime a la mitad (cada punto representa el doble de tiempo)
    for (int i = 0; i < N_PTS / 2; i++)
      for (int s = 0; s < 3; s++) hist[s][i] = hist[s][2 * i];
    hist_n = N_PTS / 2;
    decim *= 2;
    redraw_chart();
  }
  int i = hist_n++;
  hist[0][i] = T; hist[1][i] = f; hist[2][i] = w;
  lv_area_t b;
  draw_point(i, &b);
  lv_area_t a;
  a.x1 = canvas->coords.x1 + b.x1; a.x2 = canvas->coords.x1 + b.x2;
  a.y1 = canvas->coords.y1 + b.y1; a.y2 = canvas->coords.y1 + b.y2;
  lv_obj_invalidate_area(canvas, &a);
}

// ================================================================ timer
static void timer_btn_text() {
  lv_label_set_text(lbl_timer_btn, t_state == T_ZERO ? "INICIO" : t_state == T_RUN ? "PARAR" : "CERO");
}
static void timer_start() {
  if (t_state == T_RUN) return;
  if (t_state == T_ZERO) { chart_clear(); sample_count = 0; }
  t_start_ms = millis();
  t_state = T_RUN;
  low_flow_since = 0;
  armed = false;
  timer_btn_text();
  Serial.println("[UI] timer: inicio");
}
static void timer_stop() {
  if (t_state != T_RUN) return;
  t_acc_ms += millis() - t_start_ms;
  t_state = T_STOP;
  timer_btn_text();
  Serial.printf("[UI] timer: parada a %.1f s, %.1f g\n", t_acc_ms / 1000.0f, cur_w);
}
static void timer_reset() {
  t_state = T_ZERO;
  t_acc_ms = 0;
  armed = true;
  chart_clear();
  sample_count = 0;
  timer_btn_text();
}

// ================================================================ textos de la pantalla principal
static void update_captions() {
  char b[32];
  snprintf(b, sizeof(b), "%s \xC2\xB7 G", MODE_NAME[cfg.mode]);
  lv_label_set_text(cap_w, b);
  lv_label_set_text(cap_r, "RATIO 1:");
}

static void update_target_label() {
  char b[32];
  uint32_t ms = timer_ms();
  if (cfg.mode == M_POUR && t_state != T_ZERO && ms < cfg.pour_bloom_s * 1000) {
    int left = (int)ceilf(cfg.pour_bloom_s - ms / 1000.0f);
    snprintf(b, sizeof(b), "BLOOM %d", left);
  } else if (cfg.mode == M_POUR && t_state == T_ZERO) {
    snprintf(b, sizeof(b), "BLOOM %.0fG", cfg.pour_bloom_g);   // "BLOOM 150 G" no cabe
  } else {
    snprintf(b, sizeof(b), "OBJ %.*f", mode_target() >= 100 ? 0 : 1, mode_target());
  }
  set_text(lbl_tgt, cache_tgt, sizeof(cache_tgt), b);
}

// Estado de la Dot y de la Wendougee (texto con color)
static void update_status() {
  uint32_t dc = bridge_scale_connected() ? C_GREEN : bridge_scale_released() ? C_RED : C_AMBER;
  if (g_sim) dc = C_CYAN;
  if (dc != cur_dot_col) { cur_dot_col = dc; lv_obj_set_style_text_color(lbl_dot, lv_color_hex(dc), 0); }
  set_text(lbl_dot, cache_dot, sizeof(cache_dot), g_sim ? "SIM" : "DOT");
  if (cfg.mode == M_WEN) {
    lv_obj_clear_flag(lbl_app, LV_OBJ_FLAG_HIDDEN);
    uint32_t ac = bridge_app_connected() ? C_GREEN : C_AMBER;
    if (ac != cur_app_col) { cur_app_col = ac; lv_obj_set_style_text_color(lbl_app, lv_color_hex(ac), 0); }
    set_text(lbl_app, cache_app, sizeof(cache_app), "WEN");
  } else {
    lv_obj_add_flag(lbl_app, LV_OBJ_FLAG_HIDDEN);
  }
}

// ================================================================ datos: una muestra nueva
static void on_sample(float w, float flow) {
  cur_w = w; cur_flow = flow; have_weight = true;
  char buf[32];

  // --- Timer automatico ---
  if (t_state == T_ZERO && fabsf(w) < 0.2f) armed = true;
  bool app_drives = (cfg.mode == M_WEN && bridge_app_timer_running());
  if (cfg.auto_start && armed && t_state == T_ZERO && !app_drives) {
    float thr = cfg.mode == M_POUR ? 2.0f : 0.3f;     // primera gota / primer vertido
    if (w > thr) timer_start();
  }
  if (cfg.auto_stop && t_state == T_RUN && cfg.mode != M_WEN) {
    uint32_t ms = timer_ms();
    bool flowing_done = flow < 0.2f && w > 5.0f && ms > 8000;
    if (cfg.mode == M_POUR) flowing_done = flowing_done && w > mode_target() * 0.9f;
    if (flowing_done) {
      if (!low_flow_since) low_flow_since = millis();
      if (millis() - low_flow_since > (cfg.mode == M_POUR ? 5000u : 2000u)) timer_stop();
    } else {
      low_flow_since = 0;
    }
  }

  // --- Numeros ---
  if (w <= -1000)      snprintf(buf, sizeof(buf), "-999");   // "-2000" no cabe en la columna
  else if (fabsf(w) >= 100) snprintf(buf, sizeof(buf), "%.0f", w);
  else                 snprintf(buf, sizeof(buf), "%.1f", w);
  seg_set(&seg_w, buf);
  float dose = mode_dose();
  if (dose > 0.1f) snprintf(buf, sizeof(buf), "%.1f", fmaxf(0, w) / dose);
  else             snprintf(buf, sizeof(buf), "-.-");
  seg_set(&seg_r, buf);
  static uint8_t slow = 0;
  if (++slow >= 3) {                       // secundarios a ~3 Hz
    slow = 0;
    snprintf(buf, sizeof(buf), "%.1f G/S", flow);
    set_text(lbl_flow, cache_f, sizeof(cache_f), buf);
  }

  // --- Aviso con el marco ---
  uint32_t ms = timer_ms();
  float target = mode_target();
  uint32_t col = C_BLACK;
  if (cfg.mode == M_POUR && t_state != T_ZERO && ms < cfg.pour_bloom_s * 1000) {
    if (w >= cfg.pour_bloom_g) col = C_GREEN;
    else if (w >= cfg.pour_bloom_g * 0.8f) col = C_AMBER;
  } else if (target > 1 && (t_state != T_ZERO || w > 1)) {
    float pred = w + fmaxf(0, flow) * 0.5f;           // compensacion: 0.5 s de caudal
    float near = cfg.mode == M_POUR ? 20.0f : 4.0f;
    float tol  = cfg.mode == M_POUR ? 3.0f : 0.5f;
    if (w > target + tol)            col = C_RED;
    else if (pred >= target - tol)   col = C_GREEN;
    else if (pred >= target - near)  col = C_AMBER;
  }
  set_alert(col);

  // --- Grafica: 5 Hz mientras corre el timer (comprimida si se llena) ---
  if (t_state == T_RUN) {
    sample_count++;
    if (sample_count % (2 * decim) == 0) {
      float wmax = fmaxf(10.0f, target * 1.25f);
      float fmax = cfg.mode == M_POUR ? 12.0f : 5.0f;
      chart_add(-1, (int16_t)(fminf(fmaxf(flow, 0) / fmax, 1) * 1000),
                (int16_t)(fminf(fmaxf(w, 0) / wmax, 1) * 1000));
    }
  }
}

// Simulacion (comando 'S'): espresso o vertidos segun el modo, a 10 Hz
static void sim_cb(lv_timer_t *) {
  if (!g_sim) return;
  static float w = 0, t = -3;
  static int phase = 0;
  t += 0.1f;
  float f = 0;
  if (cfg.mode == M_POUR) {
    if (t < 0) f = 0; else if (t < 8) f = 5; else if (t < 35) f = 0;
    else if (w < mode_target()) f = 5; else f = 0;
  } else {
    if (t < 7) f = 0; else if (t < 12) f = 2.0f * (t - 7) / 5; else if (w < mode_target() - 0.6f) f = 1.8f;
    else f = 0;
  }
  w += f * 0.1f;
  if (t > 70) { w = 0; t = -3; phase++; timer_reset(); }
  float wn = w + ((esp_random() % 101) / 1000.0f - 0.05f);
  on_sample(wn, f);
}

// ================================================================ temporizador de datos (10 ms)
// Procesa la muestra nueva de la Dot, si la hay. Devuelve true si proceso una.
static bool sample_poll() {
  static uint32_t last_seq = 0;
  if (g_sim) return false;
  BridgeSample s;
  bridge_get_sample(&s);
  if (s.seq == last_seq) return false;
  last_seq = s.seq;
  if (!bridge_scale_connected()) return false;
  // latencia: llegada de la muestra -> fin del refresco que la dibuja
  uint32_t now32 = (uint32_t)esp_timer_get_time();
  g_lat_t0 = esp_timer_get_time() - (int64_t)(uint32_t)(now32 - s.arrival_us);
  on_sample(s.weight, s.flow);
  return true;
}

// Muestra por evento: el puente avisa de cada muestra (tarea bridge_tx) y la
// tarea de LVGL se despierta y la procesa al instante (antes la sondeaba cada
// 10 ms). El refresco sigue con el ritmo normal de LVGL: forzarlo aqui con
// lv_refr_now() solo ganaba ~5 ms y producia rayas en el panel (el bounce buffer
// se quedaba sin datos al coincidir las escrituras en PSRAM con la lectura).
static void on_sample_wake() {
  sample_poll();
}
static void sample_hook() { lvgl_port_wake(); }

static void data_cb(lv_timer_t *) {
  if (!g_sim) {
    sample_poll();   // respaldo por si se perdiera un aviso
    if (!bridge_scale_connected() && have_weight) {
      have_weight = false;
      seg_set(&seg_w, "--.-");
      set_alert(C_BLACK);
    }
  }

  // Wendougee: el timer sigue al de la maquina
  static bool prev_app_timer = false;
  if (cfg.mode == M_WEN) {
    bool at = bridge_app_timer_running();
    if (at && !prev_app_timer) { if (t_state != T_ZERO) timer_reset(); timer_start(); }
    if (!at && prev_app_timer) timer_stop();
    prev_app_timer = at;
  }

  // Tiempo y textos lentos
  char buf[16];
  uint32_t s = timer_ms() / 1000;
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  seg_set(&seg_t, buf);
  static uint32_t slow_ms = 0;
  if (millis() - slow_ms > 250) {
    slow_ms = millis();
    update_target_label();
    update_status();
  }
}

// ================================================================ layout de la pantalla principal
static void place(lv_obj_t *o, const lv_font_t *f, int x, int y, int w) {
  lv_obj_set_style_text_font(o, f, 0);
  lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_width(o, w);
}
static void apply_layout() {
  if (cfg.layout == 0) {
    lv_obj_clear_flag(cap_t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cap_r, LV_OBJ_FLAG_HIDDEN);
    // Columnas dentro del marco de alerta (14..1010). Peor caso a 200 px:
    // peso "-99.9" 330 px, tiempo "10:00" 357 px, ratio "99.9" 277 px.
    place(cap_w, &font_bebas_56, 14, 6, 338);
    place(cap_t, &font_bebas_56, 352, 6, 360);
    place(cap_r, &font_bebas_56, 712, 6, 298);
    seg_place(&seg_w, &font_bebas_200, 14, 66, 338);
    seg_place(&seg_t, &font_bebas_200, 352, 66, 360);
    seg_place(&seg_r, &font_bebas_200, 712, 66, 298);
    place(lbl_flow, &font_bebas_96, 14, 206, 338);
    place(lbl_temp, &font_bebas_96, 352, 206, 280);
    place(lbl_tgt, &font_bebas_96, 632, 206, 378);
    cv_h = CV_H_A;
    lv_obj_set_pos(canvas, CV_X, CV_Y_A);
    lv_obj_set_pos(lbl_dot, 770, 306);
    lv_obj_set_pos(lbl_app, 890, 306);
  } else {
    lv_obj_add_flag(cap_t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cap_r, LV_OBJ_FLAG_HIDDEN);
    place(cap_w, &font_bebas_56, 14, 6, 600);
    seg_place(&seg_w, &font_bebas_320, 14, 62, 600);
    seg_place(&seg_t, &font_bebas_200, 616, 16, 394);    // tinta 16..159
    seg_place(&seg_r, &font_bebas_200, 616, 162, 394);   // tinta 162..305
    place(lbl_flow, &font_bebas_96, 14, 296, 338);
    place(lbl_temp, &font_bebas_96, 352, 296, 280);
    place(lbl_tgt, &font_bebas_96, 632, 296, 378);
    cv_h = CV_H_B;
    lv_obj_set_pos(canvas, CV_X, CV_Y_B);
    lv_obj_set_pos(lbl_dot, CV_X + CV_W - 230, CV_Y_B + 8);
    lv_obj_set_pos(lbl_app, CV_X + CV_W - 110, CV_Y_B + 8);
  }
  lv_canvas_set_buffer(canvas, cbuf, CV_W, cv_h, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_size(canvas, CV_W, cv_h);   // tamano fijo (evita el mosaico de lv_img)
  lv_obj_set_pos(lbl_dbg, CV_X + 4, (cfg.layout ? CV_Y_B : CV_Y_A) + 4);
  if (cfg.debug) lv_obj_clear_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_update_layout(scr_main);
  redraw_chart();
}

// ================================================================ helpers de construccion
static lv_obj_t *mk_screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}
static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, txt);
  return l;
}
static lv_obj_t *mk_btn(lv_obj_t *p, int x, int y, int w, int h, const lv_font_t *f,
                        const char *txt, lv_event_cb_t cb, intptr_t id, lv_obj_t **lbl_out = nullptr) {
  lv_obj_t *b = lv_btn_create(p);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(C_WHITE), 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)id);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(C_WHITE), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  if (lbl_out) *lbl_out = l;
  return b;
}
static void btn_on(lv_obj_t *b, bool on) {
  lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_BTN_ON : C_BTN), 0);
}
// Titulo de pantalla + boton VOLVER (arriba a la derecha)
static void mk_header(lv_obj_t *s, const char *title, lv_event_cb_t back_cb) {
  lv_obj_t *t = mk_label(s, &font_bebas_96, C_WHITE, title);
  lv_obj_set_pos(t, 30, 8);
  mk_btn(s, 764, 10, 240, 100, &font_bebas_56, "< VOLVER", back_cb, 0);
}
static void go(lv_obj_t *s) { lv_scr_load(s); }

// ================================================================ pantalla principal
static void main_btn_cb(lv_event_t *e) {
  switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0:                                   // TARA
      if (!g_sim) bridge_tare();
      if (t_state == T_ZERO) armed = true;
      Serial.println("[UI] tara");
      break;
    case 1:                                   // INICIO / PARAR / CERO
      if (t_state == T_ZERO) timer_start();
      else if (t_state == T_RUN) timer_stop();
      else timer_reset();
      break;
    case 2: {                                 // RECETA
      extern void build_recipe();
      build_recipe();
      go(scr_recipe);
      break;
    }
    case 3: go(scr_menu); break;              // MENU
  }
}
static void canvas_cb(lv_event_t *) {
  cfg.debug = !cfg.debug; cfg_dirty = true; cfg_save();
  if (cfg.debug) lv_obj_clear_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_dbg, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *r = lv_obj_create(p);
  lv_obj_remove_style_all(r);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_size(r, w, h);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
  return r;
}

static void build_main() {
  uint32_t t0 = millis();
  dig_build(&dig200, &font_bebas_200);
  dig_build(&dig320, &font_bebas_320);
  Serial.printf("[UI] digitos pre-renderizados en %lu ms (PSRAM libre %u KB)\n", millis() - t0,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  scr_main = mk_screen();
  cap_w = mk_label(scr_main, &font_bebas_56, C_WHITE, "");
  cap_t = mk_label(scr_main, &font_bebas_56, C_WHITE, "TIEMPO");
  cap_r = mk_label(scr_main, &font_bebas_56, C_WHITE, "RATIO 1:");
  seg_create(&seg_w, scr_main);
  seg_create(&seg_t, scr_main);
  seg_create(&seg_r, scr_main);
  lbl_flow = mk_label(scr_main, &font_bebas_96, C_CYAN, "0.0 G/S");
  lbl_temp = mk_label(scr_main, &font_bebas_96, C_ORANGE, "--.-\xC2\xB0" "C");
  lbl_tgt  = mk_label(scr_main, &font_bebas_96, C_WHITE, "");
  for (lv_obj_t *l : {cap_w, cap_t, cap_r, lbl_flow, lbl_temp, lbl_tgt})
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);

  cbuf = (lv_color_t *)heap_caps_malloc(CV_W * CV_H_A * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  canvas = lv_canvas_create(scr_main);
  lv_canvas_set_buffer(canvas, cbuf, CV_W, CV_H_A, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_size(canvas, CV_W, CV_H_A);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas, canvas_cb, LV_EVENT_CLICKED, NULL);

  lbl_dot = mk_label(scr_main, &font_bebas_56, C_AMBER, "DOT");
  lbl_app = mk_label(scr_main, &font_bebas_56, C_AMBER, "WEN");
  lbl_dbg = mk_label(scr_main, &font_bebas_56, C_YELLOW, "");
  lv_label_set_long_mode(lbl_dbg, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lbl_dbg, 486);   // deja sitio al estado DOT/WEN en el diseno B
  lv_obj_set_style_bg_color(lbl_dbg, lv_color_hex(C_BLACK), 0);
  lv_obj_set_style_bg_opa(lbl_dbg, LV_OPA_50, 0);

  // Botones dentro del marco (y <= 585) y por debajo de los secundarios del diseno B (tinta hasta 384)
  mk_btn(scr_main, 746, 388, 130, 96, &font_bebas_56, "TARA", main_btn_cb, 0);
  mk_btn(scr_main, 880, 388, 130, 96, &font_bebas_56, "INICIO", main_btn_cb, 1, &lbl_timer_btn);
  mk_btn(scr_main, 746, 490, 130, 96, &font_bebas_56, "RECETA", main_btn_cb, 2);
  mk_btn(scr_main, 880, 490, 130, 96, &font_bebas_56, "MEN\xC3\x9A", main_btn_cb, 3);

  frame[0] = mk_rect(scr_main, 0, 0, SCR_W, BORDER);
  frame[1] = mk_rect(scr_main, 0, SCR_H - BORDER, SCR_W, BORDER);
  frame[2] = mk_rect(scr_main, 0, BORDER, BORDER, SCR_H - 2 * BORDER);
  frame[3] = mk_rect(scr_main, SCR_W - BORDER, BORDER, BORDER, SCR_H - 2 * BORDER);

  update_captions();
  apply_layout();
  seg_set(&seg_w, "--.-");
  seg_set(&seg_t, "0:00");
  seg_set(&seg_r, "-.-");
}

// ================================================================ menu de modos
static lv_obj_t *menu_btn[4];
static void set_mode(uint8_t m) {
  if (cfg.mode != m) { cfg.mode = m; cfg_dirty = true; cfg_save(); }
  bridge_set_enabled(cfg.mode == M_WEN);
  update_captions();
  timer_reset();
  for (int i = 0; i < 3; i++) btn_on(menu_btn[i], i == cfg.mode);
  Serial.printf("[UI] modo %s (puente %s)\n", MODE_NAME[cfg.mode], cfg.mode == M_WEN ? "activo" : "apagado");
}
static void menu_cb(lv_event_t *e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  if (id <= 2) { set_mode((uint8_t)id); go(scr_main); }
  else go(scr_settings);
}
static void back_main_cb(lv_event_t *) { cfg_save(); go(scr_main); }

static void build_menu() {
  scr_menu = mk_screen();
  mk_header(scr_menu, "MEN\xC3\x9A", back_main_cb);
  menu_btn[0] = mk_btn(scr_menu, 30, 140, 470, 200, &font_bebas_96, "ESPRESSO", menu_cb, 0);
  menu_btn[1] = mk_btn(scr_menu, 524, 140, 470, 200, &font_bebas_96, "POUR OVER", menu_cb, 1);
  menu_btn[2] = mk_btn(scr_menu, 30, 370, 470, 200, &font_bebas_96, "WENDOUGEE", menu_cb, 2);
  menu_btn[3] = mk_btn(scr_menu, 524, 370, 470, 200, &font_bebas_96, "AJUSTES", menu_cb, 3);
  for (int i = 0; i < 3; i++) btn_on(menu_btn[i], i == cfg.mode);
}

// ================================================================ receta (por modo)
struct Field { const char *name; float *val; float small, big, lo, hi; int dec; };
static Field fields[4];
static int n_fields = 0;
static lv_obj_t *field_lbl[4], *recipe_sum;

static void recipe_refresh() {
  char b[48];
  for (int i = 0; i < n_fields; i++) {
    snprintf(b, sizeof(b), "%.*f", fields[i].dec, *fields[i].val);
    lv_label_set_text(field_lbl[i], b);
  }
  if (cfg.mode == M_POUR)
    snprintf(b, sizeof(b), "AGUA TOTAL %.0f G  \xC2\xB7  BLOOM %.0f G / %.0f S",
             mode_target(), cfg.pour_bloom_g, cfg.pour_bloom_s);
  else
    snprintf(b, sizeof(b), "OBJETIVO %.1f G", mode_target());
  lv_label_set_text(recipe_sum, b);
}
static void field_cb(lv_event_t *e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  int f = id / 4, k = id % 4;
  Field &fd = fields[f];
  float step = (k == 0) ? -fd.big : (k == 1) ? -fd.small : (k == 2) ? fd.small : fd.big;
  float v = roundf((*fd.val + step) * 100) / 100;
  *fd.val = fminf(fd.hi, fmaxf(fd.lo, v));
  cfg_dirty = true;
  recipe_refresh();
}
void build_recipe() {
  lv_obj_clean(scr_recipe);
  char title[40];
  snprintf(title, sizeof(title), "RECETA \xC2\xB7 %s", MODE_NAME[cfg.mode]);
  mk_header(scr_recipe, title, back_main_cb);
  n_fields = 0;
  if (cfg.mode == M_ESP) {
    fields[n_fields++] = {"DOSIS G", &cfg.esp_dose, 0.1f, 1, 5, 30, 1};
    fields[n_fields++] = {"RATIO 1:", &cfg.esp_ratio, 0.1f, 0.5f, 1, 4, 1};
  } else if (cfg.mode == M_POUR) {
    fields[n_fields++] = {"DOSIS G", &cfg.pour_dose, 0.5f, 5, 5, 60, 1};
    fields[n_fields++] = {"RATIO 1:", &cfg.pour_ratio, 0.1f, 1, 10, 20, 1};
    fields[n_fields++] = {"BLOOM G", &cfg.pour_bloom_g, 1, 10, 0, 150, 0};
    fields[n_fields++] = {"BLOOM S", &cfg.pour_bloom_s, 1, 5, 0, 120, 0};
  } else {
    fields[n_fields++] = {"DOSIS G", &cfg.wen_dose, 0.1f, 1, 5, 30, 1};
    fields[n_fields++] = {"RATIO 1:", &cfg.wen_ratio, 0.1f, 0.5f, 1, 4, 1};
  }
  static const char *step_txt[4] = {"--", "-", "+", "++"};
  // Con 4 campos (pour over) las filas se compactan para que el resumen quepa
  // por encima del borde inferior: 118 + 4*100 + resumen (60) = 578 < 600.
  const int y0 = 118, pitch = (n_fields > 2) ? 100 : 108, bh = pitch - 8;
  for (int i = 0; i < n_fields; i++) {
    int y = y0 + i * pitch;
    lv_obj_t *n = mk_label(scr_recipe, &font_bebas_56, C_WHITE, fields[i].name);
    lv_obj_set_pos(n, 30, y + (bh - 60) / 2);
    field_lbl[i] = mk_label(scr_recipe, &font_bebas_96, C_YELLOW, "");
    lv_obj_set_width(field_lbl[i], 220);
    lv_obj_set_style_text_align(field_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(field_lbl[i], 250, y + (bh - 100) / 2);
    for (int k = 0; k < 4; k++)
      mk_btn(scr_recipe, 490 + k * 128, y, 118, bh, &font_bebas_96, step_txt[k], field_cb, i * 4 + k);
  }
  recipe_sum = mk_label(scr_recipe, &font_bebas_56, C_CYAN, "");
  lv_obj_set_pos(recipe_sum, 30, y0 + n_fields * pitch + 2);
  recipe_refresh();
}

// ================================================================ ajustes
static void back_settings_cb(lv_event_t *) { cfg_save(); go(scr_settings); }
static void settings_cb(lv_event_t *e) {
  switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: go(scr_display); break;
    case 1: go(scr_scale); break;
    case 2: go(scr_timer); break;
    case 3: go(scr_update); break;
  }
}
static void build_settings() {
  scr_settings = mk_screen();
  mk_header(scr_settings, "AJUSTES", back_main_cb);
  mk_btn(scr_settings, 30, 140, 470, 200, &font_bebas_96, "PANTALLA", settings_cb, 0);
  mk_btn(scr_settings, 524, 140, 470, 200, &font_bebas_96, "B\xC3\x81SCULA", settings_cb, 1);
  mk_btn(scr_settings, 30, 370, 470, 200, &font_bebas_96, "TIMER", settings_cb, 2);
  mk_btn(scr_settings, 524, 370, 470, 200, &font_bebas_96, "ACTUALIZAR", settings_cb, 3);
}

// --- Pantalla: brillo, diseno, depuracion
static lv_obj_t *lbl_bright, *btn_layA, *btn_layB, *btn_dbg, *lbl_dbg_btn;
static void display_refresh() {
  char b[16];
  snprintf(b, sizeof(b), "%u%%", cfg.bright);
  lv_label_set_text(lbl_bright, b);
  btn_on(btn_layA, cfg.layout == 0);
  btn_on(btn_layB, cfg.layout == 1);
  btn_on(btn_dbg, cfg.debug);
  lv_label_set_text(lbl_dbg_btn, cfg.debug ? "S\xC3\x8D" : "NO");
}
static void display_cb(lv_event_t *e) {
  switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: if (cfg.bright > 10) cfg.bright -= 10; set_brightness(cfg.bright); break;
    case 1: if (cfg.bright < 100) cfg.bright += 10; set_brightness(cfg.bright); break;
    case 2: cfg.layout = 0; apply_layout(); break;
    case 3: cfg.layout = 1; apply_layout(); break;
    case 4: cfg.debug = !cfg.debug; apply_layout(); break;
  }
  cfg_dirty = true;
  display_refresh();
}
static void build_display() {
  scr_display = mk_screen();
  mk_header(scr_display, "PANTALLA", back_settings_cb);
  lv_obj_t *l;
  l = mk_label(scr_display, &font_bebas_56, C_WHITE, "BRILLO");      lv_obj_set_pos(l, 30, 160);
  mk_btn(scr_display, 300, 140, 160, 100, &font_bebas_96, "-", display_cb, 0);
  lbl_bright = mk_label(scr_display, &font_bebas_96, C_YELLOW, "");
  lv_obj_set_width(lbl_bright, 220);
  lv_obj_set_style_text_align(lbl_bright, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lbl_bright, 470, 140);
  mk_btn(scr_display, 700, 140, 160, 100, &font_bebas_96, "+", display_cb, 1);
  l = mk_label(scr_display, &font_bebas_56, C_WHITE, "DISE\xC3\x91O");  lv_obj_set_pos(l, 30, 300);
  btn_layA = mk_btn(scr_display, 300, 280, 330, 100, &font_bebas_56, "A: TRES EN FILA", display_cb, 2);
  btn_layB = mk_btn(scr_display, 650, 280, 330, 100, &font_bebas_56, "B: PESO GRANDE", display_cb, 3);
  l = mk_label(scr_display, &font_bebas_56, C_WHITE, "DEPURACI\xC3\x93N"); lv_obj_set_pos(l, 30, 440);
  btn_dbg = mk_btn(scr_display, 300, 420, 260, 100, &font_bebas_56, "", display_cb, 4, &lbl_dbg_btn);
  display_refresh();
}

// --- Bascula: estado, liberar/reconectar, olvidar
static lv_obj_t *lbl_scale_info, *lbl_release_btn, *lbl_forget_btn;
static uint32_t forget_armed_ms = 0;
static void scale_refresh() {
  char b[200];
  const char *st = bridge_scale_connected() ? "CONECTADA" : bridge_scale_released() ? "LIBERADA" :
                   bridge_scale_scanning() ? "BUSCANDO..." : "DESCONECTADA";
  const char *mac = bridge_scale_mac();
  snprintf(b, sizeof(b), "DOT: %s\nMAC: %s\nPUENTE: %s  \xC2\xB7  %s\nWENDOUGEE: %s",
           st, mac[0] ? mac : "(ninguna guardada)",
           bridge_enabled() ? "ACTIVO" : "APAGADO", bridge_name(),
           bridge_app_connected() ? "CONECTADA" : "ESPERANDO");
  lv_label_set_text(lbl_scale_info, b);
  lv_label_set_text(lbl_release_btn, bridge_scale_released() ? "RECONECTAR" : "LIBERAR");
  if (forget_armed_ms && millis() - forget_armed_ms > 3000) {
    forget_armed_ms = 0;
    lv_label_set_text(lbl_forget_btn, "OLVIDAR B\xC3\x81SCULA");
  }
}
static void scale_cb(lv_event_t *e) {
  intptr_t id = (intptr_t)lv_event_get_user_data(e);
  if (id == 0) {
    if (bridge_scale_released()) bridge_reconnect_scale(); else bridge_release_scale();
  } else if (id == 1) {
    if (!forget_armed_ms) { forget_armed_ms = millis(); lv_label_set_text(lbl_forget_btn, "\xC2\xBFSEGURO? TOCA OTRA VEZ"); }
    else { forget_armed_ms = 0; bridge_forget_scale(); lv_label_set_text(lbl_forget_btn, "OLVIDADA"); }
  }
}
static void build_scale() {
  scr_scale = mk_screen();
  mk_header(scr_scale, "B\xC3\x81SCULA", back_settings_cb);
  lbl_scale_info = mk_label(scr_scale, &font_bebas_56, C_WHITE, "");
  lv_obj_set_pos(lbl_scale_info, 30, 130);
  mk_btn(scr_scale, 30, 440, 460, 120, &font_bebas_96, "", scale_cb, 0, &lbl_release_btn);
  mk_btn(scr_scale, 524, 440, 470, 120, &font_bebas_56, "OLVIDAR B\xC3\x81SCULA", scale_cb, 1, &lbl_forget_btn);
  lv_timer_create([](lv_timer_t *) { if (lv_scr_act() == scr_scale) scale_refresh(); }, 500, NULL);
  scale_refresh();
}

// --- Timer: inicio y parada automaticos
static lv_obj_t *btn_as, *btn_ap, *lbl_as, *lbl_ap;
static void timer_set_refresh() {
  btn_on(btn_as, cfg.auto_start);
  btn_on(btn_ap, cfg.auto_stop);
  lv_label_set_text(lbl_as, cfg.auto_start ? "S\xC3\x8D" : "NO");
  lv_label_set_text(lbl_ap, cfg.auto_stop ? "S\xC3\x8D" : "NO");
}
static void timer_set_cb(lv_event_t *e) {
  if ((intptr_t)lv_event_get_user_data(e) == 0) cfg.auto_start = !cfg.auto_start;
  else cfg.auto_stop = !cfg.auto_stop;
  cfg_dirty = true;
  timer_set_refresh();
}
static void build_timer() {
  scr_timer = mk_screen();
  mk_header(scr_timer, "TIMER", back_settings_cb);
  lv_obj_t *l;
  l = mk_label(scr_timer, &font_bebas_56, C_WHITE, "INICIO AUTOM\xC3\x81TICO\n(PRIMERA GOTA)"); lv_obj_set_pos(l, 30, 150);
  btn_as = mk_btn(scr_timer, 600, 150, 260, 110, &font_bebas_96, "", timer_set_cb, 0, &lbl_as);
  l = mk_label(scr_timer, &font_bebas_56, C_WHITE, "PARADA AUTOM\xC3\x81TICA\n(CESA EL CAUDAL)"); lv_obj_set_pos(l, 30, 340);
  btn_ap = mk_btn(scr_timer, 600, 340, 260, 110, &font_bebas_96, "", timer_set_cb, 1, &lbl_ap);
  timer_set_refresh();
}

// --- Actualizacion (OTA): pendiente, ver SPEC
static void build_update() {
  scr_update = mk_screen();
  mk_header(scr_update, "ACTUALIZAR", back_settings_cb);
  lv_obj_t *l = mk_label(scr_update, &font_bebas_56, C_WHITE,
    "PANEL  V" FW_VERSION "\n\n"
    "ACTUALIZACI\xC3\x93N POR BLE: EN DESARROLLO.\n"
    "SE HAR\xC3\x81 DESDE UNA P\xC3\x81GINA WEB\n"
    "(CHROME, WEB BLUETOOTH), PARA EL PANEL\n"
    "Y PARA EL COPROCESADOR.");
  lv_obj_set_pos(l, 30, 140);
}

// ================================================================ comprobacion de recortes (comando 'C')
// Recorre todas las pantallas con los textos del peor caso y avisa de:
//  - objetos o textos que se salen de la pantalla,
//  - textos mas anchos o altos que su etiqueta o su boton,
//  - solapes entre elementos de la misma pantalla.
static int chk_err, chk_margin;
static const char *chk_scr;
static bool chk_is_label(lv_obj_t *o) { return lv_obj_check_type(o, &lv_label_class); }
// Caja que ocupa de verdad: la del texto en las etiquetas, el objeto en lo demas
static void chk_box(lv_obj_t *o, lv_area_t *b) {
  lv_obj_get_coords(o, b);
  if (!chk_is_label(o)) return;
  const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
  lv_point_t sz;
  lv_txt_get_size(&sz, lv_label_get_text(o), f, lv_obj_get_style_text_letter_space(o, LV_PART_MAIN),
                  lv_obj_get_style_text_line_space(o, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  int cw = lv_area_get_width(b);
  lv_text_align_t al = lv_obj_get_style_text_align(o, LV_PART_MAIN);
  int x = b->x1;
  if (al == LV_TEXT_ALIGN_CENTER) x += (cw - sz.x) / 2;
  else if (al == LV_TEXT_ALIGN_RIGHT) x += cw - sz.x;
  b->x1 = x; b->x2 = x + sz.x - 1;
  // Vertical: tinta real de los glifos (la caja de linea tiene margen arriba y abajo)
  const char *t = lv_label_get_text(o);
  int lh = lv_font_get_line_height(f), bl = f->base_line;
  int top = lh, bot = 0, lines = 1;
  uint32_t i = 0;
  while (t[i]) {
    uint32_t cp = _lv_txt_encoded_next(t, &i);
    if (cp == '\n') { lines++; continue; }
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(f, &g, cp, 0) || g.box_h == 0) continue;
    top = min(top, lh - bl - (g.box_h + g.ofs_y));
    bot = max(bot, lh - bl - g.ofs_y);
  }
  if (bot == 0) { top = 0; bot = sz.y; }
  int y0 = b->y1;
  b->y1 = y0 + top;
  b->y2 = y0 + (lines - 1) * (lh + lv_obj_get_style_text_line_space(o, LV_PART_MAIN)) + bot - 1;
}
static const char *chk_name(lv_obj_t *o) {
  static char n[48];
  if (chk_is_label(o)) {
    snprintf(n, sizeof(n), "'%s'", lv_label_get_text(o));
    for (char *p = n; *p; p++) if (*p == '\n') *p = '|';
  } else if (lv_obj_get_child_cnt(o) && chk_is_label(lv_obj_get_child(o, 0))) {
    snprintf(n, sizeof(n), "[%s]", lv_label_get_text(lv_obj_get_child(o, 0)));
  } else snprintf(n, sizeof(n), "<obj>");
  return n;
}
static void chk_fail(const char *what, lv_obj_t *o, const lv_area_t *b) {
  chk_err++;
  Serial.printf("[CHK] %-10s %s %s  (%d,%d)-(%d,%d)\n", chk_scr, what, chk_name(o), b->x1, b->y1, b->x2, b->y2);
}
static void chk_obj(lv_obj_t *o, const lv_area_t *parent) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  lv_area_t b;
  chk_box(o, &b);
  if (b.x1 < 0 || b.y1 < 0 || b.x2 >= SCR_W || b.y2 >= SCR_H) chk_fail("FUERA DE PANTALLA", o, &b);
  else if (chk_margin && !parent &&
           (b.x1 < chk_margin || b.y1 < chk_margin || b.x2 >= SCR_W - chk_margin || b.y2 >= SCR_H - chk_margin))
    chk_fail("BAJO EL MARCO", o, &b);
  if (parent && (b.x1 < parent->x1 || b.y1 < parent->y1 || b.x2 > parent->x2 || b.y2 > parent->y2))
    chk_fail("NO CABE EN SU BOTON", o, &b);
  if (chk_is_label(o)) {
    lv_area_t c; lv_obj_get_coords(o, &c);
    if (lv_area_get_width(&b) > lv_area_get_width(&c) || lv_area_get_height(&b) > lv_area_get_height(&c))
      chk_fail("TEXTO RECORTADO", o, &b);
  }
  lv_area_t me; lv_obj_get_coords(o, &me);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) chk_obj(lv_obj_get_child(o, i), &me);
}
// Solapes permitidos: el marco de alerta y lo que va encima de la grafica a proposito
static bool chk_ok_overlap(lv_obj_t *a, lv_obj_t *b) {
  for (lv_obj_t *f : frame) if (a == f || b == f) return true;
  if (a == canvas || b == canvas) {
    lv_obj_t *o = (a == canvas) ? b : a;
    return o == lbl_dot || o == lbl_app || o == lbl_dbg;
  }
  return false;
}
static void chk_screen(lv_obj_t *s, const char *name) {
  chk_scr = name;
  chk_margin = (s == scr_main) ? BORDER : 0;   // el marco de alerta tapa 14 px por lado
  lv_obj_update_layout(s);
  uint32_t n = lv_obj_get_child_cnt(s);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *o = lv_obj_get_child(s, i);
    bool fr = false;
    for (lv_obj_t *f : frame) if (o == f) fr = true;
    if (!fr) chk_obj(o, nullptr);
  }
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *a = lv_obj_get_child(s, i);
    if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t ba; chk_box(a, &ba);
    for (uint32_t j = i + 1; j < n; j++) {
      lv_obj_t *b = lv_obj_get_child(s, j);
      if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) || chk_ok_overlap(a, b)) continue;
      lv_area_t bb, x; chk_box(b, &bb);
      if (_lv_area_intersect(&x, &ba, &bb)) {
        chk_err++;
        Serial.printf("[CHK] %-10s SOLAPE %s", chk_scr, chk_name(a));
        Serial.printf(" con %s  (%d,%d)-(%d,%d)\n", chk_name(b), x.x1, x.y1, x.x2, x.y2);
      }
    }
  }
}
static void check_all() {
  chk_err = 0;
  Cfg saved = cfg;
  char t[SEG_MAX + 1];
  // Pantalla principal: los dos disenos, cada modo, textos del peor caso
  for (uint8_t lay = 0; lay < 2; lay++)
    for (uint8_t m = 0; m < 3; m++) {
      cfg.layout = lay; cfg.mode = m; cfg.debug = 1;
      update_captions(); apply_layout(); update_status();
      lv_obj_clear_flag(lbl_app, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(lbl_dbg, "LAT 999 REFR 999 RAM 999K");
      lv_label_set_text(lbl_flow, "-12.5 G/S");
      lv_label_set_text(lbl_temp, "100.0\xC2\xB0" "C");
      lv_label_set_text(lbl_tgt, m == M_POUR ? "BLOOM 150G" : "OBJ 99.9");
      for (const char *w : {"-99.9", "-999", "2000"}) {
        for (const char *r : {"99.9", "-.-"}) {
          strcpy(t, w); seg_set(&seg_w, t);
          seg_set(&seg_t, "99:59");
          seg_set(&seg_r, r);
          char nm[16]; snprintf(nm, sizeof(nm), "MAIN %c/%s", lay ? 'B' : 'A', MODE_NAME[m]);
          chk_screen(scr_main, nm);
        }
      }
      if (m == M_POUR)
        for (const char *g : {"OBJ 1200", "BLOOM 120"}) { lv_label_set_text(lbl_tgt, g); chk_screen(scr_main, "MAIN tgt"); }
    }
  chk_screen(scr_menu, "MENU");
  // Recetas con los valores maximos
  for (uint8_t m = 0; m < 3; m++) {
    cfg.mode = m;
    cfg.esp_dose = cfg.wen_dose = 30; cfg.esp_ratio = cfg.wen_ratio = 4;
    cfg.pour_dose = 60; cfg.pour_ratio = 20; cfg.pour_bloom_g = 150; cfg.pour_bloom_s = 120;
    build_recipe();
    char nm[16]; snprintf(nm, sizeof(nm), "RECETA %s", MODE_NAME[m]);
    chk_screen(scr_recipe, nm);
  }
  cfg = saved;
  chk_screen(scr_settings, "AJUSTES");
  cfg.debug = 1; display_refresh(); lv_label_set_text(lbl_bright, "100%");
  chk_screen(scr_display, "PANTALLA");
  lv_label_set_text(lbl_scale_info, "DOT: DESCONECTADA\nMAC: (ninguna guardada)\n"
                    "PUENTE: APAGADO  \xC2\xB7  BOOKOO_SC_U 3ABE14\nWENDOUGEE: ESPERANDO");
  lv_label_set_text(lbl_release_btn, "RECONECTAR");
  lv_label_set_text(lbl_forget_btn, "\xC2\xBFSEGURO? TOCA OTRA VEZ");
  chk_screen(scr_scale, "BASCULA");
  lv_label_set_text(lbl_scale_info, "DOT: CONECTADA\nMAC: AA:BB:CC:DD:EE:FF\n"
                    "PUENTE: ACTIVO  \xC2\xB7  BOOKOO_SC_U 3ABE14\nWENDOUGEE: CONECTADA");
  chk_screen(scr_scale, "BASCULA");
  chk_screen(scr_timer, "TIMER");
  chk_screen(scr_update, "ACTUALIZAR");
  // Restaurar
  cfg = saved;
  cache_f[0] = cache_T[0] = cache_tgt[0] = cache_dot[0] = cache_app[0] = 0;
  lv_label_set_text(lbl_temp, "--.-\xC2\xB0" "C");   // sin sonda nadie la vuelve a escribir
  lv_label_set_text(lbl_flow, "0.0 G/S");
  update_captions(); apply_layout(); update_status(); update_target_label();
  display_refresh(); scale_refresh(); build_recipe();
  seg_set(&seg_w, "--.-"); seg_set(&seg_t, "0:00"); seg_set(&seg_r, "-.-");
  Serial.printf("[CHK] fin: %d problema(s)\n", chk_err);
}

static bool g_wdump = false;

// ================================================================ arranque
#define RGB_PANEL_CORE 1   // en el nucleo 0 hubo mucho parpadeo (ui_test_01 v0.3)
static esp_lcd_panel_handle_t g_panel = NULL;
static void panel_init_task(void *arg) {
  g_panel = waveshare_esp32_s3_rgb_lcd_init();
  xTaskNotifyGive((TaskHandle_t)arg);
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(921600);
  delay(100);
  Serial.println("\n[UI] Timemore Dot Panel v" FW_VERSION " arrancando");
  cfg_load();
  Serial.printf("[CFG] modo %s, brillo %u, dise\xC3\xB1o %c\n", MODE_NAME[cfg.mode], cfg.bright,
                cfg.layout ? 'B' : 'A');

  static esp_lcd_touch_handle_t tp_handle = NULL;
  tp_handle = touch_gt911_init();
  xTaskCreatePinnedToCore(panel_init_task, "rgb_init", 8192, xTaskGetCurrentTaskHandle(), 5, NULL, RGB_PANEL_CORE);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  wavesahre_rgb_lcd_bl_on();
  set_brightness(cfg.bright);
  ESP_ERROR_CHECK(lvgl_port_init(g_panel, tp_handle));

  if (lvgl_port_lock(-1)) {
    build_main();
    build_menu();
    scr_recipe = mk_screen();
    build_settings();
    build_display();
    build_scale();
    build_timer();
    build_update();
    lv_scr_load(scr_main);
    lv_timer_create(data_cb, 10, NULL);
    lv_timer_create(sim_cb, 100, NULL);
    lv_timer_set_period(_lv_disp_get_refr_timer(lv_disp_get_default()), 15);
    lvgl_port_unlock();
  }
  Serial.printf("[UI] interfaz creada; heap interno libre %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

  lvgl_port_set_wake_cb(on_sample_wake);
  bridge_set_sample_hook(sample_hook);
  bridge_begin(cfg.mode == M_WEN);
  Serial.printf("[UI] puente iniciado; heap interno libre %u KB (min %u KB)\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

// ================================================================ captura del framebuffer
static void fb_dump(int x, int y, int w, int h) {
  void *fb = NULL;
  esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb);
  if (!lvgl_port_lock(1000)) return;
  Serial.printf("\nFBDUMP 0 %d %d %d %d\n", x, y, w, h);
  for (int r = 0; r < h; r++)
    Serial.write((const uint8_t *)fb + ((size_t)(y + r) * SCR_W + x) * 2, w * 2);
  Serial.flush();
  Serial.print("\nFBEND\n");
  lvgl_port_unlock();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') fb_dump(0, 280, SCR_W, 320);
    else if (c == 'D') fb_dump(0, 0, SCR_W, SCR_H);
    else if (c == 'w') {   // volcado: muestras, respuestas de la Dot, comandos, toques y pixeles del peso
      extern volatile bool g_bridgeTrace;
      g_wdump = !g_wdump;
      g_bridgeTrace = g_wdump;
      Serial.printf("[W] volcado de muestras %s\n", g_wdump ? "ACTIVADO" : "desactivado");
    }
    else if (c == 'q') bridge_probe_dot();   // sonda: el tiempo hasta su respuesta mide la cola de la Dot
    else if (c == 'R') { Serial.println("[UI] reinicio"); Serial.flush(); esp_restart(); }   // vacia la cola de la Dot
    else if (c == 'x') {                               // liberar / reconectar la Dot
      if (bridge_scale_released()) { Serial.println("[UI] reconectar Dot"); bridge_reconnect_scale(); }
      else { Serial.println("[UI] liberar Dot"); bridge_release_scale(); }
    }
    else if (c == 'C') {                               // comprobar recortes y solapes
      if (lvgl_port_lock(1000)) { check_all(); lvgl_port_unlock(); }
    }
    else if (c == 'S') {
      g_sim = !g_sim;
      Serial.printf("[UI] datos simulados %s\n", g_sim ? "ACTIVADOS" : "desactivados");
      if (lvgl_port_lock(100)) { timer_reset(); lvgl_port_unlock(); }
    }
    else if (c == 'M') {                               // cambiar de modo (pruebas)
      if (lvgl_port_lock(100)) { set_mode((cfg.mode + 1) % 3); lvgl_port_unlock(); }
    }
    else if (c == 'n') {                               // recorrer las pantallas (pruebas)
      static int k = 0;
      if (lvgl_port_lock(100)) {
        lv_obj_t *all[] = {scr_main, scr_menu, scr_recipe, scr_settings, scr_display, scr_scale,
                           scr_timer, scr_update};
        k = (k + 1) % 8;
        if (all[k] == scr_recipe) build_recipe();
        go(all[k]);
        lvgl_port_unlock();
      }
    }
    else bridge_serial_cmd(c);
  }
  bridge_poll();

  // Volcado 'w': una linea por muestra (llegada en ms, peso, trama cruda)
  // y una marca TOQUE cada vez que se toca la pantalla (para sincronizar con el
  // momento fisico en que se apoya el peso).
  if (g_wdump) {
    static bool was_pressed = false;
    lv_indev_t *id = lv_indev_get_next(NULL);
    bool pr = id && id->proc.state == LV_INDEV_STATE_PRESSED;
    if (pr && !was_pressed)
      Serial.printf("[W] %lu ms  TOQUE ------------------------------\n",
                    (unsigned long)(esp_timer_get_time() / 1000));
    was_pressed = pr;
    // Respuestas de la Dot (tramas que no son peso)
    extern volatile uint8_t g_otherFrame[8][20];
    extern volatile uint8_t g_otherLen[8];
    extern volatile uint32_t g_otherUs[8];
    extern volatile uint32_t g_otherHead;
    static uint32_t other_tail = 0;
    while (other_tail != g_otherHead) {
      uint8_t k = other_tail % 8;
      char hex[64]; int n = 0;
      for (int i = 0; i < g_otherLen[k] && n < 60; i++) n += snprintf(hex + n, sizeof(hex) - n, "%02X ", g_otherFrame[k][i]);
      Serial.printf("[W] %lu ms  RX RESPUESTA %s\n", (unsigned long)(g_otherUs[k] / 1000), hex);
      other_tail++;
    }
    // Pixeles del numero del peso en el framebuffer (lo que lee el panel):
    // anota el momento en que cambian de verdad.
    static uint32_t last_hash = 0;
    void *fbp = NULL;
    esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fbp);
    if (fbp) {
      const uint16_t *fb = (const uint16_t *)fbp;
      uint32_t h = 2166136261u;
      int y0 = cfg.layout ? 62 : 66, y1 = cfg.layout ? 300 : 210, x1 = cfg.layout ? 614 : 352;
      for (int y = y0; y < y1; y += 3)
        for (int x = 14; x < x1; x += 3) h = (h ^ fb[y * SCR_W + x]) * 16777619u;
      if (h != last_hash) {
        if (last_hash) Serial.printf("[W] %lu ms  PIXELES DEL PESO CAMBIAN\n",
                                     (unsigned long)(esp_timer_get_time() / 1000));
        last_hash = h;
      }
    }
    static uint32_t last_seq = 0;
    BridgeSample s;
    bridge_get_sample(&s);
    if (s.seq != last_seq) {
      last_seq = s.seq;
      extern volatile uint8_t g_lastFrame[24];
      extern volatile uint8_t g_lastFrameLen;
      char hex[80]; int k = 0;
      for (int i = 0; i < g_lastFrameLen && k < 76; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02X ", g_lastFrame[i]);
      Serial.printf("[W] %lu ms  seq %lu  peso %.1f  | %s\n", (unsigned long)(s.arrival_us / 1000),
                    (unsigned long)s.seq, s.weight, hex);
    }
  }

  // Linea de depuracion y estadisticas (1 s)
  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    uint32_t dt = millis() - last;
    last = millis();
    uint32_t c = g_refr_count, s = g_refr_ms_sum, m = g_refr_ms_max;
    uint32_t px = g_refr_px_sum, bmp = g_bmp_us_sum;
    uint32_t ln = g_lat_n, lsum = g_lat_us_sum, lmax = g_lat_us_max;
    g_refr_count = 0; g_refr_ms_sum = 0; g_refr_ms_max = 0; g_refr_px_sum = 0;
    g_wait_us_sum = 0; g_bmp_us_sum = 0;
    g_lat_n = 0; g_lat_us_sum = 0; g_lat_us_max = 0;
    unsigned lat = ln ? lsum / ln / 1000 : 0;
    unsigned heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    if (cfg.debug && lvgl_port_lock(50)) {
      char b[64];
      snprintf(b, sizeof(b), "LAT %u REFR %u RAM %uK", lat, c ? (unsigned)(s / c) : 0, heap);
      lv_label_set_text(lbl_dbg, b);
      lvgl_port_unlock();
    }
    static uint8_t every5 = 0;
    if (++every5 >= 5) {
      every5 = 0;
      // por refresco: pixeles redibujados y tiempo copiando al framebuffer (PSRAM)
      Serial.printf("[PANEL] refresco %u ms (max %u), %.1f/s, %u kpx, copia %.1f ms | latencia muestra->pantalla %u ms "
                    "(max %u, n=%u) | heap int %u KB | modo %s\n",
                    c ? (unsigned)(s / c) : 0, (unsigned)m, c * 1000.0f / dt, c ? (unsigned)(px / c / 1000) : 0,
                    c ? bmp / 1000.0f / c : 0.0f, lat, (unsigned)(lmax / 1000), (unsigned)ln, heap, MODE_NAME[cfg.mode]);
    }
  }
  delay(5);
}

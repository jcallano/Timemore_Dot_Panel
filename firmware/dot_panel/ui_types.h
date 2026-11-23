// Tipos de la interfaz. Van en una cabecera porque Arduino genera los
// prototipos de las funciones del .ino antes de las definiciones de tipos.
#pragma once
#include "lvgl.h"

// Numero "por digitos": una celda por caracter; al cambiar un digito solo se
// redibuja esa celda (SPEC 3.5). Cada celda es una imagen RGB565 pre-renderizada
// al arrancar (DigSet): pintar un digito es copiar pixeles, no rasterizar la fuente.
#define SEG_MAX 7
#define DIG_CHARS "0123456789.:-"
#define DIG_N 13
struct DigSet {
  const lv_font_t *font;
  lv_img_dsc_t img[DIG_N];
};
struct Seg {
  lv_obj_t *cell[SEG_MAX];
  char txt[SEG_MAX + 1];
  const lv_font_t *font;
  int x, y, w;
};

// Tipos de la interfaz. Van en una cabecera porque Arduino genera los
// prototipos de las funciones del .ino antes de las definiciones de tipos.
#pragma once
#include "lvgl.h"

// Numero "por digitos": una celda (label) por caracter; al cambiar un digito
// solo se redibuja esa celda (SPEC 3.5).
#define SEG_MAX 7
struct Seg {
  lv_obj_t *cell[SEG_MAX];
  char txt[SEG_MAX + 1];
  const lv_font_t *font;
  int x, y, w;
};

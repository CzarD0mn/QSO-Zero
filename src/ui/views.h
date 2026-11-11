#ifndef QSOZERO_VIEWS_H
#define QSOZERO_VIEWS_H

#include "core/app.h"
#include "gfx.h"
#include "input.h"

#define LARGE_W 640
#define LARGE_H 360

void ui_render(app_t *a, surface_t *s);     /* picks layout from surface size */
void ui_key(app_t *a, const keyev_t *k);

#endif

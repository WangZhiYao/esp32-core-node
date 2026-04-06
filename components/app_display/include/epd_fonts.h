/**
 * @file    epd_fonts.h
 * @brief   Font definitions for e-paper display
 *
 * Ported from STMicroelectronics font library.
 * Original copyright (c) 2014 STMicroelectronics.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Max size of bitmap will based on a font24 (17x24) */
#define MAX_HEIGHT_FONT         41
#define MAX_WIDTH_FONT          32
#define OFFSET_BITMAP           54

/* ASCII font structure */
typedef struct _tFont {
    const uint8_t *table;
    uint16_t Width;
    uint16_t Height;
} sFONT;

extern sFONT Font24;
extern sFONT Font20;
extern sFONT Font16;
extern sFONT Font12;

#ifdef __cplusplus
}
#endif

/**
 * @file    epd_paint.h
 * @brief   Drawing library for e-paper displays
 *
 * Provides drawing primitives: points, lines, rectangles, circles,
 * characters, strings, and numbers for framebuffer-based e-paper displays.
 *
 * Ported from Waveshare GUI_Paint library V3.2 (2020-07-23).
 * Supports Scale=7 mode (4-bit per pixel, 2 pixels per byte) for
 * multi-color e-paper displays (e.g., 4-inch 6-color).
 *
 * Original copyright: Waveshare electronics.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "epd_fonts.h"

/**
 * Image attributes
 */
typedef struct {
    uint8_t  *Image;
    uint16_t Width;
    uint16_t Height;
    uint16_t WidthMemory;
    uint16_t HeightMemory;
    uint16_t Color;
    uint16_t Rotate;
    uint16_t Mirror;
    uint16_t WidthByte;
    uint16_t HeightByte;
    uint16_t Scale;
} PAINT;
extern PAINT Paint;

/**
 * Display rotate
 */
#define ROTATE_0            0
#define ROTATE_90           90
#define ROTATE_180          180
#define ROTATE_270          270

/**
 * Display Flip
 */
typedef enum {
    MIRROR_NONE       = 0x00,
    MIRROR_HORIZONTAL = 0x01,
    MIRROR_VERTICAL   = 0x02,
    MIRROR_ORIGIN     = 0x03,
} MIRROR_IMAGE;
#define MIRROR_IMAGE_DFT MIRROR_NONE

/**
 * Image color definitions
 */
#ifndef EPD_COLOR_WHITE
#define EPD_COLOR_WHITE          0xFF
#endif

#ifndef EPD_COLOR_BLACK
#define EPD_COLOR_BLACK          0x00
#endif

#ifndef EPD_COLOR_RED
#define EPD_COLOR_RED            EPD_COLOR_BLACK
#endif

#define IMAGE_BACKGROUND    EPD_COLOR_WHITE
#define FONT_FOREGROUND     EPD_COLOR_BLACK
#define FONT_BACKGROUND     EPD_COLOR_WHITE

/* 4 Gray level */
#define GRAY1 0x03  /* Blackest */
#define GRAY2 0x02
#define GRAY3 0x01  /* Gray */
#define GRAY4 0x00  /* White */

/**
 * The size of the point
 */
typedef enum {
    DOT_PIXEL_1X1 = 1,
    DOT_PIXEL_2X2,
    DOT_PIXEL_3X3,
    DOT_PIXEL_4X4,
    DOT_PIXEL_5X5,
    DOT_PIXEL_6X6,
    DOT_PIXEL_7X7,
    DOT_PIXEL_8X8,
} DOT_PIXEL;
#define DOT_PIXEL_DFT  DOT_PIXEL_1X1

/**
 * Point size fill style
 */
typedef enum {
    DOT_FILL_AROUND  = 1,
    DOT_FILL_RIGHTUP,
} DOT_STYLE;
#define DOT_STYLE_DFT  DOT_FILL_AROUND

/**
 * Line style, solid or dashed
 */
typedef enum {
    LINE_STYLE_SOLID = 0,
    LINE_STYLE_DOTTED,
} LINE_STYLE;

/**
 * Whether the graphic is filled
 */
typedef enum {
    DRAW_FILL_EMPTY = 0,
    DRAW_FILL_FULL,
} DRAW_FILL;

/**
 * Custom structure of a time attribute
 */
typedef struct {
    uint16_t Year;
    uint8_t  Month;
    uint8_t  Day;
    uint8_t  Hour;
    uint8_t  Min;
    uint8_t  Sec;
} PAINT_TIME;
extern PAINT_TIME sPaint_time;

/* Init and Clear */
void Paint_NewImage(uint8_t *image, uint16_t Width, uint16_t Height, uint16_t Rotate, uint16_t Color);
void Paint_SelectImage(uint8_t *image);
void Paint_SetRotate(uint16_t Rotate);
void Paint_SetMirroring(uint8_t mirror);
void Paint_SetPixel(uint16_t Xpoint, uint16_t Ypoint, uint16_t Color);
void Paint_SetScale(uint8_t scale);

void Paint_Clear(uint16_t Color);
void Paint_ClearWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t Color);

/* Drawing */
void Paint_DrawPoint(uint16_t Xpoint, uint16_t Ypoint, uint16_t Color, DOT_PIXEL Dot_Pixel, DOT_STYLE Dot_FillWay);
void Paint_DrawLine(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t Color, DOT_PIXEL Line_width, LINE_STYLE Line_Style);
void Paint_DrawRectangle(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t Color, DOT_PIXEL Line_width, DRAW_FILL Draw_Fill);
void Paint_DrawCircle(uint16_t X_Center, uint16_t Y_Center, uint16_t Radius, uint16_t Color, DOT_PIXEL Line_width, DRAW_FILL Draw_Fill);

/* Display string */
void Paint_DrawChar(uint16_t Xstart, uint16_t Ystart, const char Acsii_Char, sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background);
void Paint_DrawString_EN(uint16_t Xstart, uint16_t Ystart, const char *pString, sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background);
void Paint_DrawNum(uint16_t Xpoint, uint16_t Ypoint, int32_t Nummber, sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background);
void Paint_DrawTime(uint16_t Xstart, uint16_t Ystart, PAINT_TIME *pTime, sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background);

/* Bitmap */
void Paint_DrawBitmap(uint16_t Xstart, uint16_t Ystart, uint16_t Width, uint16_t Height,
                     const uint8_t *bitmap, uint16_t Color_Foreground, uint16_t Color_Background);
void Paint_DrawColorBitmap(uint16_t Xstart, uint16_t Ystart, uint16_t Width, uint16_t Height,
                          const uint8_t *bitmap, uint16_t Transparent);

#ifdef __cplusplus
}
#endif

/**
 * @file    epd_paint.c
 * @brief   Drawing library for e-paper displays
 *
 * Ported from Waveshare GUI_Paint library V3.2 (2020-07-23).
 * Original copyright: Waveshare electronics.
 *
 * Changes from original:
 *   - Replaced Arduino types (UBYTE/UWORD/UDOUBLE) with stdint types
 *   - Removed Debug() calls (replaced with ESP_LOGD where useful)
 *   - Removed Chinese font functions (Paint_DrawString_CN, cFONT)
 *   - Removed bitmap/image paste functions
 *   - Fixed Color_Background/Color_Foreground swap bug in Paint_DrawString_EN
 */

#include "epd_paint.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "epd_paint";

PAINT Paint;

/******************************************************************************
 * function: Create Image
 * parameter:
 *     image   :   Pointer to the image cache
 *     Width   :   The width of the picture
 *     Height  :   The height of the picture
 *     Rotate  :   Rotation angle (0, 90, 180, 270)
 *     Color   :   Initial color
 ******************************************************************************/
void Paint_NewImage(uint8_t *image, uint16_t Width, uint16_t Height, uint16_t Rotate, uint16_t Color)
{
    Paint.Image = NULL;
    Paint.Image = image;

    Paint.WidthMemory = Width;
    Paint.HeightMemory = Height;
    Paint.Color = Color;
    Paint.Scale = 2;
    Paint.WidthByte = (Width % 8 == 0) ? (Width / 8) : (Width / 8 + 1);
    Paint.HeightByte = Height;

    Paint.Rotate = Rotate;
    Paint.Mirror = MIRROR_NONE;

    if (Rotate == ROTATE_0 || Rotate == ROTATE_180) {
        Paint.Width = Width;
        Paint.Height = Height;
    } else {
        Paint.Width = Height;
        Paint.Height = Width;
    }
}

/******************************************************************************
 * function: Select Image
 * parameter:
 *     image : Pointer to the image cache
 ******************************************************************************/
void Paint_SelectImage(uint8_t *image)
{
    Paint.Image = image;
}

/******************************************************************************
 * function: Select Image Rotate
 * parameter:
 *     Rotate : 0, 90, 180, 270
 ******************************************************************************/
void Paint_SetRotate(uint16_t Rotate)
{
    if (Rotate == ROTATE_0 || Rotate == ROTATE_90 || Rotate == ROTATE_180 || Rotate == ROTATE_270) {
        Paint.Rotate = Rotate;
    } else {
        ESP_LOGD(TAG, "rotate = 0, 90, 180, 270");
    }
}

/******************************************************************************
 * function: Select Image mirror
 * parameter:
 *     mirror : Not mirror, Horizontal mirror, Vertical mirror, Origin mirror
 ******************************************************************************/
void Paint_SetMirroring(uint8_t mirror)
{
    if (mirror == MIRROR_NONE || mirror == MIRROR_HORIZONTAL ||
        mirror == MIRROR_VERTICAL || mirror == MIRROR_ORIGIN) {
        Paint.Mirror = mirror;
    } else {
        ESP_LOGD(TAG, "mirror should be MIRROR_NONE, MIRROR_HORIZONTAL, "
                 "MIRROR_VERTICAL or MIRROR_ORIGIN");
    }
}

/******************************************************************************
 * function: Set display scale
 * parameter:
 *     scale : 2 (B/W), 4 (grayscale), 7 (multi-color, 4-bit per pixel)
 ******************************************************************************/
void Paint_SetScale(uint8_t scale)
{
    if (scale == 2) {
        Paint.Scale = scale;
        Paint.WidthByte = (Paint.WidthMemory % 8 == 0) ? (Paint.WidthMemory / 8) : (Paint.WidthMemory / 8 + 1);
    } else if (scale == 4) {
        Paint.Scale = scale;
        Paint.WidthByte = (Paint.WidthMemory % 4 == 0) ? (Paint.WidthMemory / 4) : (Paint.WidthMemory / 4 + 1);
    } else if (scale == 6 || scale == 7) {
        Paint.Scale = 7;
        Paint.WidthByte = (Paint.WidthMemory % 2 == 0) ? (Paint.WidthMemory / 2) : (Paint.WidthMemory / 2 + 1);
    } else {
        ESP_LOGD(TAG, "Set Scale Input parameter error, Scale only supports: 2, 4, 7");
    }
}

/******************************************************************************
 * function: Draw Pixels
 * parameter:
 *     Xpoint : At point X
 *     Ypoint : At point Y
 *     Color  : Painted color
 ******************************************************************************/
void Paint_SetPixel(uint16_t Xpoint, uint16_t Ypoint, uint16_t Color)
{
    if (Xpoint >= Paint.Width || Ypoint >= Paint.Height) {
        return;
    }

    uint16_t X, Y;
    switch (Paint.Rotate) {
    case 0:
        X = Xpoint;
        Y = Ypoint;
        break;
    case 90:
        X = Paint.WidthMemory - Ypoint - 1;
        Y = Xpoint;
        break;
    case 180:
        X = Paint.WidthMemory - Xpoint - 1;
        Y = Paint.HeightMemory - Ypoint - 1;
        break;
    case 270:
        X = Ypoint;
        Y = Paint.HeightMemory - Xpoint - 1;
        break;
    default:
        return;
    }

    switch (Paint.Mirror) {
    case MIRROR_NONE:
        break;
    case MIRROR_HORIZONTAL:
        X = Paint.WidthMemory - X - 1;
        break;
    case MIRROR_VERTICAL:
        Y = Paint.HeightMemory - Y - 1;
        break;
    case MIRROR_ORIGIN:
        X = Paint.WidthMemory - X - 1;
        Y = Paint.HeightMemory - Y - 1;
        break;
    default:
        return;
    }

    if (X >= Paint.WidthMemory || Y >= Paint.HeightMemory) {
        return;
    }

    if (Paint.Scale == 2) {
        uint32_t Addr = X / 8 + Y * Paint.WidthByte;
        uint8_t Rdata = Paint.Image[Addr];
        if (Color == EPD_COLOR_BLACK)
            Paint.Image[Addr] = Rdata & ~(0x80 >> (X % 8));
        else
            Paint.Image[Addr] = Rdata | (0x80 >> (X % 8));
    } else if (Paint.Scale == 4) {
        uint32_t Addr = X / 4 + Y * Paint.WidthByte;
        Color = Color % 4;
        uint8_t Rdata = Paint.Image[Addr];
        Rdata = Rdata & (~(0xC0 >> ((X % 4) * 2)));
        Paint.Image[Addr] = Rdata | ((Color << 6) >> ((X % 4) * 2));
    } else if (Paint.Scale == 6 || Paint.Scale == 7 || Paint.Scale == 16) {
        uint32_t Addr = X / 2 + Y * Paint.WidthByte;
        uint8_t Rdata = Paint.Image[Addr];
        Rdata = Rdata & (~(0xF0 >> ((X % 2) * 4)));
        Paint.Image[Addr] = Rdata | ((Color << 4) >> ((X % 2) * 4));
    }
}

/******************************************************************************
 * function: Clear the color of the picture
 * parameter:
 *     Color : Painted color
 ******************************************************************************/
void Paint_Clear(uint16_t Color)
{
    if (Paint.Scale == 2) {
        for (uint16_t Y = 0; Y < Paint.HeightByte; Y++) {
            for (uint16_t X = 0; X < Paint.WidthByte; X++) {
                uint32_t Addr = X + Y * Paint.WidthByte;
                Paint.Image[Addr] = Color;
            }
        }
    } else if (Paint.Scale == 4) {
        for (uint16_t Y = 0; Y < Paint.HeightByte; Y++) {
            for (uint16_t X = 0; X < Paint.WidthByte; X++) {
                uint32_t Addr = X + Y * Paint.WidthByte;
                Paint.Image[Addr] = (Color << 6) | (Color << 4) | (Color << 2) | Color;
            }
        }
    } else if (Paint.Scale == 6 || Paint.Scale == 7 || Paint.Scale == 16) {
        for (uint16_t Y = 0; Y < Paint.HeightByte; Y++) {
            for (uint16_t X = 0; X < Paint.WidthByte; X++) {
                uint32_t Addr = X + Y * Paint.WidthByte;
                Paint.Image[Addr] = (Color << 4) | Color;
            }
        }
    }
}

/******************************************************************************
 * function: Clear the color of a window
 * parameter:
 *     Xstart : x starting point
 *     Ystart : Y starting point
 *     Xend   : x end point
 *     Yend   : y end point
 *     Color  : Painted color
 ******************************************************************************/
void Paint_ClearWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t Color)
{
    uint16_t X, Y;
    for (Y = Ystart; Y < Yend; Y++) {
        for (X = Xstart; X < Xend; X++) {
            Paint_SetPixel(X, Y, Color);
        }
    }
}

/******************************************************************************
 * function: Draw Point(Xpoint, Ypoint) Fill the color
 * parameter:
 *     Xpoint    : The Xpoint coordinate of the point
 *     Ypoint    : The Ypoint coordinate of the point
 *     Color     : Painted color
 *     Dot_Pixel : point size
 *     Dot_Style : point style
 ******************************************************************************/
void Paint_DrawPoint(uint16_t Xpoint, uint16_t Ypoint, uint16_t Color,
                     DOT_PIXEL Dot_Pixel, DOT_STYLE Dot_Style)
{
    if (Xpoint >= Paint.Width || Ypoint >= Paint.Height) {
        return;
    }

    int16_t XDir_Num, YDir_Num;
    if (Dot_Style == DOT_FILL_AROUND) {
        for (XDir_Num = 0; XDir_Num < 2 * Dot_Pixel - 1; XDir_Num++) {
            for (YDir_Num = 0; YDir_Num < 2 * Dot_Pixel - 1; YDir_Num++) {
                if (Xpoint + XDir_Num - Dot_Pixel < 0 || Ypoint + YDir_Num - Dot_Pixel < 0)
                    break;
                Paint_SetPixel(Xpoint + XDir_Num - Dot_Pixel, Ypoint + YDir_Num - Dot_Pixel, Color);
            }
        }
    } else {
        for (XDir_Num = 0; XDir_Num < Dot_Pixel; XDir_Num++) {
            for (YDir_Num = 0; YDir_Num < Dot_Pixel; YDir_Num++) {
                Paint_SetPixel(Xpoint + XDir_Num - 1, Ypoint + YDir_Num - 1, Color);
            }
        }
    }
}

/******************************************************************************
 * function: Draw a line of arbitrary slope
 * parameter:
 *     Xstart     : Starting X coordinate
 *     Ystart     : Starting Y coordinate
 *     Xend       : End point X coordinate
 *     Yend       : End point Y coordinate
 *     Color      : The color of the line segment
 *     Line_width : Line width
 *     Line_Style : Solid and dotted lines
 ******************************************************************************/
void Paint_DrawLine(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,
                    uint16_t Color, DOT_PIXEL Line_width, LINE_STYLE Line_Style)
{
    if (Xstart >= Paint.Width || Ystart >= Paint.Height ||
        Xend >= Paint.Width || Yend >= Paint.Height) {
        return;
    }

    uint16_t Xpoint = Xstart;
    uint16_t Ypoint = Ystart;
    int dx = (int)Xend - (int)Xstart >= 0 ? Xend - Xstart : Xstart - Xend;
    int dy = (int)Yend - (int)Ystart <= 0 ? Yend - Ystart : Ystart - Yend;

    int XAddway = Xstart < Xend ? 1 : -1;
    int YAddway = Ystart < Yend ? 1 : -1;

    int Esp = dx + dy;
    char Dotted_Len = 0;

    for (;;) {
        Dotted_Len++;
        if (Line_Style == LINE_STYLE_DOTTED && Dotted_Len % 3 == 0) {
            Paint_DrawPoint(Xpoint, Ypoint, Paint.Color, Line_width, DOT_STYLE_DFT);
            Dotted_Len = 0;
        } else {
            Paint_DrawPoint(Xpoint, Ypoint, Color, Line_width, DOT_STYLE_DFT);
        }
        if (2 * Esp >= dy) {
            if (Xpoint == Xend)
                break;
            Esp += dy;
            Xpoint += XAddway;
        }
        if (2 * Esp <= dx) {
            if (Ypoint == Yend)
                break;
            Esp += dx;
            Ypoint += YAddway;
        }
    }
}

/******************************************************************************
 * function: Draw a rectangle
 * parameter:
 *     Xstart     : Starting X coordinate
 *     Ystart     : Starting Y coordinate
 *     Xend       : End point X coordinate
 *     Yend       : End point Y coordinate
 *     Color      : The color of the rectangle
 *     Line_width : Line width
 *     Draw_Fill  : Whether to fill the inside of the rectangle
 ******************************************************************************/
void Paint_DrawRectangle(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,
                         uint16_t Color, DOT_PIXEL Line_width, DRAW_FILL Draw_Fill)
{
    if (Xstart >= Paint.Width || Ystart >= Paint.Height ||
        Xend >= Paint.Width || Yend >= Paint.Height) {
        return;
    }

    if (Draw_Fill) {
        uint16_t Ypoint;
        for (Ypoint = Ystart; Ypoint < Yend; Ypoint++) {
            Paint_DrawLine(Xstart, Ypoint, Xend, Ypoint, Color, Line_width, LINE_STYLE_SOLID);
        }
    } else {
        Paint_DrawLine(Xstart, Ystart, Xend, Ystart, Color, Line_width, LINE_STYLE_SOLID);
        Paint_DrawLine(Xstart, Ystart, Xstart, Yend, Color, Line_width, LINE_STYLE_SOLID);
        Paint_DrawLine(Xend, Yend, Xend, Ystart, Color, Line_width, LINE_STYLE_SOLID);
        Paint_DrawLine(Xend, Yend, Xstart, Yend, Color, Line_width, LINE_STYLE_SOLID);
    }
}

/******************************************************************************
 * function: Draw a circle
 * parameter:
 *     X_Center   : Center X coordinate
 *     Y_Center   : Center Y coordinate
 *     Radius     : Circle radius
 *     Color      : The color of the circle
 *     Line_width : Line width
 *     Draw_Fill  : Whether to fill the inside of the circle
 ******************************************************************************/
void Paint_DrawCircle(uint16_t X_Center, uint16_t Y_Center, uint16_t Radius,
                      uint16_t Color, DOT_PIXEL Line_width, DRAW_FILL Draw_Fill)
{
    if (X_Center >= Paint.Width || Y_Center >= Paint.Height) {
        return;
    }

    int16_t XCurrent, YCurrent;
    XCurrent = 0;
    YCurrent = Radius;

    int16_t Esp = 3 - (Radius << 1);

    int16_t sCountY;
    if (Draw_Fill == DRAW_FILL_FULL) {
        while (XCurrent <= YCurrent) {
            for (sCountY = XCurrent; sCountY <= YCurrent; sCountY++) {
                Paint_DrawPoint(X_Center + XCurrent, Y_Center + sCountY, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center - XCurrent, Y_Center + sCountY, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center - sCountY, Y_Center + XCurrent, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center - sCountY, Y_Center - XCurrent, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center - XCurrent, Y_Center - sCountY, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center + XCurrent, Y_Center - sCountY, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center + sCountY, Y_Center - XCurrent, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
                Paint_DrawPoint(X_Center + sCountY, Y_Center + XCurrent, Color, DOT_PIXEL_DFT, DOT_STYLE_DFT);
            }
            if (Esp < 0)
                Esp += 4 * XCurrent + 6;
            else {
                Esp += 10 + 4 * (XCurrent - YCurrent);
                YCurrent--;
            }
            XCurrent++;
        }
    } else {
        while (XCurrent <= YCurrent) {
            Paint_DrawPoint(X_Center + XCurrent, Y_Center + YCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center - XCurrent, Y_Center + YCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center - YCurrent, Y_Center + XCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center - YCurrent, Y_Center - XCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center - XCurrent, Y_Center - YCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center + XCurrent, Y_Center - YCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center + YCurrent, Y_Center - XCurrent, Color, Line_width, DOT_STYLE_DFT);
            Paint_DrawPoint(X_Center + YCurrent, Y_Center + XCurrent, Color, Line_width, DOT_STYLE_DFT);

            if (Esp < 0)
                Esp += 4 * XCurrent + 6;
            else {
                Esp += 10 + 4 * (XCurrent - YCurrent);
                YCurrent--;
            }
            XCurrent++;
        }
    }
}

/******************************************************************************
 * function: Show English characters
 * parameter:
 *     Xpoint           : X coordinate
 *     Ypoint           : Y coordinate
 *     Acsii_Char       : The English character to display
 *     Font             : A structure pointer that displays a character size
 *     Color_Foreground : Select the foreground color
 *     Color_Background : Select the background color
 ******************************************************************************/
void Paint_DrawChar(uint16_t Xpoint, uint16_t Ypoint, const char Acsii_Char,
                    sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background)
{
    uint16_t Page, Column;

    if (Xpoint >= Paint.Width || Ypoint >= Paint.Height) {
        return;
    }

    uint32_t Char_Offset = (Acsii_Char - ' ') * Font->Height * (Font->Width / 8 + (Font->Width % 8 ? 1 : 0));
    const unsigned char *ptr = &Font->table[Char_Offset];

    for (Page = 0; Page < Font->Height; Page++) {
        for (Column = 0; Column < Font->Width; Column++) {
            if (FONT_BACKGROUND == Color_Background) {
                if (*ptr & (0x80 >> (Column % 8)))
                    Paint_SetPixel(Xpoint + Column, Ypoint + Page, Color_Foreground);
            } else {
                if (*ptr & (0x80 >> (Column % 8))) {
                    Paint_SetPixel(Xpoint + Column, Ypoint + Page, Color_Foreground);
                } else {
                    Paint_SetPixel(Xpoint + Column, Ypoint + Page, Color_Background);
                }
            }
            if (Column % 8 == 7)
                ptr++;
        }
        if (Font->Width % 8 != 0)
            ptr++;
    }
}

/******************************************************************************
 * function: Display the string
 * parameter:
 *     Xstart           : X coordinate
 *     Ystart           : Y coordinate
 *     pString          : The first address of the English string to be displayed
 *     Font             : A structure pointer that displays a character size
 *     Color_Foreground : Select the foreground color
 *     Color_Background : Select the background color
 *
 * NOTE: The original Waveshare code had Color_Background and Color_Foreground
 *       swapped in the call to Paint_DrawChar. This has been fixed here.
 ******************************************************************************/
void Paint_DrawString_EN(uint16_t Xstart, uint16_t Ystart, const char *pString,
                         sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background)
{
    uint16_t Xpoint = Xstart;
    uint16_t Ypoint = Ystart;

    if (Xstart >= Paint.Width || Ystart >= Paint.Height) {
        return;
    }

    while (*pString != '\0') {
        if ((Xpoint + Font->Width) > Paint.Width) {
            Xpoint = Xstart;
            Ypoint += Font->Height;
        }

        if ((Ypoint + Font->Height) > Paint.Height) {
            Xpoint = Xstart;
            Ypoint = Ystart;
        }

        /* BUG FIX: Original code had Color_Background, Color_Foreground (swapped) */
        Paint_DrawChar(Xpoint, Ypoint, *pString, Font, Color_Foreground, Color_Background);

        pString++;
        Xpoint += Font->Width;
    }
}

/******************************************************************************
 * function: Display number
 * parameter:
 *     Xpoint           : X coordinate
 *     Ypoint           : Y coordinate
 *     Nummber          : The number displayed
 *     Font             : A structure pointer that displays a character size
 *     Color_Foreground : Select the foreground color
 *     Color_Background : Select the background color
 ******************************************************************************/
void Paint_DrawNum(uint16_t Xpoint, uint16_t Ypoint, int32_t Nummber,
                   sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background)
{
    if (Xpoint >= Paint.Width || Ypoint >= Paint.Height) {
        return;
    }

    char num_str[16];
    snprintf(num_str, sizeof(num_str), "%" PRId32, Nummber);
    Paint_DrawString_EN(Xpoint, Ypoint, num_str, Font, Color_Foreground, Color_Background);
}

/******************************************************************************
 * function: Display time
 * parameter:
 *     Xstart           : X coordinate
 *     Ystart           : Y coordinate
 *     pTime            : Time-related structure
 *     Font             : A structure pointer that displays a character size
 *     Color_Foreground : Select the foreground color
 *     Color_Background : Select the background color
 ******************************************************************************/
void Paint_DrawTime(uint16_t Xstart, uint16_t Ystart, PAINT_TIME *pTime, sFONT *Font,
                    uint16_t Color_Foreground, uint16_t Color_Background)
{
    uint8_t value[10] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

    uint16_t Dx = Font->Width;

    Paint_DrawChar(Xstart,                              Ystart, value[pTime->Hour / 10], Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx,                         Ystart, value[pTime->Hour % 10], Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx + Dx / 4 + Dx / 2,      Ystart, ':',                     Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx * 2 + Dx / 2,           Ystart, value[pTime->Min / 10],  Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx * 3 + Dx / 2,           Ystart, value[pTime->Min % 10],  Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx * 4 + Dx / 2 - Dx / 4,  Ystart, ':',                     Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx * 5,                     Ystart, value[pTime->Sec / 10],  Font, Color_Foreground, Color_Background);
    Paint_DrawChar(Xstart + Dx * 6,                     Ystart, value[pTime->Sec % 10],  Font, Color_Foreground, Color_Background);
}

/******************************************************************************
 * function: Draw a 1-bit monochrome bitmap
 * parameter:
 *     Xstart           : X coordinate of top-left corner
 *     Ystart           : Y coordinate of top-left corner
 *     Width            : Bitmap width in pixels
 *     Height           : Bitmap height in pixels
 *     bitmap           : Pointer to 1-bit packed bitmap data (MSB first, row-major)
 *     Color_Foreground : Color for '1' bits
 *     Color_Background : Color for '0' bits (use FONT_BACKGROUND for transparent)
 ******************************************************************************/
void Paint_DrawBitmap(uint16_t Xstart, uint16_t Ystart, uint16_t Width, uint16_t Height,
                     const uint8_t *bitmap, uint16_t Color_Foreground, uint16_t Color_Background)
{
    uint16_t byte_width = (Width + 7) / 8;

    for (uint16_t row = 0; row < Height; row++) {
        for (uint16_t col = 0; col < Width; col++) {
            uint16_t byte_idx = row * byte_width + col / 8;
            uint8_t bit_mask = 0x80 >> (col % 8);

            if (bitmap[byte_idx] & bit_mask) {
                Paint_SetPixel(Xstart + col, Ystart + row, Color_Foreground);
            } else if (Color_Background != FONT_BACKGROUND) {
                Paint_SetPixel(Xstart + col, Ystart + row, Color_Background);
            }
        }
    }
}

/******************************************************************************
 * function: Draw a 4-bit color bitmap (2 pixels per byte, high nibble = left)
 * parameter:
 *     Xstart      : X coordinate of top-left corner
 *     Ystart      : Y coordinate of top-left corner
 *     Width       : Bitmap width in pixels (must be even)
 *     Height      : Bitmap height in pixels
 *     bitmap      : Pointer to 4-bit packed color data
 *     Transparent : Color value to treat as transparent (e.g. 0xF)
 ******************************************************************************/
void Paint_DrawColorBitmap(uint16_t Xstart, uint16_t Ystart, uint16_t Width, uint16_t Height,
                          const uint8_t *bitmap, uint16_t Transparent)
{
    uint16_t byte_width = Width / 2;

    for (uint16_t row = 0; row < Height; row++) {
        for (uint16_t col = 0; col < Width; col++) {
            uint16_t byte_idx = row * byte_width + col / 2;
            uint8_t color;
            if (col % 2 == 0) {
                color = (bitmap[byte_idx] >> 4) & 0x0F;
            } else {
                color = bitmap[byte_idx] & 0x0F;
            }
            if (color != Transparent) {
                Paint_SetPixel(Xstart + col, Ystart + row, color);
            }
        }
    }
}

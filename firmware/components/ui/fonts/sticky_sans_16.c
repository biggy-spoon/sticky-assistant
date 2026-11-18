/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --font /home/thecc/.claude/jobs/a63163eb/tmp/fonts_dl/liberation-fonts-ttf-2.1.5/LiberationSans-Regular.ttf -r 0x20-0x7E,0xB0,0x2022 --font /mnt/c/Users/thecc/Documents/git/sticky-claude/.claude/worktrees/fix-button-stack-overflow/firmware/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF00C,0xF053,0xF054,0xF077,0xF078 --size 16 --bpp 1 --format lvgl --no-compress --lv-include lvgl.h -o /mnt/c/Users/thecc/Documents/git/sticky-claude/.claude/worktrees/sticky-os-ui/firmware/components/ui/fonts/sticky_sans_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef STICKY_SANS_16
#define STICKY_SANS_16 1
#endif

#if STICKY_SANS_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0x10,

    /* U+0022 "\"" */
    0x99, 0x90,

    /* U+0023 "#" */
    0x11, 0x8, 0x84, 0x82, 0x4f, 0xf9, 0x10, 0x88,
    0x48, 0xff, 0x22, 0x11, 0x8, 0x80,

    /* U+0024 "$" */
    0x3e, 0x25, 0x92, 0x69, 0x6, 0x81, 0xe0, 0x3c,
    0x13, 0x8, 0xa4, 0x5a, 0x47, 0xc0, 0x80, 0x40,

    /* U+0025 "%" */
    0x70, 0x44, 0x46, 0x22, 0x21, 0x13, 0x8, 0x90,
    0x45, 0x3a, 0x6a, 0x2e, 0x91, 0xc, 0x88, 0x44,
    0x46, 0x22, 0x20, 0xe0,

    /* U+0026 "&" */
    0x1c, 0x8, 0x82, 0x20, 0x88, 0x2c, 0xe, 0x7,
    0x1a, 0x24, 0x8d, 0x21, 0x8c, 0x61, 0xf7,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x26, 0x44, 0x88, 0x88, 0x88, 0x84, 0x46, 0x20,

    /* U+0029 ")" */
    0x46, 0x22, 0x11, 0x11, 0x11, 0x12, 0x26, 0x40,

    /* U+002A "*" */
    0x21, 0x3e, 0x45, 0x8,

    /* U+002B "+" */
    0x10, 0x10, 0x10, 0xff, 0x10, 0x10, 0x10, 0x10,

    /* U+002C "," */
    0xf0,

    /* U+002D "-" */
    0xf0,

    /* U+002E "." */
    0xc0,

    /* U+002F "/" */
    0x11, 0x13, 0x22, 0x64, 0x44, 0xc8,

    /* U+0030 "0" */
    0x38, 0x8b, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x9, 0x11, 0xc0,

    /* U+0031 "1" */
    0x30, 0xe3, 0x40, 0x81, 0x2, 0x4, 0x8, 0x10,
    0x20, 0x47, 0xf0,

    /* U+0032 "2" */
    0x3c, 0x8f, 0x8, 0x10, 0x20, 0x83, 0xc, 0x30,
    0xc3, 0x7, 0xf0,

    /* U+0033 "3" */
    0x3c, 0x66, 0x42, 0x2, 0x6, 0x18, 0x6, 0x1,
    0x1, 0x41, 0x62, 0x3c,

    /* U+0034 "4" */
    0x4, 0xc, 0x1c, 0x14, 0x24, 0x24, 0x44, 0xc4,
    0xff, 0x4, 0x4, 0x4,

    /* U+0035 "5" */
    0x7f, 0x40, 0x40, 0x40, 0x40, 0x7c, 0x42, 0x1,
    0x1, 0xc1, 0x42, 0x3c,

    /* U+0036 "6" */
    0x3c, 0x8d, 0x4, 0xb, 0x98, 0xa0, 0xc1, 0x83,
    0x5, 0x11, 0xc0,

    /* U+0037 "7" */
    0xfe, 0xc, 0x10, 0x40, 0x82, 0x4, 0x8, 0x20,
    0x40, 0x81, 0x0,

    /* U+0038 "8" */
    0x7d, 0x8e, 0xc, 0x1c, 0x67, 0x31, 0xc1, 0x83,
    0x7, 0x1b, 0xe0,

    /* U+0039 "9" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x51, 0x9d, 0x2,
    0xb, 0x13, 0xc0,

    /* U+003A ":" */
    0x80, 0x80,

    /* U+003B ";" */
    0x80, 0xe0,

    /* U+003C "<" */
    0x1, 0x7, 0x1c, 0xe0, 0x80, 0xe0, 0x1c, 0x7,
    0x1,

    /* U+003D "=" */
    0xff, 0x0, 0x0, 0x0, 0xff,

    /* U+003E ">" */
    0x80, 0xe0, 0x38, 0x7, 0x1, 0x7, 0x38, 0xe0,
    0x80,

    /* U+003F "?" */
    0x3c, 0x62, 0xc1, 0x1, 0x1, 0x2, 0x4, 0x8,
    0x10, 0x0, 0x0, 0x10,

    /* U+0040 "@" */
    0x7, 0xe0, 0x60, 0xc2, 0x0, 0x91, 0xeb, 0x4c,
    0x66, 0x21, 0x99, 0x4, 0x64, 0x11, 0x90, 0xce,
    0x47, 0x2c, 0xf7, 0x10, 0x0, 0x30, 0x70, 0x3f,
    0x0,

    /* U+0041 "A" */
    0xc, 0x1, 0xc0, 0x38, 0xd, 0x81, 0xb0, 0x66,
    0xc, 0x61, 0xc, 0x7f, 0x8c, 0x19, 0x3, 0x60,
    0x20,

    /* U+0042 "B" */
    0xfe, 0x83, 0x81, 0x81, 0x83, 0xfc, 0x83, 0x81,
    0x81, 0x81, 0x82, 0xfc,

    /* U+0043 "C" */
    0x1f, 0x18, 0x64, 0xa, 0x0, 0x80, 0x20, 0x8,
    0x2, 0x0, 0x80, 0x90, 0x26, 0x18, 0x78,

    /* U+0044 "D" */
    0xfc, 0x41, 0xa0, 0x50, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x81, 0xc0, 0xa0, 0x9f, 0x80,

    /* U+0045 "E" */
    0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0xfe, 0x80,
    0x80, 0x80, 0x80, 0xff,

    /* U+0046 "F" */
    0xff, 0x2, 0x4, 0x8, 0x10, 0x3f, 0xc0, 0x81,
    0x2, 0x4, 0x0,

    /* U+0047 "G" */
    0x1f, 0x18, 0x64, 0xe, 0x0, 0x80, 0x20, 0x8,
    0x3e, 0x1, 0x80, 0x50, 0x16, 0xc, 0x7c,

    /* U+0048 "H" */
    0x80, 0xc0, 0x60, 0x30, 0x18, 0xc, 0x7, 0xff,
    0x1, 0x80, 0xc0, 0x60, 0x30, 0x10,

    /* U+0049 "I" */
    0xff, 0xf0,

    /* U+004A "J" */
    0x1c, 0x10, 0x41, 0x4, 0x10, 0x41, 0x7, 0x1c,
    0xde,

    /* U+004B "K" */
    0x83, 0x43, 0x23, 0x13, 0xb, 0x7, 0x83, 0x41,
    0x30, 0x8c, 0x43, 0x20, 0xd0, 0x60,

    /* U+004C "L" */
    0x81, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x81,
    0x2, 0x7, 0xf0,

    /* U+004D "M" */
    0xc0, 0x78, 0xf, 0x83, 0xd0, 0x5a, 0xb, 0x63,
    0x64, 0x4c, 0xd9, 0x8a, 0x31, 0x46, 0x38, 0xc2,
    0x10,

    /* U+004E "N" */
    0xc0, 0xe0, 0x78, 0x36, 0x19, 0xc, 0xc6, 0x33,
    0x9, 0x86, 0xc1, 0xe0, 0x70, 0x30,

    /* U+004F "O" */
    0x1f, 0x4, 0x11, 0x1, 0x40, 0x18, 0x3, 0x0,
    0x60, 0xc, 0x1, 0x80, 0x28, 0x8, 0x82, 0xf,
    0x80,

    /* U+0050 "P" */
    0xfe, 0x83, 0x81, 0x81, 0x81, 0x82, 0xfc, 0x80,
    0x80, 0x80, 0x80, 0x80,

    /* U+0051 "Q" */
    0x1f, 0x4, 0x11, 0x1, 0x40, 0x18, 0x3, 0x0,
    0x60, 0xc, 0x1, 0x80, 0x28, 0x9, 0x83, 0xf,
    0x80, 0x60, 0x6, 0x0, 0x70,

    /* U+0052 "R" */
    0xfe, 0x40, 0xe0, 0x30, 0x18, 0xc, 0xf, 0xf9,
    0xc, 0x83, 0x40, 0xa0, 0x70, 0x10,

    /* U+0053 "S" */
    0x1e, 0x30, 0x90, 0x28, 0x6, 0x1, 0xf0, 0x3c,
    0x3, 0x0, 0xa0, 0x58, 0x47, 0xc0,

    /* U+0054 "T" */
    0xff, 0x84, 0x2, 0x1, 0x0, 0x80, 0x40, 0x20,
    0x10, 0x8, 0x4, 0x2, 0x1, 0x0,

    /* U+0055 "U" */
    0x80, 0xc0, 0x60, 0x30, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x80, 0xc0, 0x50, 0x47, 0xc0,

    /* U+0056 "V" */
    0xc0, 0x48, 0x19, 0x83, 0x30, 0x42, 0x18, 0x63,
    0x4, 0x40, 0x98, 0x1a, 0x1, 0x40, 0x38, 0x6,
    0x0,

    /* U+0057 "W" */
    0xc1, 0x7, 0x87, 0xd, 0xa, 0x13, 0x14, 0x26,
    0x28, 0xc4, 0xd9, 0x89, 0x92, 0x1a, 0x24, 0x34,
    0x58, 0x38, 0xe0, 0x60, 0xc0, 0xc1, 0x80,

    /* U+0058 "X" */
    0x60, 0xc8, 0x63, 0x10, 0x6c, 0xa, 0x3, 0x0,
    0xe0, 0x68, 0x13, 0xc, 0x66, 0x9, 0x3,

    /* U+0059 "Y" */
    0xc1, 0xa0, 0x98, 0xc4, 0x43, 0x60, 0xa0, 0x20,
    0x10, 0x8, 0x4, 0x2, 0x1, 0x0,

    /* U+005A "Z" */
    0xff, 0x1, 0x81, 0x80, 0x80, 0xc0, 0xc0, 0x40,
    0x60, 0x20, 0x20, 0x30, 0x1f, 0xf0,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x92, 0x49, 0x38,

    /* U+005C "\\" */
    0x8c, 0x44, 0x46, 0x22, 0x31, 0x11,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x24, 0x92, 0x78,

    /* U+005E "^" */
    0x18, 0x50, 0x93, 0x24, 0x78, 0x40,

    /* U+005F "_" */
    0xff, 0x80,

    /* U+0060 "`" */
    0xc4, 0x20,

    /* U+0061 "a" */
    0x7c, 0xc2, 0x2, 0x7e, 0xc2, 0x82, 0x86, 0x86,
    0x7b,

    /* U+0062 "b" */
    0x81, 0x2, 0x5, 0xec, 0x50, 0x60, 0xc1, 0x83,
    0x7, 0x15, 0xc0,

    /* U+0063 "c" */
    0x39, 0x38, 0x60, 0x82, 0x8, 0x53, 0x38,

    /* U+0064 "d" */
    0x2, 0x4, 0xb, 0xd4, 0x70, 0x60, 0xc1, 0x83,
    0x5, 0x1b, 0xd0,

    /* U+0065 "e" */
    0x3c, 0x46, 0x82, 0x82, 0xfe, 0x80, 0x80, 0x46,
    0x3c,

    /* U+0066 "f" */
    0x74, 0x4f, 0x44, 0x44, 0x44, 0x44,

    /* U+0067 "g" */
    0x7a, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xa3, 0x7a,
    0x7, 0x1b, 0xe0,

    /* U+0068 "h" */
    0x81, 0x2, 0x5, 0xec, 0x70, 0x60, 0xc1, 0x83,
    0x6, 0xc, 0x10,

    /* U+0069 "i" */
    0x9f, 0xf0,

    /* U+006A "j" */
    0x41, 0x55, 0x55, 0x5c,

    /* U+006B "k" */
    0x81, 0x2, 0x4, 0x69, 0x92, 0x28, 0x70, 0xb1,
    0x32, 0x34, 0x20,

    /* U+006C "l" */
    0xff, 0xf0,

    /* U+006D "m" */
    0xb9, 0xd8, 0xc6, 0x10, 0xc2, 0x18, 0x43, 0x8,
    0x61, 0xc, 0x21, 0x84, 0x20,

    /* U+006E "n" */
    0xbd, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x82,

    /* U+006F "o" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x60, 0xa2, 0x38,

    /* U+0070 "p" */
    0xbd, 0x8a, 0xc, 0x18, 0x30, 0x60, 0xe2, 0xb9,
    0x2, 0x4, 0x0,

    /* U+0071 "q" */
    0x7a, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xa3, 0x7a,
    0x4, 0x8, 0x10,

    /* U+0072 "r" */
    0xbc, 0x88, 0x88, 0x88, 0x80,

    /* U+0073 "s" */
    0x7a, 0x38, 0x30, 0x78, 0x30, 0x61, 0x78,

    /* U+0074 "t" */
    0x44, 0xf4, 0x44, 0x44, 0x44, 0x70,

    /* U+0075 "u" */
    0x83, 0x6, 0xc, 0x18, 0x30, 0x60, 0xc3, 0x7a,

    /* U+0076 "v" */
    0xc3, 0x42, 0x42, 0x66, 0x24, 0x24, 0x3c, 0x18,
    0x18,

    /* U+0077 "w" */
    0x86, 0x2c, 0x62, 0x4e, 0x24, 0xa6, 0x49, 0x46,
    0x94, 0x39, 0x43, 0x1c, 0x31, 0x80,

    /* U+0078 "x" */
    0x42, 0x66, 0x24, 0x18, 0x18, 0x3c, 0x24, 0x66,
    0x43,

    /* U+0079 "y" */
    0x83, 0x8f, 0x1a, 0x26, 0xc5, 0xa, 0x1c, 0x10,
    0x60, 0x86, 0x0,

    /* U+007A "z" */
    0xfc, 0x31, 0x84, 0x30, 0x84, 0x30, 0xfc,

    /* U+007B "{" */
    0x19, 0x8, 0x42, 0x10, 0x98, 0x21, 0x8, 0x42,
    0x10, 0x60,

    /* U+007C "|" */
    0xff, 0xfe,

    /* U+007D "}" */
    0xc1, 0x8, 0x42, 0x10, 0x83, 0x21, 0x8, 0x42,
    0x13, 0x0,

    /* U+007E "~" */
    0xf0, 0xf,

    /* U+00B0 "°" */
    0x74, 0x63, 0x17, 0x0,

    /* U+2022 "•" */
    0x6f, 0xf6,

    /* U+F00C "" */
    0x0, 0x6, 0x0, 0xf, 0x0, 0x1f, 0x0, 0x3e,
    0x60, 0x7c, 0xf0, 0xf8, 0xf9, 0xf0, 0x7f, 0xe0,
    0x3f, 0xc0, 0x1f, 0x80, 0xf, 0x0, 0x6, 0x0,

    /* U+F053 "" */
    0x3, 0x7, 0xe, 0x1c, 0x38, 0x70, 0xe0, 0x70,
    0x38, 0x1c, 0xe, 0x7, 0x3,

    /* U+F054 "" */
    0xc0, 0xe0, 0x70, 0x38, 0x1c, 0xe, 0x7, 0xe,
    0x1c, 0x38, 0x70, 0xe0, 0xc0,

    /* U+F077 "" */
    0x3, 0x0, 0x1e, 0x0, 0xfc, 0x7, 0xf8, 0x3c,
    0xf1, 0xe1, 0xef, 0x3, 0xf0, 0x3,

    /* U+F078 "" */
    0x40, 0xb, 0x80, 0x77, 0x87, 0x8f, 0x3c, 0x1f,
    0xe0, 0x3f, 0x0, 0x78, 0x0, 0xc0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 71, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 71, .box_w = 1, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 91, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 5, .adv_w = 142, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 19, .adv_w = 142, .box_w = 9, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 35, .adv_w = 228, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 55, .adv_w = 171, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 70, .adv_w = 49, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 71, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 79, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 87, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 91, .adv_w = 150, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 99, .adv_w = 71, .box_w = 1, .box_h = 4, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 100, .adv_w = 85, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 101, .adv_w = 71, .box_w = 1, .box_h = 2, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 102, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 108, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 119, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 130, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 141, .adv_w = 142, .box_w = 8, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 153, .adv_w = 142, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 165, .adv_w = 142, .box_w = 8, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 177, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 188, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 199, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 210, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 221, .adv_w = 71, .box_w = 1, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 223, .adv_w = 71, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 225, .adv_w = 150, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 234, .adv_w = 150, .box_w = 8, .box_h = 5, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 239, .adv_w = 150, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 248, .adv_w = 142, .box_w = 8, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 260, .adv_w = 260, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 285, .adv_w = 171, .box_w = 11, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 302, .adv_w = 171, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 314, .adv_w = 185, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 329, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 343, .adv_w = 171, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 355, .adv_w = 156, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 366, .adv_w = 199, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 381, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 395, .adv_w = 71, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 397, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 406, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 420, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 431, .adv_w = 213, .box_w = 11, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 448, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 462, .adv_w = 199, .box_w = 11, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 479, .adv_w = 171, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 491, .adv_w = 199, .box_w = 11, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 512, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 526, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 540, .adv_w = 156, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 554, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 568, .adv_w = 171, .box_w = 11, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 585, .adv_w = 242, .box_w = 15, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 608, .adv_w = 171, .box_w = 10, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 623, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 637, .adv_w = 156, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 651, .adv_w = 71, .box_w = 3, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 657, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 663, .adv_w = 71, .box_w = 3, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 669, .adv_w = 120, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 675, .adv_w = 142, .box_w = 9, .box_h = 1, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 677, .adv_w = 85, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 679, .adv_w = 142, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 688, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 699, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 706, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 717, .adv_w = 142, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 726, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 732, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 743, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 754, .adv_w = 57, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 756, .adv_w = 57, .box_w = 2, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 760, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 771, .adv_w = 57, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 773, .adv_w = 213, .box_w = 11, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 786, .adv_w = 142, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 794, .adv_w = 142, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 802, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 813, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 824, .adv_w = 85, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 829, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 836, .adv_w = 71, .box_w = 4, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 842, .adv_w = 142, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 850, .adv_w = 128, .box_w = 8, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 859, .adv_w = 185, .box_w = 12, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 873, .adv_w = 128, .box_w = 8, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 882, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 893, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 900, .adv_w = 86, .box_w = 5, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 910, .adv_w = 67, .box_w = 1, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 912, .adv_w = 86, .box_w = 5, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 922, .adv_w = 150, .box_w = 8, .box_h = 2, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 924, .adv_w = 102, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 928, .adv_w = 90, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 930, .adv_w = 256, .box_w = 16, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 954, .adv_w = 160, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 967, .adv_w = 160, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 980, .adv_w = 224, .box_w = 14, .box_h = 8, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 994, .adv_w = 224, .box_w = 14, .box_h = 8, .ofs_x = 0, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x1f72, 0xef5c, 0xefa3, 0xefa4, 0xefc7, 0xefc8
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 176, .range_length = 61385, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 7, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t sticky_sans_16 = {
#else
lv_font_t sticky_sans_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if STICKY_SANS_16*/


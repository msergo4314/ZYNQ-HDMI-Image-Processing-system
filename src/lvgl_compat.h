#ifndef LVGL_COMPAT_H
#define LVGL_COMPAT_H
#include <stddef.h>
#include <stdint.h>

// Ignore LVGL-specific attributes (they’re for section placement / alignment)
#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif
#ifndef LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_CONST
#endif

// Fake LVGL enums so the generator compiles cleanly
#define LV_IMAGE_HEADER_MAGIC 0x464C56 // 'LVF' arbitrary
#define LV_COLOR_FORMAT_RGB565 0x02
#define LV_COLOR_FORMAT_XRGB8888 0x4

typedef struct {
  uint32_t bitmap_index; // Offset into the glyph_bitmap[] array
  uint16_t adv_w;        // Advance width: how far to move cursor after drawing
  uint8_t box_w;         // Glyph bitmap width in pixels
  uint8_t box_h;         // Glyph bitmap height in pixels
  int8_t ofs_x;          // X offset from cursor position
  int8_t ofs_y; // Y offset from baseline (usually negative for descenders)
} lv_font_fmt_txt_glyph_dsc_t;

// alias used by LVGL converter webpage
typedef lv_font_fmt_txt_glyph_dsc_t glyph_dsc_t;

typedef struct {
  uint32_t cf;    // color format (we assume RGB565)
  uint32_t magic; // LVGL magic header
  uint32_t w;
  uint32_t h;
} lv_image_header_t;

typedef struct {
  lv_image_header_t header;
  size_t data_size;
  const uint8_t *data;
} lv_image_dsc_t;

#endif /* LVGL_COMPAT_H */
#pragma once // optional (??)
#ifndef ZYNQ_HDMI_GRAPHICS
#define ZYNQ_HDMI_GRAPHICS
/****************************************************************************
A comprehensive library for drawing to the HDMI output the Smart Zynq SP
development board
*****************************************************************************/

#include <stdarg.h> // variable arguments for ZHDMI_printf()
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>    // printf
#include <stdlib.h>   // snprintf
#include <xaxivdma.h> // VDMA required to transfer buffer images to PL
#include <xaxivdma.h>
#include <xil_cache.h> // cache flush operations so VDMA reads correct data
#include <xil_printf.h>
#include <xil_types.h>
#include <xparameters.h> // base addresses

#include "lvgl_compat.h" // LVGL compatibility layer

/*******************************************************************************************
NOTE : "ZHDMI" is a prefix to indicate the library streams data to the HDMI port
of the SmartZynq SP ZYNQ FPGA
********************************************************************************************/

// user orientation types
typedef enum {
  ZHDMI_LANDSCAPE_ORIENTATION =
      0, // note: internal framebuffers use landscape orientation
  ZHDMI_INVERTED_LANDSCAPE_ORIENTATION,

  ZHDMI_PORTRAIT_ORIENTATION,
  ZHDMI_INVERTED_PORTRAIT_ORIENTATION,
  ZHDMI_NUM_ORIENTATIONS,
  ZHDMI_UNKNOWN_ORIENTATION = -1
} ZHDMI_ORIENTATION;

// Function return codes
typedef enum {
  ZHDMI_SUCCESS = 0,
  ZHDMI_FAILURE = -1,
  ZHDMI_ERR_NOT_INITIALIZED = -2 // if user calls functions before the init
} ZHDMI_RETURN_STATUS;

typedef struct pixel_coord {
  // maximum supported resolution is 1920 x 1080p
  u16 x;
  u16 y;
} ZHDMI_pixel_coordinate;

typedef enum {
  ZHDMI_DRAW_INCREMENTAL, // preserve the framebuffer in between frames to add
                          // individual elements
  ZHDMI_DRAW_FRAME, // rebuild the entire framebuffer after each frame with no
                    // preservation of previous frames
  ZHDMI_UNKNOWN_DRAWING_MODE = -1
} ZHDMI_DRAWING_MODE;

/// @brief Logical text alignment for strings
typedef enum {
  ZHDMI_ALIGN_LEFT,   // allign to far left
  ZHDMI_ALIGN_CENTER, // allign to the middle of the screen
  ZHDMI_ALIGN_RIGHT   // allign to far right
} ZHDMI_TEXT_ALIGNMENT;

/// @brief controls behaviour for ZHDMI_printf()
typedef enum {
  ZHDMI_PRINTF_MODE_SCROLL,    // behaves like printf(), moves to next line
  ZHDMI_PRINTF_MODE_OVERWRITE, // stays on same line unless '\n' given
  ZHDMI_PRINTF_MODE_UNKNOWN = -1
} ZHDMI_PRINTF_MODE;

/****************************************************
Use LVGL format to import fonts easily
Download fonts from a .ttf file using  https://www.dafont.com/
and convert to C arrays using https://lvgl.io/tools/fontconverter
Make sure to use the character range 32-127 for printable characters
****************************************************/

/// @brief ZHDMI_font object based on LVGL font struct
typedef struct {
  char font_name[31]; // might as well use 31 bytes due to padding
  // roughly the height from the top of ascenders (like “h”)
  // to the bottom of descenders (like “g”), measured in pixels
  uint8_t font_size;
  const uint8_t *glyph_bitmap;
  const glyph_dsc_t *glyph_descriptors;
} ZHDMI_font;

/// @brief convert an lvgl font into a ZHDMI_font
/// @param lv_struct pointer to lvgl glyph description
/// @param glyph_bitmap pointer to lvgl font bitmap
/// @param name name of font (31 chars max)
/// @param font_size size of font
/// @return
ZHDMI_font lvgl_font_to_ZHDMI(const glyph_dsc_t *lv_struct,
                              const uint8_t *glyph_bitmap, const char *name,
                              size_t font_size);

/// @brief The hardware is configured for 32 bit transfers with axi VDMA. Though
/// three
// bytes is sufficient to store the pixel information (8 bits / channel),
// padding to 4 bytes saves PL resources at minimal cost to memory utlization.
// Therefore, all pixel data is padded to a 4 byte unsigned integer (u32) with
// the leading byte being insignificant and the following bytes representing RED
// / BLUE / GREEN colour channels (respectively)
typedef u32 rgb888;

// construct rgb888 from R/G/B channels
#define RGB888(r, g, b) ((((r)&0xFF) << 16) | (((g)&0xFF) << 8) | ((b)&0xFF))
// leading byte is not used (stuff byte)

// HDMI pixel dimensions for horizontal (Landscape) orientation
// Do not modify unless exported hardware has different values
#define ZHDMI_WIDTH (uint16_t)1920  // width of display in pixels
#define ZHDMI_HEIGHT (uint16_t)1080 // height of display in pixels

#define FRAME_WIDTH ZHDMI_WIDTH
#define FRAME_HEIGHT ZHDMI_HEIGHT

#define BYTES_PER_PIXEL (XPAR_AXI_VDMA_0_ADDRWIDTH / 8)
#define VDMA_FRAME_SIZE FRAME_WIDTH *FRAME_HEIGHT
#define VDMA_FRAME_SIZE_BYTES FRAME_WIDTH *FRAME_HEIGHT *BYTES_PER_PIXEL
#define NUM_FRAME_BUFFERS XPAR_AXI_VDMA_0_NUM_FSTORES

/*******************************************************************************
Steps for displaying any image:

1 - downsacle the resolution if needed (if any dimension exceeds 1920x1080)
with: https://www.imageresizer.work/resize-image-in-pixel or:
    https://image.online-convert.com/convert-to-bmp for BMP images

2 - Use the LVGL converter to convert the image to C array which can
    be copied into a header directly. This will convert any compatible
    image format (jpeg, png, svg, etc.) into a single array of bytes.
    Be sure to select the option for RGBX888 encoding since one pixel
    must be 4 bytes. No casting is necessary. Use:
https://lvgl.io/tools/imageconverter

*******************************************************************************/

/// @brief Struct for storing raw image data
typedef struct {
  uint16_t width;
  uint16_t height;
  size_t data_size;
  const rgb888 *map;
  uint16_t offset_x; // where to start drawing the image x (relative to left)
  uint16_t offset_y; // where to start drawing the image y (relative to top)
} ZHDMI_image;

// function to convert lv_image_dsc_t to ZHDMI_image

/// @brief convert an lvgl image to a ZHDMI_image
/// @param lv_struct pointer to lvgl image description struct
/// @param x_off x offset in pixels -- a positive value moves the image left
/// @param y_off y offset in pixels -- a positive value moves the image up
/// @return the converted image ZHDMI_image
ZHDMI_image lvgl_image_to_ZHDMI(const lv_image_dsc_t *lv_struct, uint16_t x_off,
                                uint16_t y_off);

// marco for error checking ZHDMI functions that return @ZHDMI_RETURN_STATUS
#define ZHDMI_ERROR_CHECK(call)                                                \
  do {                                                                         \
    if ((call) != ZHDMI_SUCCESS) {                                             \
      fprintf(stderr, "ZHDMI error at %s:%d\n", __FILE__, __LINE__);           \
      abort();                                                                 \
    }                                                                          \
  } while (0)

// RGB888 color definitions -- add more as needed
#define WHITE 0xFFFFFF
#define SILVER 0xC0C0C0
#define GRAY 0x808080
#define BLACK 0x000000
#define RED 0xFF0000
#define MAROON 0x800000
#define YELLOW 0xFFFF00
#define OLIVE 0x808000
#define LIME 0x00FF00
#define GREEN 0x008000
#define AQUA 0x00FFFF
#define TEAL 0x008080
#define BLUE 0x0000FF
#define NAVY 0x000080
#define MAGENTA 0xFF00FF
#define PURPLE 0x800080
// extra colours
#define ORANGE 0xFFA500
#define BROWN 0xA52A2A
#define PINK 0xFFC0CB
#define GOLD 0xFFD700
#define CORAL 0xFF7F50
#define SALMON 0xFA8072
#define CRIMSON 0xDC143C
#define VIOLET 0xEE82EE
#define INDIGO 0x4B0082
#define CYAN 0x00FFFF
#define SKY_BLUE 0x87CEEB
#define LIGHT_BLUE 0xADD8E6
#define DARK_RED 0x8B0000
#define DARK_GREEN 0x006400
#define DARK_BLUE 0x00008B
#define LIGHT_GREEN 0x90EE90
#define LIGHT_YELLOW 0xFFFFE0
#define LIGHT_GRAY 0xD3D3D3
#define BEIGE 0xF5F5DC
#define TAN 0xD2B48C
#define KHAKI 0xF0E68C
#define CHOCOLATE 0xD2691E
#define TOMATO 0xFF6347
#define PLUM 0xDDA0DD
#define TURQUOISE 0x40E0D0
#define MINT 0x98FF98

/// @brief modify a ZHDMI_pixel_coordinate by setting x and y
/// @param coordinate pointer to the coordinate to modify
/// @param new_x the new desired x value
/// @param new_y the new desired y value
void ZHDMI_change_pixel_coordinate(ZHDMI_pixel_coordinate *coordinate,
                                   uint16_t new_x, uint16_t new_y);
/// @brief create a ZHDMI_pixel_coordinate object from an x-y pair
/// @param x x value of coordinate
/// @param y y value of coordinate
/// @return the ZHDMI_pixel_coordinate created
ZHDMI_pixel_coordinate ZHDMI_create_coordinate(uint16_t x, uint16_t y);

/// @brief construct an rgb888 value from the individual channels
/// @param red the value of the red channel (0-255)
/// @param green the value of the green channel (0-255)
/// @param blue the value of the blue channel (0-255)
/// @return the colour requested as rgb888 (uint32_t)
rgb888 ZHDMI_construct_rgb888(uint8_t red, uint8_t green, uint8_t blue);

/// @brief initialize the HDMI display with parameters. Note that printf_mode
/// defaults to ZHDMI_PRINTF_MODE_SCROLL and must be changed manually
/// @param drawing_mode drawing mode selected by user. Can be either incremental
/// or full frames depending on the application
/// @param desired_orientation the logical orientation of the screen (landscape,
/// portrait, etc.)
/// @param background_colour the default background colour that should be drawn
/// when calling the draw_background function
/// @return success status
ZHDMI_RETURN_STATUS ZHDMI_init(ZHDMI_DRAWING_MODE drawing_mode,
                               ZHDMI_ORIENTATION desired_orientation,
                               rgb888 background_colour);

/// @brief return the current logical orientation of the screen
/// @return the current orientation as ZHDMI_ORIENTATION
ZHDMI_ORIENTATION ZHDMI_get_orientation(void);

/// @brief set an individual pixel to a specified colour using a coordinate
/// object. Note that performance can be poor due to checks and caching
/// behaviour
/// @param coordinate the coordiante to change
/// @param colour the colour to set the pixel to
/// @return success code
ZHDMI_RETURN_STATUS ZHDMI_set_pixel(ZHDMI_pixel_coordinate coordinate,
                                    rgb888 colour);
/// @brief set an individual pixel to a specified colour based on x/y location.
/// Note that performance can be poor due to checks and caching behaviour
/// @param x the x value of the coordinate to change
/// @param y the y value of the coordinate to change
/// @param colour the colour to set the pixel to
/// @return success code
ZHDMI_RETURN_STATUS ZHDMI_set_pixel_xy(uint16_t x, uint16_t y, rgb888 colour);

/// @brief set the logical orientation of the screen. All coordinates provided
/// by the user will be relative to this orientation
/// @param desired_orientation the logical orientation desired
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_set_orientation(ZHDMI_ORIENTATION desired_orientation);

/// @brief set the background colour used for ZHDMI_draw_background()
/// @param background_colour background colour as rgb888
void ZHDMI_set_background_colour(rgb888 background_colour);

/// @brief set the drawing mode of the display. Can be either incremental or
/// full frame.
///        Incremental preserves data between refreshes, while full frame
///        expects the user to reconstruct the entire frame between refresh
///        calls
/// @param drawing_mode the drawing mode to set
void ZHDMI_set_drawing_mode(ZHDMI_DRAWING_MODE drawing_mode);

/// @brief get the currentt drawing mode of the display
/// @return the current drawing mode
ZHDMI_DRAWING_MODE ZHDMI_get_drawing_mode(void);

/// @brief sets the internal framebuffer to be all black pixels (0x000000).
/// Optimized with memset operations, so more efficient than calling
/// ZHDMI_draw_background() if the background is black
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_clear(void);

/// @brief push any changes made since the last call of ZHDMI_refresh_display()
/// according to the current drawing mode. Note that incremental mode is
/// generally slower due to an additional memcpy() operation (~30 ms)
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_refresh_display(void);

/// @brief check that a coordinate is valid (can fit on the screen)
/// @param coordinate coordinate to check validity of
/// @return error code indicating if the pixel is valid (0 = valid)
ZHDMI_RETURN_STATUS
ZHDMI_verify_coordinate_is_valid(ZHDMI_pixel_coordinate coordinate);

/// @brief check that an x-y pair is valid (can fit on the screen)
/// @param x x value of the pixel coordinate
/// @param y y value of the pixel coordinate
/// @return error code indicating if the pixel is valid (0 = valid)
ZHDMI_RETURN_STATUS ZHDMI_verify_coordinate_is_valid_xy(uint16_t x, uint16_t y);

/// @brief draw a straight 1 pixel wide line between two coordinates
/// @param p1 the first coodrinate to draw from
/// @param p2 the second coodrinate to draw from
/// @param colour the colour of the line
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_line(ZHDMI_pixel_coordinate p1,
                                    ZHDMI_pixel_coordinate p2, rgb888 colour);

/// @brief draw a 1 pixel wide line between two pairs of points
/// @param x1 x value of the first coodrinate
/// @param y1 y value of the first coodrinate
/// @param x2 x value of the second coodrinate
/// @param y2 y value of the second coodrinate
/// @param colour the colour of the line
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_line_xy(uint16_t x1, uint16_t y1, uint16_t x2,
                                       uint16_t y2, rgb888 colour);
/// @brief draw a horizontal line 1 pixel wide
/// @param y y value of the horizontal line
/// @param x1 x value of the first pixel coordinate
/// @param x2 x value of the second pixel coordinate
/// @param colour desired colour of the line
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_hline(uint16_t y, uint16_t x1, uint16_t x2,
                                     rgb888 colour);

/// @brief draw a vertical line 1 pixel wide
/// @param x x value of the vertical line
/// @param y1 y value of the first pixel coordinate
/// @param y2 y value of the second pixel coordinate
/// @param colour desired colour of the line
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_vline(uint16_t x, uint16_t y1, uint16_t y2,
                                     rgb888 colour);
/// @brief draw a rectangle without the inside being filled in
/// @param origin top left corner of the rectangle
/// @param width_px width of rectangle in pixels
/// @param height_px height of rectangle in pixels
/// @param border_thickness_px thickness of the border in pixels
/// @param border_colour colour of the rectangle border
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_unfilled_rectangle(ZHDMI_pixel_coordinate origin, uint16_t width_px,
                              uint16_t height_px, uint16_t border_thickness_px,
                              rgb888 border_colour);

/// @brief draw a rectangle without the inside being filled in
/// @param origin_x x value of the top left corner of the rectangle
/// @param origin_y y value of the top left corner of the rectangle
/// @param width_px width of rectangle in pixels
/// @param height_px height of rectangle in pixels
/// @param border_thickness_px thickness of the border in pixels
/// @param border_colour colour of the rectangle border
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_rectangle_xy(
    uint16_t origin_x, uint16_t origin_y, uint16_t width_px, uint16_t height_px,
    uint16_t border_thickness_px, rgb888 border_colour);

/// @brief draw a rectangle with the inside filled in
/// @param origin top left corner of the rectangle
/// @param width_px width of rectangle in pixels
/// @param height_px height of rectangle in pixels
/// @param border_thickness_px thickness of the border in pixels
/// @param border_colour colour of the rectangle border
/// @param fill_colour colour to fill the inside of the rectangle
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_rectangle(ZHDMI_pixel_coordinate origin, uint16_t width_px,
                            uint16_t height_px, uint16_t border_thickness_px,
                            rgb888 border_colour, rgb888 fill_colour);
/// @brief draw a rectangle with the inside filled in
/// @param origin_x x value of the top left corner of the rectangle
/// @param origin_y y value of the top left corner of the rectangle
/// @param width_px width of rectangle in pixels
/// @param height_px height of rectangle in pixels
/// @param border_thickness_px thickness of the border in pixels
/// @param border_colour colour of the rectangle border
/// @param fill_colour colour to fill the inside of the rectangle
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_filled_rectangle_xy(
    uint16_t origin_x, uint16_t origin_y, uint16_t width_px, uint16_t height_px,
    uint16_t border_thickness_px, rgb888 border_colour, rgb888 fill_colour);

/// @brief draw an unfilled triangle with lines 1 pixel thick using three points
/// @param p1 the first vertex of the triangle
/// @param p2 the second vertex of the triangle
/// @param p3 the third vertex of the triangle
/// @param border_colour border colour of the triangle
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_triangle(ZHDMI_pixel_coordinate p1,
                                                 ZHDMI_pixel_coordinate p2,
                                                 ZHDMI_pixel_coordinate p3,
                                                 rgb888 border_colour);
/// @brief draw an unfilled triangle with lines 1 pixel thick using three x-y
/// pairs
/// @param p1x the x value of the first vertex of the triangle
/// @param p1y the y value of the first vertex of the triangle
/// @param p2x the x value of the second vertex of the triangle
/// @param p2y the y value of the second vertex of the triangle
/// @param p3x the x value of the third vertex of the triangle
/// @param p3y the y value of the third vertex of the triangle
/// @param border_colour border colour of the triangle
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_triangle_xy(uint16_t p1x, uint16_t p1y,
                                                    uint16_t p2x, uint16_t p2y,
                                                    uint16_t p3x, uint16_t p3y,
                                                    rgb888 border_colour);

/// @brief draw a filled triangle with a border 1 pixel wide using three pixel
/// coordinates
/// @param p1 the first vertex of the triangle
/// @param p2 the second vertex of the triangle
/// @param p3 the third vertex of the triangle
/// @param border_colour border colour of the triangle
/// @param fill_colour the colour to fill the inside of the triangle with
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_triangle(ZHDMI_pixel_coordinate p1, ZHDMI_pixel_coordinate p2,
                           ZHDMI_pixel_coordinate p3, rgb888 border_colour,
                           rgb888 fill_colour);

/// @brief draw a filled triangle with a border 1 pixel wide using three pixel
/// coordinates
/// @param p1x the x value of the first vertex of the triangle
/// @param p1y the y value of the first vertex of the triangle
/// @param p2x the x value of the second vertex of the triangle
/// @param p2y the y value of the second vertex of the triangle
/// @param p3x the x value of the third vertex of the triangle
/// @param p3y the y value of the third vertex of the triangle
/// @param border_colour border colour of the triangle
/// @param fill_colour the colour to fill the inside of the triangle with
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_filled_triangle_xy(uint16_t p1x, uint16_t p1y,
                                                  uint16_t p2x, uint16_t p2y,
                                                  uint16_t p3x, uint16_t p3y,
                                                  rgb888 border_colour,
                                                  rgb888 fill_colour);

/// @brief draw an unfilled circle with a border thickness of 1
/// @param origin the origin of the circle
/// @param radius_px the radius of the circle, in pixels
/// @param circle_colour the colour of the circle border
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_circle(ZHDMI_pixel_coordinate origin,
                                               uint16_t radius_px,
                                               rgb888 circle_colour);

/// @brief draw an unfilled circle with a border thickness of 1
/// @param origin_x the x value of the origin of the circle
/// @param origin_y the y value of the origin of the circle
/// @param radius_px the radius of the circle, in pixels
/// @param circle_colour the colour of the circle border
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_circle_xy(uint16_t origin_x,
                                                  uint16_t origin_y,
                                                  uint16_t radius_px,
                                                  rgb888 circle_colour);

/// @brief draw an circle with a border thickness of 1 and a filled interior
/// @param origin the origin of the circle
/// @param radius_px the radius of the circle, in pixels
/// @param border_colour the colour of the circle border
/// @param fill_colour colour of the interior of the circle
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_filled_circle(ZHDMI_pixel_coordinate origin,
                                             uint16_t radius_px,
                                             rgb888 border_colour,
                                             rgb888 fill_colour);

/// @brief draw an circle with a border thickness of 1 and a filled interior
/// @param origin_x the x value of the origin of the circle
/// @param origin_y the y value of the origin of the circle
/// @param radius_px the radius of the circle, in pixels
/// @param border_colour the colour of the circle border
/// @param fill_colour colour of the interior of the circle
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_circle_xy(uint16_t origin_x, uint16_t origin_y,
                            uint16_t radius_px, rgb888 border_colour,
                            rgb888 fill_colour);

/// @brief draw a character at a position on the screen. Should not be used for
/// strings
/// @param character the ascii character to draw (must be in range 32-127)
/// @param base_x the x value of the bottom left of the character
/// @param base_y the y value of the bottom left of the character
/// @param colour the colour of the character
/// @param f pointer to the font used to draw the character
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_char_xy(char character, uint16_t base_x,
                                       uint16_t base_y, rgb888 colour,
                                       const ZHDMI_font *f);

/// @brief draw a character at a position on the screen with a solid background.
/// Should not be used for strings
/// @param character the ascii character to draw (must be in range 32-127)
/// @param base the coordinate value of the bottom left of the character
/// @param colour the colour of the character
/// @param background_colour the background colour of the character
/// @param f pointer to the font used to draw the character
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_char_on_background(char character, ZHDMI_pixel_coordinate base,
                              rgb888 colour, rgb888 background_colour,
                              const ZHDMI_font *f);

/// @brief draw a character at a position on the screen with a solid background.
/// Should not be used for strings
/// @param character the ascii character to draw (must be in range 32-127)
/// @param base_x the x value of the bottom left of the character
/// @param base_y the y value of the bottom left of the character
/// @param colour the colour of the character
/// @param background_colour the background colour of the character
/// @param f pointer to the font used to draw the character
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_draw_char_on_background_xy(char character, uint16_t base_x,
                                 uint16_t base_y, rgb888 colour,
                                 rgb888 background_colour, const ZHDMI_font *f);

/// @brief print a string at a specified location on the screen
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base the bottom left of the first character in the string as a
/// coordinate
/// @param colour the colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_print_string(const char *string,
                                       ZHDMI_pixel_coordinate base,
                                       rgb888 colour, const ZHDMI_font *f);

/// @brief print a string at a specified location on the screen
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_x the x value of the bottom left of the first character in the
/// string
/// @param base_y the y value of the bottom left of the first character in the
/// string
/// @param colour the colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_print_string_xy(const char *string, uint16_t base_x,
                                          uint16_t base_y, rgb888 colour,
                                          const ZHDMI_font *f);

/// @brief print a string at a specified location on the screen with a solid
/// background
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base the bottom left of the first character in the string as a
/// coordinate
/// @param colour the colour of the text
/// @param background_colour the background colour of the string
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_print_string_on_background(const char *string,
                                 ZHDMI_pixel_coordinate base, rgb888 colour,
                                 rgb888 background_colour, const ZHDMI_font *f);

/// @brief print a string at a specified location on the screen with a solid
/// background
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_x the x value of the bottom left of the first character in the
/// string
/// @param base_y the y value of the bottom left of the first character in the
/// string
/// @param colour the colour of the text
/// @param background_colour the background colour of the string
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_print_string_on_background_xy(const char *string, uint16_t base_x,
                                    uint16_t base_y, rgb888 colour,
                                    rgb888 background_colour,
                                    const ZHDMI_font *f);

/// @brief draw an image on the screen at a specified location
/// @param image_origin the top left corner of where the image should be placed
/// @param image the image to draw
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_image(ZHDMI_pixel_coordinate image_origin,
                                     const ZHDMI_image *image);

/// @brief print a string aligned to a logical position (left, center, right,
/// etc.)
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_y the y value of the bottom of the first character in the string
/// @param alignment the logical alignment of the string to print
/// @param colour the colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_print_aligned_string(const char *string, uint16_t base_y,
                           ZHDMI_TEXT_ALIGNMENT alignment, rgb888 colour,
                           const ZHDMI_font *f);

/// @brief print a string aligned to a logical position (left, center, right,
/// etc.) with a solid background
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_y the y value of the bottom left of the first character in the
/// string
/// @param alignment the logical alignment of the string to print
/// @param colour the colour of the text
/// @param background_colour the bacground colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_print_aligned_string_on_background(const char *string, uint16_t base_y,
                                         ZHDMI_TEXT_ALIGNMENT alignment,
                                         rgb888 colour,
                                         rgb888 background_colour,
                                         const ZHDMI_font *f);

/// @brief print a string that wraps around to the next "line" below if it would
/// be rendered beyond the right margin
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_x the x value of the bottom left of the first character in the
/// string
/// @param base_y the y value of the bottom left of the first character in the
/// string
/// @param left_margin the offset in pixels of wrapped text from the far left of
/// the screen
/// @param right_margin the offset in pixels of wrapped text from the far right
/// of the screen
/// @param colour the colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_print_wrapped_string_xy(
    const char *string, uint16_t base_x, uint16_t base_y, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, const ZHDMI_font *f);

/// @brief print a string that wraps around to the next "line" below if it would
/// be rendered beyond the right margin
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base the value of the bottom left of the first character in the
/// string as a coordinate
/// @param left_margin the offset in pixels of wrapped text from the far left of
/// the screen
/// @param right_margin the offset in pixels of wrapped text from the far right
/// of the screen
/// @param colour the colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS
ZHDMI_print_wrapped_string(const char *string, ZHDMI_pixel_coordinate base,
                           uint16_t left_margin, uint16_t right_margin,
                           rgb888 colour, const ZHDMI_font *f);

/// @brief print a string on a solid background that wraps around to the next
/// "line" below if it would be rendered beyond the right margin
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base the value of the bottom left of the first character in the
/// string as a coordinate
/// @param left_margin the offset in pixels of wrapped text from the far left of
/// the screen
/// @param right_margin the offset in pixels of wrapped text from the far right
/// of the screen
/// @param colour the colour of the text
/// @param background_colour the bacground colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_print_wrapped_string_on_background(
    const char *string, ZHDMI_pixel_coordinate base, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, rgb888 background_colour,
    const ZHDMI_font *f);

/// @brief print a string on a solid background that wraps around to the next
/// "line" below if it would be rendered beyond the right margin
/// @param string the string to print. All characters present should be in the
/// range (32-127)
/// @param base_x the x value of the bottom left of the first character in the
/// string
/// @param base_y the y value of the bottom left of the first character in the
/// string
/// @param left_margin the offset in pixels of wrapped text from the far left of
/// the screen
/// @param right_margin the offset in pixels of wrapped text from the far right
/// of the screen
/// @param colour the colour of the text
/// @param background_colour the bacground colour of the text
/// @param f pointer to the font used to draw the string
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_print_wrapped_string_on_background_xy(
    const char *string, uint16_t base_x, uint16_t base_y, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, rgb888 background_colour,
    const ZHDMI_font *f);

/// @brief draws the current background colour into the framebuffer and sets the
/// ZHDMI_printf cursor to the top left of the screen
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_draw_background(void);

/// @brief print a string on the screen using a predefined font in a manner
/// similar to printf(). Functionality depends on the current PRINTF_MODE value
/// @param format formatted string with format specifiers if desired
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_printf(const char *format, ...);

/// @brief set the current printf mode value depending on desired ZHDMI_printf()
/// behaviour
/// @param mode the mode to set
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_set_printf_mode(ZHDMI_PRINTF_MODE mode);

/// @brief set the internal starting position of the ZHDMI_printf cursor
/// @param coord the coordinate corresponding to the bottom left of where the
/// first character printed should go
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_set_printf_cursor(ZHDMI_pixel_coordinate coord);

/// @brief set the internal starting position of the ZHDMI_printf cursor
/// @param cursor_x the x value of the coordinate corresponding to the bottom
/// left of where the first character printed should go
/// @param cursor_y the y value of the coordinate corresponding to the bottom
/// left of where the first character printed should go
/// @return error code
ZHDMI_RETURN_STATUS ZHDMI_set_printf_cursor_xy(uint16_t cursor_x,
                                               uint16_t cursor_y);
/// @brief get the current printf mode of the API
/// @return the printf mode currently selected
ZHDMI_PRINTF_MODE ZHDMI_get_printf_mode(void);
// print to both stdout and the HDMI

/// @brief print a string to both the screen buffer and serial output
/// @param fmt formatted string with format specifiers if desired
void print_output(const char *fmt, ...);

/*
read a BMP file and return a ZHDMI_image with the relevant data
*/
// ZHDMI_image ZHDMI_read_BMP(const u8 *BMP_data, size_t BMP_data_length,
//                            u8 *map_destination_arr,
//                            size_t map_destination_size);

// font used by ZHDMI_printf() should always be included - uses ~1.6 Kb of RAM

// the font is defined in the .c file
extern const ZHDMI_font printf_font;

static LV_ATTRIBUTE_LARGE_CONST const uint8_t printf_bmp[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xfc, 0x80,

    /* U+0022 "\"" */
    0x99, 0x99,

    /* U+0023 "#" */
    0x24, 0x48, 0x93, 0xf2, 0x89, 0x3f, 0xa4, 0x48,

    /* U+0024 "$" */
    0x75, 0x69, 0xc7, 0x16, 0xb5, 0x71, 0x0,

    /* U+0025 "%" */
    0xe5, 0x4a, 0xa5, 0x8f, 0x5, 0xca, 0xa5, 0x4e,

    /* U+0026 "&" */
    0x31, 0x24, 0x9c, 0x66, 0x99, 0xa2, 0x74,

    /* U+0027 "'" */
    0xf0,

    /* U+0028 "(" */
    0x29, 0x49, 0x24, 0x89, 0x10,

    /* U+0029 ")" */
    0x89, 0x12, 0x49, 0x29, 0x40,

    /* U+002A "*" */
    0x25, 0x5c, 0xa1, 0x0,

    /* U+002B "+" */
    0x21, 0x3e, 0x42, 0x10,

    /* U+002C "," */
    0xea,

    /* U+002D "-" */
    0xe0,

    /* U+002E "." */
    0xc0,

    /* U+002F "/" */
    0x8, 0x21, 0x4, 0x20, 0x84, 0x30, 0x80,

    /* U+0030 "0" */
    0x79, 0x28, 0x61, 0xa6, 0x18, 0x52, 0x78,

    /* U+0031 "1" */
    0x10, 0xcd, 0x4, 0x10, 0x41, 0x4, 0xfc,

    /* U+0032 "2" */
    0x39, 0x14, 0x41, 0x8, 0x42, 0x10, 0xfc,

    /* U+0033 "3" */
    0x74, 0x62, 0x13, 0xe, 0x31, 0x70,

    /* U+0034 "4" */
    0x8, 0x62, 0x8a, 0x4a, 0x2f, 0xc2, 0x8,

    /* U+0035 "5" */
    0xfc, 0x21, 0xe9, 0x84, 0x33, 0x70,

    /* U+0036 "6" */
    0x72, 0x61, 0x6c, 0xc6, 0x39, 0x70,

    /* U+0037 "7" */
    0xf8, 0x44, 0x62, 0x11, 0x8, 0x40,

    /* U+0038 "8" */
    0x74, 0x63, 0x17, 0x46, 0x31, 0x70,

    /* U+0039 "9" */
    0x74, 0xe3, 0x18, 0xbc, 0x32, 0x70,

    /* U+003A ":" */
    0xc6,

    /* U+003B ";" */
    0x50, 0x15, 0xa0,

    /* U+003C "<" */
    0x0, 0x37, 0x20, 0xe0, 0x60, 0x40,

    /* U+003D "=" */
    0xfc, 0x0, 0x3f,

    /* U+003E ">" */
    0x3, 0x83, 0x81, 0x1d, 0x88, 0x0,

    /* U+003F "?" */
    0x7b, 0x18, 0x41, 0x8, 0x42, 0x0, 0x20,

    /* U+0040 "@" */
    0x38, 0x89, 0xed, 0x5c, 0xb9, 0x72, 0xef, 0xfc, 0x88, 0xe0,

    /* U+0041 "A" */
    0x10, 0x70, 0xa1, 0x46, 0xc8, 0x9f, 0x63, 0x82,

    /* U+0042 "B" */
    0xf2, 0x28, 0xa2, 0xf2, 0x38, 0x61, 0xf8,

    /* U+0043 "C" */
    0x79, 0x28, 0x20, 0x82, 0x8, 0x52, 0x78,

    /* U+0044 "D" */
    0xf2, 0x28, 0x61, 0x86, 0x18, 0x62, 0xf0,

    /* U+0045 "E" */
    0xfa, 0x8, 0x20, 0xfa, 0x8, 0x20, 0xfc,

    /* U+0046 "F" */
    0xfc, 0x21, 0x8, 0x7e, 0x10, 0x80,

    /* U+0047 "G" */
    0x79, 0x38, 0x20, 0x9e, 0x18, 0x51, 0x38,

    /* U+0048 "H" */
    0x8c, 0x63, 0x1f, 0xc6, 0x31, 0x88,

    /* U+0049 "I" */
    0xf9, 0x8, 0x42, 0x10, 0x84, 0xf8,

    /* U+004A "J" */
    0x38, 0x42, 0x10, 0x84, 0x29, 0x70,

    /* U+004B "K" */
    0x8a, 0x6b, 0x28, 0xe2, 0xc9, 0x22, 0x8c,

    /* U+004C "L" */
    0x84, 0x21, 0x8, 0x42, 0x10, 0xf8,

    /* U+004D "M" */
    0xcf, 0x3c, 0xed, 0xb6, 0xd8, 0x61, 0x84,

    /* U+004E "N" */
    0x8e, 0x73, 0x5a, 0xd6, 0x73, 0x98,

    /* U+004F "O" */
    0x79, 0x28, 0x61, 0x86, 0x18, 0x52, 0x78,

    /* U+0050 "P" */
    0xfa, 0x38, 0x61, 0x8f, 0xe8, 0x20, 0x80,

    /* U+0051 "Q" */
    0x79, 0x28, 0x61, 0x86, 0x18, 0x73, 0x78, 0x41, 0x3,

    /* U+0052 "R" */
    0xfa, 0x18, 0x61, 0xfa, 0x48, 0xa2, 0x84,

    /* U+0053 "S" */
    0x7a, 0x38, 0x30, 0x38, 0x18, 0x61, 0x78,

    /* U+0054 "T" */
    0xfe, 0x20, 0x40, 0x81, 0x2, 0x4, 0x8, 0x10,

    /* U+0055 "U" */
    0x8c, 0x63, 0x18, 0xc6, 0x31, 0x70,

    /* U+0056 "V" */
    0x82, 0x8d, 0x12, 0x22, 0x45, 0xa, 0xc, 0x10,

    /* U+0057 "W" */
    0x83, 0x6, 0xe, 0x95, 0xad, 0x9b, 0x36, 0x64,

    /* U+0058 "X" */
    0x44, 0xc8, 0xa0, 0xc1, 0x5, 0xb, 0x22, 0x42,

    /* U+0059 "Y" */
    0xc6, 0x88, 0xa1, 0x41, 0x2, 0x4, 0x8, 0x10,

    /* U+005A "Z" */
    0xfc, 0x30, 0x84, 0x30, 0x84, 0x30, 0xfc,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x92, 0x70,

    /* U+005C "\\" */
    0x82, 0x4, 0x8, 0x20, 0x41, 0x2, 0x8,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x24, 0xf0,

    /* U+005E "^" */
    0x22, 0x94, 0xa8, 0xc4,

    /* U+005F "_" */
    0xfe,

    /* U+0060 "`" */
    0xc8,

    /* U+0061 "a" */
    0x73, 0x20, 0x9e, 0x8a, 0x6e, 0xc0,

    /* U+0062 "b" */
    0x84, 0x2d, 0x98, 0xc6, 0x31, 0xf0,

    /* U+0063 "c" */
    0x76, 0x61, 0x8, 0x65, 0xc0,

    /* U+0064 "d" */
    0x8, 0x5b, 0x38, 0xc6, 0x33, 0x68,

    /* U+0065 "e" */
    0x73, 0x28, 0xbe, 0x83, 0x27, 0x80,

    /* U+0066 "f" */
    0x3c, 0x8f, 0xc8, 0x20, 0x82, 0x8, 0x20,

    /* U+0067 "g" */
    0x6c, 0xe3, 0x18, 0xcd, 0xa1, 0xcb, 0x80,

    /* U+0068 "h" */
    0x84, 0x2d, 0x98, 0xc6, 0x31, 0x88,

    /* U+0069 "i" */
    0x20, 0x38, 0x42, 0x10, 0x84, 0xf8,

    /* U+006A "j" */
    0x10, 0x71, 0x11, 0x11, 0x11, 0x1e,

    /* U+006B "k" */
    0x84, 0x27, 0x2a, 0x72, 0x92, 0x88,

    /* U+006C "l" */
    0xe1, 0x8, 0x42, 0x10, 0x84, 0xf8,

    /* U+006D "m" */
    0xef, 0x26, 0x4c, 0x99, 0x32, 0x64, 0x80,

    /* U+006E "n" */
    0xb6, 0x63, 0x18, 0xc6, 0x20,

    /* U+006F "o" */
    0x7b, 0x38, 0x61, 0x87, 0x37, 0x80,

    /* U+0070 "p" */
    0xb6, 0x63, 0x18, 0xc7, 0xd0, 0x84, 0x0,

    /* U+0071 "q" */
    0x6c, 0xe3, 0x18, 0xcd, 0xa1, 0x8, 0x40,

    /* U+0072 "r" */
    0xbe, 0x21, 0x8, 0x42, 0x0,

    /* U+0073 "s" */
    0x74, 0x60, 0xe0, 0xc7, 0xc0,

    /* U+0074 "t" */
    0x42, 0x3e, 0x84, 0x21, 0x8, 0x78,

    /* U+0075 "u" */
    0x8c, 0x63, 0x18, 0xcd, 0xa0,

    /* U+0076 "v" */
    0x46, 0x89, 0x11, 0x62, 0x85, 0x4, 0x0,

    /* U+0077 "w" */
    0x83, 0x5, 0x4b, 0x56, 0xcd, 0x99, 0x0,

    /* U+0078 "x" */
    0x89, 0x45, 0x8, 0x51, 0x68, 0x80,

    /* U+0079 "y" */
    0x46, 0x89, 0x11, 0x62, 0x83, 0x4, 0x8, 0x30, 0xc0,

    /* U+007A "z" */
    0xf8, 0xc4, 0x44, 0x63, 0xe0,

    /* U+007B "{" */
    0x39, 0x8, 0x42, 0x60, 0x84, 0x21, 0x8, 0x30,

    /* U+007C "|" */
    0xff, 0xf0,

    /* U+007D "}" */
    0xe1, 0x8, 0x42, 0xc, 0x84, 0x21, 0x9, 0x80,

    /* U+007E "~" */
    0xe0, 0x70};

static const lv_font_fmt_txt_glyph_dsc_t printf_dsc[] = {
    {.bitmap_index = 0,
     .adv_w = 0,
     .box_w = 0,
     .box_h = 0,
     .ofs_x = 0,
     .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 1,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 1,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 9,
     .ofs_x = 3,
     .ofs_y = 0},
    {.bitmap_index = 3,
     .adv_w = 115,
     .box_w = 4,
     .box_h = 4,
     .ofs_x = 2,
     .ofs_y = 5},
    {.bitmap_index = 5,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 13,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 10,
     .ofs_x = 1,
     .ofs_y = -1},
    {.bitmap_index = 20,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 28,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 35,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 4,
     .ofs_x = 3,
     .ofs_y = 5},
    {.bitmap_index = 36,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 12,
     .ofs_x = 2,
     .ofs_y = -3},
    {.bitmap_index = 41,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 12,
     .ofs_x = 2,
     .ofs_y = -3},
    {.bitmap_index = 46,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 5,
     .ofs_x = 1,
     .ofs_y = 4},
    {.bitmap_index = 50,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 6,
     .ofs_x = 1,
     .ofs_y = 1},
    {.bitmap_index = 54,
     .adv_w = 115,
     .box_w = 2,
     .box_h = 4,
     .ofs_x = 2,
     .ofs_y = -2},
    {.bitmap_index = 55,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 1,
     .ofs_x = 2,
     .ofs_y = 3},
    {.bitmap_index = 56,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 2,
     .ofs_x = 3,
     .ofs_y = 0},
    {.bitmap_index = 57,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 64,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 71,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 78,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 85,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 91,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 98,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 104,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 110,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 116,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 122,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 128,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 7,
     .ofs_x = 3,
     .ofs_y = 0},
    {.bitmap_index = 129,
     .adv_w = 115,
     .box_w = 2,
     .box_h = 10,
     .ofs_x = 2,
     .ofs_y = -3},
    {.bitmap_index = 132,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 1},
    {.bitmap_index = 138,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 4,
     .ofs_x = 1,
     .ofs_y = 2},
    {.bitmap_index = 141,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 1},
    {.bitmap_index = 147,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 154,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 11,
     .ofs_x = 1,
     .ofs_y = -2},
    {.bitmap_index = 164,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 172,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 179,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 186,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 193,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 200,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 206,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 213,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 219,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 225,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 231,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 238,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 244,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 251,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 257,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 264,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 271,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 12,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 280,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 287,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 294,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 302,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 308,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 316,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 324,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 332,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 340,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 347,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 12,
     .ofs_x = 2,
     .ofs_y = -3},
    {.bitmap_index = 352,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 359,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 12,
     .ofs_x = 2,
     .ofs_y = -3},
    {.bitmap_index = 364,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 6,
     .ofs_x = 1,
     .ofs_y = 3},
    {.bitmap_index = 368,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 1,
     .ofs_x = 0,
     .ofs_y = -2},
    {.bitmap_index = 369,
     .adv_w = 115,
     .box_w = 3,
     .box_h = 2,
     .ofs_x = 2,
     .ofs_y = 8},
    {.bitmap_index = 370,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 376,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 382,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 387,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 393,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 399,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 9,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 406,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 10,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 413,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 419,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 425,
     .adv_w = 115,
     .box_w = 4,
     .box_h = 12,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 431,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 437,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 443,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 450,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 455,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 461,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 10,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 468,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 10,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 475,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 2,
     .ofs_y = 0},
    {.bitmap_index = 480,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 485,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 9,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 491,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 496,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 7,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 503,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 7,
     .ofs_x = 0,
     .ofs_y = 0},
    {.bitmap_index = 510,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 516,
     .adv_w = 115,
     .box_w = 7,
     .box_h = 10,
     .ofs_x = 0,
     .ofs_y = -3},
    {.bitmap_index = 525,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 7,
     .ofs_x = 1,
     .ofs_y = 0},
    {.bitmap_index = 530,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 12,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 538,
     .adv_w = 115,
     .box_w = 1,
     .box_h = 12,
     .ofs_x = 3,
     .ofs_y = -3},
    {.bitmap_index = 540,
     .adv_w = 115,
     .box_w = 5,
     .box_h = 12,
     .ofs_x = 1,
     .ofs_y = -3},
    {.bitmap_index = 548,
     .adv_w = 115,
     .box_w = 6,
     .box_h = 2,
     .ofs_x = 1,
     .ofs_y = 3}};

#endif // ZHDMI_H

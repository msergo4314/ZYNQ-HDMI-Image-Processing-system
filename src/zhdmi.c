#include "zhdmi.h"
#include <stddef.h>
#include <stdint.h>
#include <xaxivdma.h>
#include <xstatus.h>

/*************************************************
  header implementation file for PL controlled HDMI graphics
  for the Smart Zynq SP board
**************************************************/
#define log_error_message(string)                                              \
  printf("\n%s\nFile: %s%d\n\n", (string), __FILE__, __LINE__);

/*******************************
        TYPEDEFS HERE
********************************/

/*****************************************************************
  helper functions are needed to transform relative coordinates
  to absolute coordinates for the default layout of the internal VDMA
  buffers (landscape orientation). For example, a coordiante of (0,0)
  in portrait mode is actually (0, 1079) in the landscape orientation
*******************************************************************/
typedef size_t (*pixel_transform_function)(uint16_t x, uint16_t y);

typedef enum {
  BUFFER_PARKED = 0, // unavailable to touch with CPU
  BUFFER_FREE,  // can be freely changed by CPU without impacting the screen
  BUFFER_USING, // is currently being used to store the next VDMA frame
  NUM_BUFFER_STATES,
  BUFFER_UNKNOWN_STATE = -1,
} ZHDMI_BUFFER_STATE;

typedef struct {
  uint16_t horizontal_axis_length_px, vertical_axis_length_px;
  ZHDMI_ORIENTATION orientation_type;
} ZHDMI_orientation_parameters;

// for handling negative coordinates in internal functions only
typedef struct {
  int16_t x;
  int16_t y;
} ZHDMI_internal_coordinate;

typedef struct {
  uint16_t min_x;
  uint16_t max_x;
  uint16_t min_y;
  uint16_t max_y;
  bool dirty;
} ZHDMI_dirty_region; // bounding rectangle which must contain all changed
                      // pixels since last visible frame

/*
Bitmap header located at the start of any BMP file
*/
#pragma pack(push, 1) // ensure fully packed
typedef struct {
  uint16_t signature;    // 'BM'
  uint32_t file_size;    // full file size including headers
  uint32_t reserved;     // unused
  uint32_t pixel_offset; // offset to start of px data
  // end of Bitmap file header
  // start of Bitmap info header
  uint32_t dib_header_size;  // must be 40
  int32_t width;             // in px
  int32_t height;            // in px
  uint16_t planes;           // must be 1
  uint16_t bits_per_pixel;   // 1,4,8,16,24,32
  uint32_t compression;      // should be 0
  uint32_t img_size;         // ignore if compresion is 0
  int32_t x_ppm;             // pixels/meter
  int32_t y_ppm;             // pixels/meter
  uint32_t colors_used;      // number of actually used colours
  uint32_t important_colors; // number of important colours, 0 for all
} BMPHeader;
#pragma pack(pop)

/*******************************
  STATIC GLOBAL VARIABLES HERE
********************************/

static XAxiVdma vdma_instance;

// tracks current orientation data
static ZHDMI_orientation_parameters current_orientation = {0};

static ZHDMI_PRINTF_MODE current_printf_mode = ZHDMI_PRINTF_MODE_SCROLL;

static bool ZHDMI_initialized = false; // has the user initialized yet?

// static uint16_t cached_col_start = 0xFFFF;
// static uint16_t cached_col_end = 0xFFFF;
// static uint16_t cached_row_start = 0xFFFF;
// static uint16_t cached_row_end = 0xFFFF;

// when the user draws, which regions of memory have been touched
// should be stored to optimize the cache flush before the parking switch

/*************************************************************************************
NOTE: the bounding rectangle for cache flushing is only significant in preserve
mode
*************************************************************************************/

static ZHDMI_dirty_region dirty_region = {0};

static rgb888 current_background_colour = 0;

static pixel_transform_function current_transform_fun = NULL;

// ZHDMI_printf cursor index
static uint16_t printf_y = 0, printf_x = 0;

// visible to users because of extern header declaration
const ZHDMI_font printf_font = {.font_name = "Liberation Mono",
                                .font_size = 12,
                                // bitmap and descriptors in header file
                                .glyph_bitmap = printf_bmp,
                                .glyph_descriptors = printf_dsc};

/******************************
VDMA FRAMEBUFFERS DECLARED HERE
******************************/

/************************************************
NOTE: the number of frame buffers for VDMA should be
3 since the frame switching functionality is built
a triple buffer in mind
************************************************/

// Allocate space for frame buffers in DDR (aligned to 64 bytes)
static rgb888 vdma_frame_buffers[NUM_FRAME_BUFFERS][VDMA_FRAME_SIZE]
    __attribute__((aligned(64)));

// at any point in time, one buffer is used for CPU writes, one is currently
// visible (parked), and one is free.
static ZHDMI_BUFFER_STATE current_buffer_states[NUM_BUFFER_STATES];

static ZHDMI_DRAWING_MODE current_drawing_mode = ZHDMI_UNKNOWN_DRAWING_MODE;

/*******************************
    STATIC FUNCTIONS HERE
********************************/

static void update_dirty_region(uint16_t x_min, uint16_t x_max, uint16_t y_min,
                                uint16_t y_max);

static int init_vdma(XAxiVdma *vdma_instance, UINTPTR VDMA_base_address);

static int init_vdma(XAxiVdma *vdma_instance, UINTPTR VDMA_base_address) {
  XAxiVdma_Config *Config;
  XAxiVdma_DmaSetup ReadCfg;
  int Status;
  UINTPTR BufferAddr[NUM_FRAME_BUFFERS];

  printf("--- Initializing Zynq VDMA --- \r\n");

  if (NUM_FRAME_BUFFERS != 3) {
    xil_printf("ERROR: ZHDMI expects triple buffers for hardware platform\n");
    return XST_FAILURE;
  }
  if (BYTES_PER_PIXEL != 4) {
    xil_printf(
        "ERROR: ZHDMI expects pixel data to be 32 bits long. Ensure that the "
        "axi VDMA block outputs 32 bit values in the block diagram\n");
    return XST_FAILURE;
  }

  Config = XAxiVdma_LookupConfig(VDMA_base_address);
  if (!Config) {
    xil_printf("No VDMA configuration found for base address %p\r\n",
               VDMA_base_address);
    return XST_FAILURE;
  }

  Status = XAxiVdma_CfgInitialize(vdma_instance, Config, Config->BaseAddress);
  if (Status != XST_SUCCESS) {
    xil_printf("Initialization failed %d\r\n", Status);
    return XST_FAILURE;
  }

  // Configure the MM2S (Read) channel properties
  ReadCfg.VertSizeInput = FRAME_HEIGHT;
  ReadCfg.HoriSizeInput = FRAME_WIDTH * BYTES_PER_PIXEL; // Total line bytes
  ReadCfg.Stride = FRAME_WIDTH * BYTES_PER_PIXEL; // Distance to next line start
  ReadCfg.FrameDelay = 0;                         // No frame delay
  ReadCfg.EnableCircularBuf = 1;                  // Circular ring buffer mode
  ReadCfg.EnableSync = 1;          // Enable GenLock synchronization
  ReadCfg.PointNum = 0;            // Master GenLock point
  ReadCfg.EnableFrameCounter = 0;  // Continuous operation
  ReadCfg.FixedFrameStoreAddr = 0; // Ignored in circular mode

  Status = XAxiVdma_DmaConfig(vdma_instance, XAXIVDMA_READ, &ReadCfg);
  if (Status != XST_SUCCESS) {
    xil_printf("Read channel config failed %d\r\n", Status);
    return XST_FAILURE;
  }

  // Populate frame store memory locations
  for (int i = 0; i < NUM_FRAME_BUFFERS; i++) {
    BufferAddr[i] = (UINTPTR)&vdma_frame_buffers[i];
  }

  Status = XAxiVdma_DmaSetBufferAddr(vdma_instance, XAXIVDMA_READ, BufferAddr);
  if (Status != XST_SUCCESS) {
    xil_printf("Setting buffer addresses failed %d\r\n", Status);
    return XST_FAILURE;
  }

  Status = XAxiVdma_DmaStart(vdma_instance, XAXIVDMA_READ);
  if (Status != XST_SUCCESS) {
    xil_printf("Starting read channel failed %d\r\n", Status);
    return XST_FAILURE;
  }

  // default VDMA behaviour is to park on buffer 0
  // Buffer 1 is selected for CPU writes first and
  // buffer 2 is used as a spare buffer
  current_buffer_states[0] = BUFFER_PARKED;
  current_buffer_states[1] = BUFFER_USING;
  current_buffer_states[2] = BUFFER_FREE;
  return XST_SUCCESS;
}

// function pointer options based on orientation
static size_t pixel_coordinate_to_internal_index_portrait(uint16_t x,
                                                          uint16_t y);
static size_t pixel_coordinate_to_internal_index_inverted_portrait(uint16_t x,
                                                                   uint16_t y);
static size_t pixel_coordinate_to_internal_index_landscape(uint16_t x,
                                                           uint16_t y);
static size_t pixel_coordinate_to_internal_index_inverted_landscape(uint16_t x,
                                                                    uint16_t y);

static ZHDMI_RETURN_STATUS
ZHDMI_draw_triangle_internal(ZHDMI_pixel_coordinate p1,
                             ZHDMI_pixel_coordinate p2,
                             ZHDMI_pixel_coordinate p3, rgb888 border_colour,
                             bool fill, rgb888 fill_colour);

static void
ZHDMI_draw_rectangle_xy_internal(uint16_t origin_x, uint16_t origin_y,
                                 uint16_t width_px, uint16_t height_px,
                                 uint16_t border_thickness_px, bool fill,
                                 rgb888 border_colour, rgb888 fill_colour);

static ZHDMI_RETURN_STATUS
ZHDMI_draw_circle_xy_internal(uint16_t origin_x, uint16_t origin_y,
                              uint16_t radius_px, rgb888 border_colour,
                              bool fill, rgb888 fill_colour);

static void fill_bottom_flat_triangle(ZHDMI_pixel_coordinate v1,
                                      ZHDMI_pixel_coordinate v2,
                                      ZHDMI_pixel_coordinate v3, rgb888 colour);

static void fill_top_flat_triangle(ZHDMI_pixel_coordinate v1,
                                   ZHDMI_pixel_coordinate v2,
                                   ZHDMI_pixel_coordinate v3, rgb888 colour);

static void ZHDMI_draw_hline_internal(int16_t y, int16_t x1, int16_t x2,
                                      rgb888 colour);
static void ZHDMI_draw_vline_internal(int16_t x, int16_t y1, int16_t y2,
                                      rgb888 colour);
static void ZHDMI_draw_line_xy_internal(int16_t x1, int16_t y1, int16_t x2,
                                        int16_t y2, rgb888 colour);

static inline void ZHDMI_set_pixel_xy_internal(int16_t x, int16_t y,
                                               rgb888 colour);
// static void ZHDMI_set_pixel_internal(ZHDMI_internal_coordinate p, rgb888
// colour);
static void ZHDMI_draw_line_internal(ZHDMI_internal_coordinate p1,
                                     ZHDMI_internal_coordinate p2,
                                     rgb888 colour);

static void ZHDMI_draw_char_xy_internal(char character, uint16_t base_x,
                                        uint16_t base_y, rgb888 colour,
                                        bool draw_background,
                                        rgb888 background_colour,
                                        const ZHDMI_font *f);

static void ZHDMI_print_string_xy_internal(const char *string, uint16_t base_x,
                                           uint16_t base_y, rgb888 colour,
                                           bool draw_background,
                                           rgb888 background_colour,
                                           const ZHDMI_font *f);

static void ZHDMI_print_aligned_string_internal(
    const char *string, uint16_t base_y, ZHDMI_TEXT_ALIGNMENT alignment,
    rgb888 colour, bool draw_background, rgb888 background_colour,
    const ZHDMI_font *f);

static uint16_t get_font_height(const char *string, const ZHDMI_font *f,
                                int8_t *y_offset);

/*******************************
    FUNCTION DEFINITIONS HERE
********************************/

ZHDMI_pixel_coordinate ZHDMI_create_coordinate(uint16_t x, uint16_t y) {
  return (ZHDMI_pixel_coordinate){.x = x, .y = y};
}

void ZHDMI_change_pixel_coordinate(ZHDMI_pixel_coordinate *coordinate,
                                   uint16_t new_x, uint16_t new_y) {
  if (coordinate == NULL)
    return;
  coordinate->x = new_x;
  coordinate->y = new_y;
}

rgb888 ZHDMI_construct_rgb888(uint8_t red, uint8_t green, uint8_t blue) {
  return (u32)(red << 16 | green << 8 | blue); // bits [31:24] are not used
}

ZHDMI_RETURN_STATUS
ZHDMI_set_orientation(ZHDMI_ORIENTATION desired_orientation) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (desired_orientation == current_orientation.orientation_type) {
    return ZHDMI_SUCCESS;
  }
  switch (desired_orientation) {
  case ZHDMI_PORTRAIT_ORIENTATION:
    current_orientation.horizontal_axis_length_px = ZHDMI_HEIGHT;
    current_orientation.vertical_axis_length_px = ZHDMI_WIDTH;
    current_transform_fun = pixel_coordinate_to_internal_index_portrait;
    break;
  case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
    current_orientation.horizontal_axis_length_px = ZHDMI_HEIGHT;
    current_orientation.vertical_axis_length_px = ZHDMI_WIDTH;
    current_transform_fun =
        pixel_coordinate_to_internal_index_inverted_portrait;
    break;
  case ZHDMI_LANDSCAPE_ORIENTATION:
    current_orientation.horizontal_axis_length_px = ZHDMI_WIDTH;
    current_orientation.vertical_axis_length_px = ZHDMI_HEIGHT;
    current_transform_fun = pixel_coordinate_to_internal_index_landscape;
    break;
  case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
    current_orientation.horizontal_axis_length_px = ZHDMI_WIDTH;
    current_orientation.vertical_axis_length_px = ZHDMI_HEIGHT;
    current_transform_fun =
        pixel_coordinate_to_internal_index_inverted_landscape;
    break;
  default:
    printf("ERROR: invalid orientation value\nFILE: %s\nLINE: %u\n", __FILE__,
           __LINE__);
    return ZHDMI_FAILURE;
  }
  current_orientation.orientation_type =
      desired_orientation; // update current orientation
  printf_x = 0;
  printf_y = printf_font.font_size;
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_init(ZHDMI_DRAWING_MODE drawing_mode,
                               ZHDMI_ORIENTATION desired_orientation,
                               rgb888 background_colour) {
  if (ZHDMI_initialized) {
    return ZHDMI_SUCCESS;
  }

  if (init_vdma(&vdma_instance, XPAR_AXI_VDMA_0_BASEADDR) != XST_SUCCESS) {
    return ZHDMI_ERR_NOT_INITIALIZED;
  }

  ZHDMI_initialized = true;
  // set background colour
  current_orientation.orientation_type = ZHDMI_UNKNOWN_ORIENTATION;
  ZHDMI_set_orientation(desired_orientation);

  ZHDMI_set_drawing_mode(drawing_mode);
  ZHDMI_set_background_colour(background_colour);
  ZHDMI_draw_background(); // set pixels and refresh screen

  ZHDMI_refresh_display();
  // set cache refesh bounds and update immediately
  return ZHDMI_SUCCESS;
}

static void update_dirty_region(uint16_t x_min, uint16_t x_max, uint16_t y_min,
                                uint16_t y_max) {
  if (current_drawing_mode != ZHDMI_DRAW_INCREMENTAL)
    return;

  uint16_t xs[4] = {x_min, x_max, x_min, x_max};
  uint16_t ys[4] = {y_min, y_min, y_max, y_max};

  size_t index;

  uint16_t internal_x_min = UINT16_MAX;
  uint16_t internal_x_max = 0;
  uint16_t internal_y_min = UINT16_MAX;
  uint16_t internal_y_max = 0;

  for (int i = 0; i < 4; i++) {
    index = current_transform_fun(xs[i], ys[i]);
    // convert from arbitrary rotation to default (landscape)
    uint16_t x = index % ZHDMI_WIDTH;
    uint16_t y = index / ZHDMI_WIDTH;

    if (x < internal_x_min)
      internal_x_min = x;
    if (x > internal_x_max)
      internal_x_max = x;
    if (y < internal_y_min)
      internal_y_min = y;
    if (y > internal_y_max)
      internal_y_max = y;
  }

  if (!dirty_region.dirty) {
    dirty_region.min_x = internal_x_min;
    dirty_region.max_x = internal_x_max;
    dirty_region.min_y = internal_y_min;
    dirty_region.max_y = internal_y_max;
    dirty_region.dirty = true;
    return;
  }

  if (internal_x_min < dirty_region.min_x)
    dirty_region.min_x = internal_x_min;

  if (internal_x_max > dirty_region.max_x)
    dirty_region.max_x = internal_x_max;

  if (internal_y_min < dirty_region.min_y)
    dirty_region.min_y = internal_y_min;

  if (internal_y_max > dirty_region.max_y)
    dirty_region.max_y = internal_y_max;
}

static inline void ZHDMI_set_pixel_xy_internal(int16_t x, int16_t y,
                                               rgb888 colour) {
  if (x < 0 || x >= current_orientation.horizontal_axis_length_px) {
    return;
  }
  if (y < 0 || y >= current_orientation.vertical_axis_length_px) {
    return;
  }
  size_t index = current_transform_fun(x, y);

  u32 cpu_buffer = current_buffer_states[BUFFER_USING];
  vdma_frame_buffers[cpu_buffer][index] = colour;
}

// static void ZHDMI_set_pixel_internal(ZHDMI_internal_coordinate p, rgb888
// colour) { 	return ZHDMI_set_pixel_xy_internal(p.x, p.y, colour);
// }

ZHDMI_RETURN_STATUS ZHDMI_set_pixel_xy(uint16_t x, uint16_t y, rgb888 colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }

  // inlined version of the ZHDMI_verify_coordinate_is_valid_xy() function
  if (x >= current_orientation.horizontal_axis_length_px) {
    char error_message[100];
    snprintf(error_message, sizeof(error_message) - 1,
             "Error: coordinate %d exceeds maximum allowed value of %d", x,
             current_orientation.horizontal_axis_length_px - 1);
    log_error_message(error_message);
    return ZHDMI_FAILURE;
  }
  if (y >= current_orientation.vertical_axis_length_px) {
    char error_message[100];
    snprintf(error_message, sizeof(error_message) - 1,
             "Error: y coordinate %d exceeds maximum allowed value of %d", y,
             current_orientation.vertical_axis_length_px - 1);
    log_error_message(error_message);
    return ZHDMI_FAILURE;
  }

  // convert x and y to portrait coordinates
  uint16_t converted_x, converted_y;
  switch (current_orientation.orientation_type) {
  case ZHDMI_PORTRAIT_ORIENTATION:
    // rotate landscape 90 degrees clockwise
    converted_x = y;
    converted_y = ZHDMI_HEIGHT - 1 - x;
    break;
  case ZHDMI_LANDSCAPE_ORIENTATION:
    // default monitor orientation
    converted_x = x;
    converted_y = y;
    break;
  case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
    // 90 degrees counter-clockwise relative to landscape
    converted_x = ZHDMI_WIDTH - 1 - y;
    converted_y = x;
    break;
  case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
    // 180 degrees clockwise relative to landscape
    converted_x = ZHDMI_WIDTH - 1 - x;
    converted_y = ZHDMI_HEIGHT - 1 - y;
    break;
  default:;
    char error_message[100] = "";
    snprintf(error_message, sizeof(error_message) - 1,
             "ERROR: invalid orientation type %d found",
             current_orientation.orientation_type);
    log_error_message(error_message);
    return ZHDMI_FAILURE;
  }
  size_t index = (converted_y * ZHDMI_WIDTH + converted_x);
  const u32 cpu_buffer = current_buffer_states[BUFFER_USING];
  vdma_frame_buffers[cpu_buffer][index] = colour;
  update_dirty_region(x, x, y, y);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_set_pixel(ZHDMI_pixel_coordinate coordinate,
                                    rgb888 colour) {
  return ZHDMI_set_pixel_xy(coordinate.x, coordinate.y, colour);
}

// remove me
u32 t1 = 0, t2 = 0;
#include <xscutimer.h>

static XScuTimer TimerInstance;
static inline uint32_t get_timer_value(void) {
  return XScuTimer_GetCounterValue(&TimerInstance);
}

#define TIME_SECTION(start, end, func_call)                                    \
  do {                                                                         \
    start = get_timer_value();                                                 \
    func_call;                                                                 \
    end = get_timer_value();                                                   \
  } while (0)

static void setup_timer() {
  XScuTimer_Config *config = XScuTimer_LookupConfig(XPAR_SCUTIMER_BASEADDR);
  if (!config) {
    printf("Timer config lookup failed!\n");
    return;
  }
  // Initialize timer driver
  XScuTimer_CfgInitialize(&TimerInstance, config, config->BaseAddr);

  // Disable auto reload so it runs once up to overflow
  XScuTimer_DisableAutoReload(&TimerInstance);

  // Load timer with max value (counts down)
  XScuTimer_LoadTimer(&TimerInstance, 0xFFFFFFFF);

  // Start the timer
  XScuTimer_Start(&TimerInstance);
}

ZHDMI_RETURN_STATUS ZHDMI_refresh_display(void) {

  static int timer_setup = 0;
  if (timer_setup == 0) {
    setup_timer();
  }

  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  /*
  Set the free buffer as the new using buffer and set the parked buffer to be
  the new free buffer The current buffer should become the new parked buffer
  */
  u32 using = current_buffer_states[BUFFER_USING];
  u32 free = current_buffer_states[BUFFER_FREE];
  u32 parked = current_buffer_states[BUFFER_PARKED];

  // ensure switch from last refresh call has happened
  while (XAxiVdma_CurrFrameStore(&vdma_instance, XAXIVDMA_READ) != parked)
    ;

  if (current_drawing_mode == ZHDMI_DRAW_INCREMENTAL && dirty_region.dirty) {
    // XScuTimer_RestartTimer(&TimerInstance);
    memcpy(&vdma_frame_buffers[free], &vdma_frame_buffers[using],
           sizeof(vdma_frame_buffers[0]));
    // printf("Time to do memcpy: %.3f us\n", (double)(t1 - t2) / 333.0);
    
    // printf("min dirty x: %d\n", dirty_region.min_x);
    // printf("max dirty x: %d\n", dirty_region.max_x);
    
    // printf("min dirty y: %d\n", dirty_region.min_y);
    // printf("max dirty y: %d\n", dirty_region.max_y);
    for (int y = dirty_region.min_y; y <= dirty_region.max_y; y++) {
      size_t length =
          (dirty_region.max_x - dirty_region.min_x + 1) * sizeof(rgb888);
      Xil_DCacheFlushRange((UINTPTR)&vdma_frame_buffers[using][y * ZHDMI_WIDTH],
                           length);
      // memcpy(&vdma_frame_buffers[free][y * ZHDMI_WIDTH + dirty_region.min_x], &vdma_frame_buffers[using][y * ZHDMI_WIDTH + dirty_region.min_x], length);
    }
  } else {
    Xil_DCacheFlushRange((UINTPTR)vdma_frame_buffers[using],
                         sizeof(vdma_frame_buffers[0]));
  }

  XAxiVdma_StartParking(&vdma_instance, using, XAXIVDMA_READ);

  current_buffer_states[BUFFER_USING] = free;
  current_buffer_states[BUFFER_PARKED] = using;
  current_buffer_states[BUFFER_FREE] = parked;
  if (current_drawing_mode == ZHDMI_DRAW_INCREMENTAL) {
    dirty_region.dirty = false;
  }
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_verify_coordinate_is_valid_xy(uint16_t x,
                                                        uint16_t y) {
  uint16_t horizontal_axis_length =
      current_orientation.horizontal_axis_length_px;
  uint16_t vertical_axis_length = current_orientation.vertical_axis_length_px;

  // depends on current orientation set by user
  if (x >= horizontal_axis_length) {
    char error_message[100];
    snprintf(error_message, sizeof(error_message) - 1,
             "Error: coordinate %d exceeds maxium allowed value of %d", x,
             horizontal_axis_length - 1);
    log_error_message(error_message);
    return ZHDMI_FAILURE;
  }
  if (y >= vertical_axis_length) {
    char error_message[100];
    snprintf(error_message, sizeof(error_message) - 1,
             "Error: y coordinate %d exceeds maxium allowed value of %d", y,
             vertical_axis_length - 1);
    log_error_message(error_message);
    return ZHDMI_FAILURE;
  }
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_verify_coordinate_is_valid(ZHDMI_pixel_coordinate coordinate) {
  return ZHDMI_verify_coordinate_is_valid_xy(coordinate.x, coordinate.y);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_line(ZHDMI_pixel_coordinate p1,
                                    ZHDMI_pixel_coordinate p2, rgb888 colour) {
  return ZHDMI_draw_line_xy(p1.x, p1.y, p2.x, p2.y, colour);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_line_xy(uint16_t x1, uint16_t y1, uint16_t x2,
                                       uint16_t y2, rgb888 colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x1, y1) != ZHDMI_SUCCESS) {
    printf("Line point (%u, %u) is not valid\n", x1, y1);
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x2, y2) != ZHDMI_SUCCESS) {
    printf("Line point (%u, %u) is not valid\n", x2, y2);
    return ZHDMI_FAILURE;
  }
  if (x1 == x2) {
    return ZHDMI_draw_vline(x1, y1, y2, colour);
  } else if (y1 == y2) {
    return ZHDMI_draw_hline(y1, x1, x2, colour);
  }

  // Implement Bresenham's line algorithm

  int dx = abs(x2 - x1);
  int dy = -abs(y2 - y1);
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;

  uint16_t min_x = 0xFFFF, min_y = 0xFFFF;
  uint16_t max_x = 0, max_y = 0;

  while (1) {
    if (x1 < min_x) {
      min_x = x1;
    }
    if (x1 > max_x) {
      max_x = x1;
    }
    if (y1 < min_y) {
      min_y = y1;
    }
    if (y1 > max_y) {
      max_y = y1;
    }
    ZHDMI_set_pixel_xy_internal(x1, y1, colour);
    if (x1 == x2 && y1 == y2)
      break;

    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
  }
  update_dirty_region(min_x, max_x, min_y, max_y);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_hline(uint16_t y, uint16_t x1, uint16_t x2,
                                     rgb888 colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x1, y) != ZHDMI_SUCCESS) {
    printf("Point (x1,y) invalid\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x2, y) != ZHDMI_SUCCESS) {
    printf("Point (x2,y) invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_draw_hline_internal(y, x1, x2, colour);
  uint16_t min_x = x1 < x2 ? x1 : x2;
  uint16_t max_x = x1 > x2 ? x1 : x2;
  update_dirty_region(min_x, max_x, y, y);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_vline(uint16_t x, uint16_t y1, uint16_t y2,
                                     rgb888 colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x, y1) != ZHDMI_SUCCESS) {
    printf("Point (x,y1) invalid\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(x, y2) != ZHDMI_SUCCESS) {
    printf("Point (x,y2) invalid\n");
    return ZHDMI_FAILURE;
  }
  uint16_t min_y = y1 < y2 ? y1 : y2;
  uint16_t max_y = y1 > y2 ? y1 : y2;
  update_dirty_region(x, x, min_y, max_y);
  ZHDMI_draw_vline_internal(x, y1, y2, colour);
  return ZHDMI_SUCCESS;
}

void ZHDMI_set_background_colour(rgb888 background_colour) {
  current_background_colour = background_colour;
}

ZHDMI_RETURN_STATUS ZHDMI_clear(void) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  // ZHDMI_ORIENTATION temp = current_orientation.orientation_type;
  // switch to portrait mode briefly
  // ZHDMI_set_orientation(ZHDMI_LANDSCAPE_ORIENTATION);
  memset(vdma_frame_buffers[current_buffer_states[BUFFER_USING]], 0x00,
         sizeof(vdma_frame_buffers[0]));

  // ZHDMI_draw_rectangle_xy_internal(0, 0, ZHDMI_WIDTH, ZHDMI_HEIGHT, 1, true,
  //                                  BLACK, BLACK);
  if (current_drawing_mode == ZHDMI_DRAW_INCREMENTAL) {
    dirty_region.min_x = 0;
    dirty_region.min_y = 0;

    dirty_region.max_x = current_orientation.horizontal_axis_length_px - 1;
    dirty_region.max_y = current_orientation.vertical_axis_length_px - 1;
    dirty_region.dirty = true;
  }
  // restore user orientation
  // ZHDMI_set_orientation(temp);
  printf_x = 0;
  printf_y = printf_font.font_size;
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_background(void) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }

  rgb888 *dst = &vdma_frame_buffers[current_buffer_states[BUFFER_USING]][0];

  const rgb888 colour = current_background_colour;
  for (size_t i = 0; i < ZHDMI_WIDTH * ZHDMI_HEIGHT; i++) {
    dst[i] = colour;
  }

  if (current_drawing_mode == ZHDMI_DRAW_INCREMENTAL) {
    dirty_region.min_x = 0;
    dirty_region.min_y = 0;

    dirty_region.max_x = ZHDMI_WIDTH - 1;
    dirty_region.max_y = ZHDMI_HEIGHT - 1;
    dirty_region.dirty = true;
  }
  printf_x = 0;
  printf_y = printf_font.font_size;
  return ZHDMI_SUCCESS;
}

static void ZHDMI_draw_hline_internal(int16_t y, int16_t x1, int16_t x2,
                                      rgb888 colour) {
  if (y < 0 || y >= current_orientation.vertical_axis_length_px) {
    return;
  }
  int16_t start = (x1 < x2) ? x1 : x2;
  int16_t end = (x1 > x2) ? x1 : x2;
  if (start >= current_orientation.horizontal_axis_length_px) {
    // if smaller value is still off screen
    return;
  }
  if (end < 0) {
    // if larger value is still off screen
    return;
  }

  // clamp to bounds
  if (start < 0) {
    start = 0;
  }
  if (end >= current_orientation.horizontal_axis_length_px) {
    end = current_orientation.horizontal_axis_length_px - 1;
  }
  size_t start_index = current_transform_fun(start, y);
  uint16_t length = end - start + 1;
  
  // if (x1 == 0) {
  //   printf("STARTING INDEX: %zu\n", start_index);
  // }

  u32 cpu_buffer = current_buffer_states[BUFFER_USING];
  if (cpu_buffer >= 3) {
    printf("MAJOR ERROR\n");
    return;
  }

  switch (current_orientation.orientation_type) {
  case ZHDMI_PORTRAIT_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index - (i * ZHDMI_WIDTH)] = colour;
    }
    break;

  case ZHDMI_LANDSCAPE_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index + i] = colour;
    }
    break;

  case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index + (i * ZHDMI_WIDTH)] = colour;
    }
    break;

  case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index - i] = colour;
    }
    break;
  default:
    break;
  }
}

static void ZHDMI_draw_vline_internal(int16_t x, int16_t y1, int16_t y2,
                                      rgb888 colour) {
  if (x < 0 || x >= current_orientation.horizontal_axis_length_px) {
    return;
  }
  int16_t start = (y1 < y2) ? y1 : y2;
  int16_t end = (y1 > y2) ? y1 : y2;

  if (start >= current_orientation.vertical_axis_length_px) {
    // if smaller value is still off screen
    return;
  }
  if (end < 0) {
    // if larger value is still off screen
    return;
  }

  // clamp to bounds
  if (start < 0) {
    start = 0;
  }
  if (end >= current_orientation.vertical_axis_length_px) {
    end = current_orientation.vertical_axis_length_px - 1;
  }
  size_t start_index = current_transform_fun(x, start);
  u32 cpu_buffer = current_buffer_states[BUFFER_USING];
  uint16_t length = end - start + 1;
  switch (current_orientation.orientation_type) {
  case ZHDMI_PORTRAIT_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index + i] = colour;
    }
    break;

  case ZHDMI_LANDSCAPE_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index + (i * ZHDMI_WIDTH)] = colour;
    }
    break;

  case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index - i] = colour;
    }
    break;

  case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
    for (int i = 0; i < length; i++) {
      vdma_frame_buffers[cpu_buffer][start_index - (i * ZHDMI_WIDTH)] = colour;
    }
    break;
  default:
    break;
  }
}

static void ZHDMI_draw_line_xy_internal(int16_t x1, int16_t y1, int16_t x2,
                                        int16_t y2, rgb888 colour) {
  if (x1 == x2) {
    ZHDMI_draw_vline_internal(x1, y1, y2, colour);
    return;
  } else if (y1 == y2) {
    ZHDMI_draw_hline_internal(y1, x1, x2, colour);
    return;
  }

  // Implement Bresenham's line algorithm

  int dx = abs(x2 - x1);
  int dy = -abs(y2 - y1);
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;

  while (1) {
    // pixel set function will fail if coordinates are not valid
    ZHDMI_set_pixel_xy_internal(x1, y1, colour);
    if (x1 == x2 && y1 == y2)
      break;

    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
  }
}

static void ZHDMI_draw_line_internal(ZHDMI_internal_coordinate p1,
                                     ZHDMI_internal_coordinate p2,
                                     rgb888 colour) {
  ZHDMI_draw_line_xy_internal(p1.x, p1.y, p2.x, p2.y, colour);
}

static void
ZHDMI_draw_rectangle_xy_internal(uint16_t origin_x, uint16_t origin_y,
                                 uint16_t width_px, uint16_t height_px,
                                 uint16_t border_thickness_px, bool fill,
                                 rgb888 border_colour, rgb888 fill_colour) {
  if (border_thickness_px == 0) {
    // if user enters 0, set thickness to 1
    border_thickness_px = 1;
  }
  if (width_px == 0 || height_px == 0) {
    return;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(origin_x, origin_y) !=
      ZHDMI_SUCCESS) {
    return;
  }
  if (fill) {
    for (uint16_t y_current = origin_y; y_current < origin_y + height_px;
         y_current++) {
      ZHDMI_draw_hline_internal(y_current, origin_x, origin_x + width_px - 1,
                                fill_colour);
    }
  }
  // Draw borders of thickness border_thickness_px
  for (uint16_t t = 0; t < border_thickness_px; t++) {
    // Top border
    ZHDMI_draw_hline_internal(origin_y + t, origin_x, origin_x + width_px - 1,
                              border_colour);
    // Bottom border
    ZHDMI_draw_hline_internal(origin_y + height_px - t - 1, origin_x,
                              origin_x + width_px - 1, border_colour);
    // Left border
    ZHDMI_draw_vline_internal(origin_x + t, origin_y, origin_y + height_px - 1,
                              border_colour);
    // Right border
    ZHDMI_draw_vline_internal(origin_x + width_px - t - 1, origin_y,
                              origin_y + height_px - 1, border_colour);
  }
  return;
}

ZHDMI_RETURN_STATUS
ZHDMI_draw_unfilled_rectangle(ZHDMI_pixel_coordinate origin, uint16_t width_px,
                              uint16_t height_px, uint16_t border_thickness_px,
                              rgb888 border_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (width_px == 0 || height_px == 0) {
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(origin) != ZHDMI_SUCCESS) {
    printf("Rectangle origin point must be on the screen\n");
    return ZHDMI_FAILURE;
  }
  if ((border_thickness_px >= width_px / 2) ||
      (border_thickness_px >= height_px / 2)) {
    uint16_t smaller_side = (width_px > height_px) ? height_px : width_px;
    printf("border thickness for rectangle is too great. Passed %u but "
           "thickness should not exceed %u\n",
           border_thickness_px, smaller_side / 2);
    return ZHDMI_FAILURE;
  }
  update_dirty_region(origin.x, origin.x + width_px - 1, origin.y,
                      origin.y + height_px - 1);
  ZHDMI_draw_rectangle_xy_internal(origin.x, origin.y, width_px, height_px,
                                   border_thickness_px, false, border_colour,
                                   0x0);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_rectangle_xy(
    uint16_t origin_x, uint16_t origin_y, uint16_t width_px, uint16_t height_px,
    uint16_t border_thickness_px, rgb888 border_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (width_px == 0 || height_px == 0) {
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(origin_x, origin_y) !=
      ZHDMI_SUCCESS) {
    printf("Rectangle origin point must be on the screen\n");
    return ZHDMI_FAILURE;
  }
  if ((border_thickness_px >= width_px / 2) ||
      (border_thickness_px >= height_px / 2)) {
    uint16_t smaller_side = (width_px > height_px) ? height_px : width_px;
    printf("border thickness for rectangle is too great. Passed %u but "
           "thickness should not exceed %u\n",
           border_thickness_px, smaller_side / 2);
    return ZHDMI_FAILURE;
  }
  update_dirty_region(origin_x, origin_x + width_px - 1, origin_y,
                      origin_y + height_px - 1);
  ZHDMI_draw_rectangle_xy_internal(origin_x, origin_y, width_px, height_px,
                                   border_thickness_px, false, border_colour,
                                   0x0);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_rectangle(ZHDMI_pixel_coordinate origin, uint16_t width_px,
                            uint16_t height_px, uint16_t border_thickness_px,
                            rgb888 border_colour, rgb888 fill_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (width_px == 0 || height_px == 0) {
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(origin) != ZHDMI_SUCCESS) {
    printf("Rectangle origin point must be on the screen\n");
    return ZHDMI_FAILURE;
  }
  if ((border_thickness_px >= width_px / 2) ||
      (border_thickness_px >= height_px / 2)) {
    uint16_t smaller_side = (width_px > height_px) ? height_px : width_px;
    printf("border thickness for rectangle is too great. Passed %u but "
           "thickness should not exceed %u\n",
           border_thickness_px, smaller_side / 2);
    return ZHDMI_FAILURE;
  }
  update_dirty_region(origin.x, origin.x + width_px - 1, origin.y,
                      origin.y + height_px - 1);
  ZHDMI_draw_rectangle_xy_internal(origin.x, origin.y, width_px, height_px,
                                   border_thickness_px, true, border_colour,
                                   fill_colour);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_filled_rectangle_xy(
    uint16_t origin_x, uint16_t origin_y, uint16_t width_px, uint16_t height_px,
    uint16_t border_thickness_px, rgb888 border_colour, rgb888 fill_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (width_px == 0 || height_px == 0) {
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(origin_x, origin_y) !=
      ZHDMI_SUCCESS) {
    printf("Rectangle origin point must be on the screen\n");
    return ZHDMI_FAILURE;
  }
  if ((border_thickness_px >= width_px / 2) ||
      (border_thickness_px >= height_px / 2)) {
    uint16_t smaller_side = (width_px > height_px) ? height_px : width_px;
    printf("border thickness for rectangle is too great. Passed %u but "
           "thickness should not exceed %u\n",
           border_thickness_px, smaller_side / 2);
    return ZHDMI_FAILURE;
  }
  update_dirty_region(origin_x, origin_x + width_px - 1, origin_y,
                      origin_y + height_px - 1);
  ZHDMI_draw_rectangle_xy_internal(origin_x, origin_y, width_px, height_px,
                                   border_thickness_px, true, border_colour,
                                   fill_colour);

  return ZHDMI_SUCCESS;
}

static void fill_bottom_flat_triangle(ZHDMI_pixel_coordinate v1,
                                      ZHDMI_pixel_coordinate v2,
                                      ZHDMI_pixel_coordinate v3,
                                      rgb888 colour) {
  // use 16.16 fixed-point
  int32_t dx1 = ((int32_t)v2.x - (int32_t)v1.x) << 16;
  int32_t dx2 = ((int32_t)v3.x - (int32_t)v1.x) << 16;
  int32_t dy = (int32_t)v2.y - (int32_t)v1.y;

  if (dy == 0)
    return; // avoid divide by zero

  int32_t slope_1 = dx1 / dy;
  int32_t slope_2 = dx2 / dy;

  int32_t x1 = ((int32_t)v1.x) << 16;
  int32_t x2 = ((int32_t)v1.x) << 16;

  for (int y = v1.y; y <= v2.y; y++) {
    uint16_t start_x = (uint16_t)(x1 >> 16);
    uint16_t end_x = (uint16_t)(x2 >> 16);

    ZHDMI_draw_hline_internal(y, start_x, end_x, colour);

    x1 += slope_1;
    x2 += slope_2;
  }
  return;
}

static void fill_top_flat_triangle(ZHDMI_pixel_coordinate v1,
                                   ZHDMI_pixel_coordinate v2,
                                   ZHDMI_pixel_coordinate v3, rgb888 colour) {
  // use 16.16 fixed-point
  int32_t dx1 = ((int32_t)v3.x - (int32_t)v1.x) << 16;
  int32_t dx2 = ((int32_t)v3.x - (int32_t)v2.x) << 16;
  int32_t dy = (int32_t)v3.y - (int32_t)v1.y;

  if (dy == 0)
    return; // avoid divide by zero

  int32_t slope_1 = dx1 / dy;
  int32_t slope_2 = dx2 / dy;

  int32_t x1 = ((int32_t)v3.x) << 16;
  int32_t x2 = ((int32_t)v3.x) << 16;

  for (int y = v3.y; y > v1.y; y--) {
    uint16_t start_x = (uint16_t)(x1 >> 16);
    uint16_t end_x = (uint16_t)(x2 >> 16);

    ZHDMI_draw_hline_internal(y, start_x, end_x, colour);

    x1 -= slope_1; // decrement because we're iterating downward
    x2 -= slope_2;
  }
  return;
}

static ZHDMI_RETURN_STATUS
ZHDMI_draw_triangle_internal(ZHDMI_pixel_coordinate p1,
                             ZHDMI_pixel_coordinate p2,
                             ZHDMI_pixel_coordinate p3, rgb888 border_colour,
                             bool fill, rgb888 fill_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid(p1) != ZHDMI_SUCCESS) {
    printf("coordinate 1 of triangle is not in screen bounds\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(p2) != ZHDMI_SUCCESS) {
    printf("coordinate 2 of triangle is not in screen bounds\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(p3) != ZHDMI_SUCCESS) {
    printf("coordinate 3 of triangle is not in screen bounds\n");
    return ZHDMI_FAILURE;
  }
  // sort points by y
  ZHDMI_pixel_coordinate tmp;
  if (p2.y < p1.y) {
    tmp = p1;
    p1 = p2;
    p2 = tmp;
  }
  if (p3.y < p1.y) {
    tmp = p1;
    p1 = p3;
    p3 = tmp;
  }
  if (p3.y < p2.y) {
    tmp = p2;
    p2 = p3;
    p3 = tmp;
  }

  if (fill) {
    // handle trivial cases
    if (p2.y == p3.y) {
      fill_bottom_flat_triangle(p1, p2, p3, fill_colour);
    } else if (p1.y == p2.y) {
      fill_top_flat_triangle(p1, p2, p3, fill_colour);
    } else {
      /* general case - split the triangle in a topflat and bottom-flat one */
      ZHDMI_pixel_coordinate p4 = {
          .x = (uint16_t)(p1.x + ((float)(p2.y - p1.y) / (float)(p3.y - p1.y)) *
                                     (p3.x - p1.x)),
          .y = p2.y};

      fill_bottom_flat_triangle(p1, p2, p4, fill_colour);
      fill_top_flat_triangle(p2, p4, p3, fill_colour);
    }
  }
  ZHDMI_internal_coordinate p1_temp, p2_temp, p3_temp;
  p1_temp.x = p1.x;
  p1_temp.y = p1.y;
  p2_temp.x = p2.x;
  p2_temp.y = p2.y;
  p3_temp.x = p3.x;
  p3_temp.y = p3.y;
  ZHDMI_draw_line_internal(p1_temp, p2_temp, border_colour);
  ZHDMI_draw_line_internal(p2_temp, p3_temp, border_colour);
  ZHDMI_draw_line_internal(p1_temp, p3_temp, border_colour);

  uint16_t min_x = p1.x < p2.x ? p1.x : p2.x;
  min_x = min_x < p3.x ? min_x : p3.x;

  uint16_t max_x = p1.x > p2.x ? p1.x : p2.x;
  max_x = max_x > p3.x ? max_x : p3.x;

  uint16_t min_y = p1.y < p2.y ? p1.y : p2.y;
  min_y = min_y < p3.y ? min_y : p3.y;

  uint16_t max_y = p1.y > p2.y ? p1.y : p2.y;
  max_y = max_y > p3.y ? max_y : p3.y;
  update_dirty_region(min_x, max_x, min_y, max_y);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_triangle(ZHDMI_pixel_coordinate p1,
                                                 ZHDMI_pixel_coordinate p2,
                                                 ZHDMI_pixel_coordinate p3,
                                                 rgb888 border_colour) {
  return ZHDMI_draw_triangle_internal(p1, p2, p3, border_colour, false, 0x00);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_triangle_xy(uint16_t p1x, uint16_t p1y,
                                                    uint16_t p2x, uint16_t p2y,
                                                    uint16_t p3x, uint16_t p3y,
                                                    rgb888 border_colour) {
  ZHDMI_pixel_coordinate p1, p2, p3;
  p1 = ZHDMI_create_coordinate(p1x, p1y);
  p2 = ZHDMI_create_coordinate(p2x, p2y);
  p3 = ZHDMI_create_coordinate(p3x, p3y);
  return ZHDMI_draw_triangle_internal(p1, p2, p3, border_colour, false, 0x00);
}

ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_triangle(ZHDMI_pixel_coordinate p1, ZHDMI_pixel_coordinate p2,
                           ZHDMI_pixel_coordinate p3, rgb888 border_colour,
                           rgb888 fill_colour) {
  return ZHDMI_draw_triangle_internal(p1, p2, p3, border_colour, true,
                                      fill_colour);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_filled_triangle_xy(uint16_t p1x, uint16_t p1y,
                                                  uint16_t p2x, uint16_t p2y,
                                                  uint16_t p3x, uint16_t p3y,
                                                  rgb888 border_colour,
                                                  rgb888 fill_colour) {
  ZHDMI_pixel_coordinate p1, p2, p3;
  p1 = ZHDMI_create_coordinate(p1x, p1y);
  p2 = ZHDMI_create_coordinate(p2x, p2y);
  p3 = ZHDMI_create_coordinate(p3x, p3y);
  return ZHDMI_draw_triangle_internal(p1, p2, p3, border_colour, true,
                                      fill_colour);
}

static ZHDMI_RETURN_STATUS
ZHDMI_draw_circle_xy_internal(uint16_t origin_x, uint16_t origin_y,
                              uint16_t radius_px, rgb888 border_colour,
                              bool fill, rgb888 fill_colour) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(origin_x, origin_y) !=
      ZHDMI_SUCCESS) {
    printf("Circle origin is not on the screen\n");
    return ZHDMI_FAILURE;
  }
  if (radius_px > ZHDMI_WIDTH) {
    printf("Circle radius is too large\n");
    return ZHDMI_FAILURE;
  }
  int16_t origin_x_signed = origin_x;
  int16_t origin_y_signed = origin_y;
  if (fill) {
    // draw filled part of circle
    int x = 0;
    int y = radius_px;
    int d = 3 - (2 * radius_px);

    while (x <= y) {
      int16_t x_start_1 = origin_x - x;
      int16_t x_start_2 = origin_x - y;
      ZHDMI_draw_hline_internal(origin_y_signed - y, x_start_1, origin_x + x,
                                fill_colour);
      ZHDMI_draw_hline_internal(origin_y_signed - x, x_start_2, origin_x + y,
                                fill_colour);
      ZHDMI_draw_hline_internal(origin_y + x, x_start_2, origin_x + y,
                                fill_colour);
      ZHDMI_draw_hline_internal(origin_y + y, x_start_1, origin_x + x,
                                fill_colour);

      if (d < 0) {
        d += (4 * x) + 6;
      } else {
        d += 4 * (x - y) + 10;
        y--;
      }
      x++;
    }
  }

  // draw circle border
  int x = 0;
  int y = radius_px;
  int d = 3 - (2 * radius_px);
  while (x <= y) {
    // draw all 8 symmetric points
    ZHDMI_set_pixel_xy_internal(origin_x_signed + x, origin_y_signed + y,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed - x, origin_y_signed + y,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed + x, origin_y_signed - y,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed - x, origin_y_signed - y,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed + y, origin_y_signed + x,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed - y, origin_y_signed + x,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed + y, origin_y_signed - x,
                                border_colour);
    ZHDMI_set_pixel_xy_internal(origin_x_signed - y, origin_y_signed - x,
                                border_colour);
    if (d < 0) {
      d += (4 * x) + 6;
    } else {
      d += 4 * (x - y) + 10;
      y--;
    }
    x++;
  }
  uint16_t x_min, x_max, y_min, y_max;
  x_min = origin_x < radius_px ? 0 : origin_x - radius_px;
  y_min = origin_y < radius_px ? 0 : origin_y - radius_px;

  x_max = (origin_x + radius_px) >= ZHDMI_WIDTH ? ZHDMI_WIDTH - 1
                                                : origin_x + radius_px;
  y_max = (origin_y + radius_px) >= ZHDMI_HEIGHT ? ZHDMI_HEIGHT - 1
                                                 : origin_y + radius_px;
  update_dirty_region(x_min, x_max, y_min, y_max);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_circle(ZHDMI_pixel_coordinate origin,
                                               uint16_t radius_px,
                                               rgb888 circle_colour) {
  return ZHDMI_draw_circle_xy_internal(origin.x, origin.y, radius_px,
                                       circle_colour, false, 0x0);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_unfilled_circle_xy(uint16_t origin_x,
                                                  uint16_t origin_y,
                                                  uint16_t radius_px,
                                                  rgb888 circle_colour) {
  return ZHDMI_draw_circle_xy_internal(origin_x, origin_y, radius_px,
                                       circle_colour, false, 0x0);
}

ZHDMI_RETURN_STATUS ZHDMI_draw_filled_circle(ZHDMI_pixel_coordinate origin,
                                             uint16_t radius_px,
                                             rgb888 border_colour,
                                             rgb888 fill_colour) {
  return ZHDMI_draw_circle_xy_internal(origin.x, origin.y, radius_px,
                                       border_colour, true, fill_colour);
}

ZHDMI_RETURN_STATUS
ZHDMI_draw_filled_circle_xy(uint16_t origin_x, uint16_t origin_y,
                            uint16_t radius_px, rgb888 border_colour,
                            rgb888 fill_colour) {
  return ZHDMI_draw_circle_xy_internal(origin_x, origin_y, radius_px,
                                       border_colour, true, fill_colour);
}

ZHDMI_ORIENTATION ZHDMI_get_orientation(void) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_UNKNOWN_ORIENTATION;
  }
  return current_orientation.orientation_type;
}

ZHDMI_DRAWING_MODE ZHDMI_get_drawing_mode(void) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_UNKNOWN_DRAWING_MODE;
  }
  return current_drawing_mode;
}

void ZHDMI_set_drawing_mode(ZHDMI_DRAWING_MODE drawing_mode) {
  current_drawing_mode = drawing_mode;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_char_xy(char character, uint16_t base_x,
                                       uint16_t base_y, rgb888 colour,
                                       const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (f == NULL) {
    printf("font pointer is NULL\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for char draw invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_draw_char_xy_internal(character, base_x, base_y, colour, false, 0x00,
                              f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_char(char character, ZHDMI_pixel_coordinate base,
                                    rgb888 colour, const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (f == NULL) {
    printf("font pointer is NULL\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for char draw invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_draw_char_xy_internal(character, base.x, base.y, colour, false, 0x00,
                              f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_char_on_background_xy(
    char character, uint16_t base_x, uint16_t base_y, rgb888 colour,
    rgb888 background_colour, const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (f == NULL) {
    printf("font pointer is NULL\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for char draw invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_draw_char_xy_internal(character, base_x, base_y, colour, true,
                              background_colour, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_draw_char_on_background(char character, ZHDMI_pixel_coordinate base,
                              rgb888 colour, rgb888 background_colour,
                              const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (f == NULL) {
    printf("font pointer is NULL\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for char draw invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_draw_char_xy_internal(character, base.x, base.y, colour, true,
                              background_colour, f);
  return ZHDMI_SUCCESS;
}

static void ZHDMI_draw_char_xy_internal(char character, uint16_t base_x,
                                        uint16_t base_y, rgb888 colour,
                                        bool draw_background,
                                        rgb888 background_colour,
                                        const ZHDMI_font *f) {
  // assume pointers are valid if we've reached this point
  if (character < 32 || character > 127) {
    return;
  }
  // subtract 31 not 32 because of the reserved spot
  const glyph_dsc_t *dsc = &(f->glyph_descriptors[character - 31]);
  const uint8_t *current_character_bitmap =
      &(f->glyph_bitmap[dsc->bitmap_index]);

  int box_w = dsc->box_w;
  int box_h = dsc->box_h;
  int ofs_x = dsc->ofs_x;
  int ofs_y = dsc->ofs_y;

  uint16_t bit_index;
  uint8_t byte_index, bit_offset;

  int glyph_x0 = base_x + ofs_x;
  int glyph_y0 = base_y - box_h - ofs_y;

  int cell_w = dsc->adv_w >> 4;
  int cell_h = f->font_size;

  uint16_t min_x = 0xFFFF, min_y = 0xFFFF;
  uint16_t max_x = 0, max_y = 0;
  // important: Can be a negative value
  int8_t offset_y = dsc->ofs_y;
  for (int y = base_y - offset_y; y > (base_y - cell_h); y--) {
    for (int x = base_x; x < (base_x + cell_w); x++) {
      if (x < min_x) {
        min_x = x;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y < min_y) {
        min_y = y;
      }
      if (y > max_y) {
        max_y = y;
      }
    }
  }

  if (draw_background) {
    for (int y = base_y - offset_y; y > (base_y - cell_h); y--) {
      for (int x = base_x; x < (base_x + cell_w); x++) {
        ZHDMI_set_pixel_xy_internal(x, y, background_colour);
      }
    }
  }

  for (int16_t row = 0; row < box_h; row++) {
    for (int16_t column = 0; column < box_w; column++) {
      bit_index = (row * box_w) + column;
      byte_index = bit_index / 8; // integer division
      bit_offset = 7 - (bit_index % 8);
      if ((current_character_bitmap[byte_index] >> bit_offset) & 0x1) {
        ZHDMI_set_pixel_xy_internal(glyph_x0 + column, glyph_y0 + row, colour);
      }
    }
  }
  update_dirty_region(min_x, max_x, min_y, max_y);
  return;
}

static void ZHDMI_print_string_xy_internal(const char *string, uint16_t base_x,
                                           uint16_t base_y, rgb888 colour,
                                           bool draw_background,
                                           rgb888 background_colour,
                                           const ZHDMI_font *f) {

  if (string == NULL || f == NULL) {
    return;
  }
  // all characters must be drawable
  for (size_t i = 0; i < strlen(string); i++) {
    char c = *(string + i);
    if (c > 127 || c < 32) {
      // special case: newlines are ok
      if (c == '\n' || c == '\r') {
        continue;
      }
      return;
    }
  }
  const glyph_dsc_t *dsc;
  const char *string_cpy = string;
  int cursor_x = base_x, cursor_y = base_y;

  int8_t y_offset;
  uint16_t text_height = get_font_height(string_cpy, f, &y_offset);

  if (draw_background) {
    int16_t rectangle_length = 0;
    int16_t rectangle_start_x = base_x;
    uint16_t rectangle_start_y = base_y - text_height - y_offset;

    string_cpy = string;
    while (*string_cpy) {
      char c = *(string_cpy++);

      if (c == '\n' || c == '\r') {
        // draw current rectangle and move rectangle start position
        ZHDMI_draw_rectangle_xy_internal(rectangle_start_x, rectangle_start_y,
                                         rectangle_length >> 4, text_height, 1,
                                         draw_background, background_colour,
                                         background_colour);
        rectangle_length = 0;
        rectangle_start_y += text_height;
        if (rectangle_start_y >= current_orientation.vertical_axis_length_px) {
          break;
        }
        continue;
      } else {
        dsc = &(f->glyph_descriptors[c - 31]);
        rectangle_length += (dsc->adv_w);
      }
    }
    ZHDMI_draw_rectangle_xy_internal(
        rectangle_start_x, rectangle_start_y, rectangle_length >> 4,
        text_height, 1, draw_background, background_colour, background_colour);
  }

  string_cpy = string;
  cursor_x = base_x << 4;
  cursor_y = base_y;
  while (*string_cpy) {
    char c = *(string_cpy++);
    if (c == '\n' || c == '\r') {
      cursor_x = base_x << 4;
      cursor_y += text_height; // move down the screen
      if (cursor_y >= current_orientation.vertical_axis_length_px)
        break;
      continue;
    }
    dsc = &(f->glyph_descriptors[c - 31]);
    ZHDMI_draw_char_xy_internal(c, cursor_x >> 4, cursor_y, colour, false,
                                background_colour, f);
    cursor_x += (dsc->adv_w);
  }
}

// right margin indactes number of pixels that will not be touched
static void ZHDMI_print_wrapped_string_xy_internal(
    const char *string, uint16_t base_x, uint16_t base_y, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, bool draw_background,
    rgb888 background_colour, const ZHDMI_font *f) {
  if (string == NULL || f == NULL) {
    return;
  }
  if (right_margin >= current_orientation.horizontal_axis_length_px) {
    printf("Right margin too large for wrapped string\n");
    return;
  }
  if (left_margin >= current_orientation.horizontal_axis_length_px) {
    printf("Left margin too large for wrapped string\n");
    return;
  }

  // all characters must be drawable
  for (size_t i = 0; i < strlen(string); i++) {
    char c = *(string + i);
    if (c > 127 || c < 32) {
      // special case: newlines are ok
      if (c == '\n' || c == '\r') {
        continue;
      }
      return;
    }
  }
  const glyph_dsc_t *dsc;
  const char *string_cpy = string;
  int cursor_x = base_x, cursor_y = base_y;

  int8_t y_offset;
  uint16_t text_height = get_font_height(string_cpy, f, &y_offset);

  if (draw_background) {
    string_cpy = string;
    int cursor_x = base_x << 4;
    int cursor_y = base_y;
    int line_start_x = base_x;
    int line_length_px = 0;

    while (*string_cpy) {
      char c = *(string_cpy++);
      if (c == '\n' || c == '\r' ||
          ((cursor_x + f->glyph_descriptors[c - 31].adv_w) >> 4) >=
              (current_orientation.horizontal_axis_length_px - right_margin)) {

        // Draw background for this line
        ZHDMI_draw_rectangle_xy_internal(
            line_start_x, cursor_y - text_height - y_offset,
            line_length_px >> 4, text_height, 1, true, background_colour,
            background_colour);

        // Move to next line
        cursor_y += text_height;
        cursor_x = left_margin << 4;
        line_start_x = left_margin;
        line_length_px = 0;
        if (cursor_y >= current_orientation.vertical_axis_length_px)
          break;

        if (c == '\n' || c == '\r')
          continue;
      }

      const glyph_dsc_t *dsc = &(f->glyph_descriptors[c - 31]);
      line_length_px += dsc->adv_w;
      cursor_x += dsc->adv_w;
    }

    // draw background for last line if any text remains
    if (line_length_px > 0) {
      ZHDMI_draw_rectangle_xy_internal(
          line_start_x, cursor_y - text_height - y_offset, line_length_px >> 4,
          text_height, 1, true, background_colour, background_colour);
    }
  }

  string_cpy = string;
  cursor_x = base_x << 4;
  cursor_y = base_y;
  while (*string_cpy) {
    char c = *(string_cpy++);
    if (c == '\n' || c == '\r' ||
        ((cursor_x + (f->glyph_descriptors[c - 31].adv_w)) >> 4) >=
            (current_orientation.horizontal_axis_length_px - right_margin)) {
      cursor_y += text_height;
      cursor_x = left_margin << 4;
      if (c == '\n' || c == '\r') {
        continue;
      }
    }
    dsc = &(f->glyph_descriptors[c - 31]);
    ZHDMI_draw_char_xy_internal(c, cursor_x >> 4, cursor_y, colour, false,
                                background_colour, f);
    cursor_x += (dsc->adv_w);
  }
}

ZHDMI_RETURN_STATUS
ZHDMI_print_wrapped_string_xy(const char *string, uint16_t base_x,
                              uint16_t base_y, uint16_t left_margin,
                              uint16_t right_margin, rgb888 colour,
                              const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_wrapped_string_xy_internal(string, base_x, base_y, left_margin,
                                         right_margin, colour, false, 0x0, f);

  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_print_wrapped_string(const char *string, ZHDMI_pixel_coordinate base,
                           uint16_t left_margin, uint16_t right_margin,
                           rgb888 colour, const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_wrapped_string_xy_internal(string, base.x, base.y, left_margin,
                                         right_margin, colour, false, 0x0, f);

  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_print_wrapped_string_on_background(
    const char *string, ZHDMI_pixel_coordinate base, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, rgb888 background_colour,
    const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_wrapped_string_xy_internal(string, base.x, base.y, left_margin,
                                         right_margin, colour, true,
                                         background_colour, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_print_wrapped_string_on_background_xy(
    const char *string, uint16_t base_x, uint16_t base_y, uint16_t left_margin,
    uint16_t right_margin, rgb888 colour, rgb888 background_colour,
    const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_wrapped_string_xy_internal(string, base_x, base_y, left_margin,
                                         right_margin, colour, true,
                                         background_colour, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_print_string_xy(const char *string, uint16_t base_x,
                                          uint16_t base_y, rgb888 colour,
                                          const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_string_xy_internal(string, base_x, base_y, colour, false, 0x0, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_print_string(const char *string,
                                       ZHDMI_pixel_coordinate base,
                                       rgb888 colour, const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_string_xy_internal(string, base.x, base.y, colour, false, 0x0, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_print_string_on_background(const char *string,
                                 ZHDMI_pixel_coordinate base, rgb888 colour,
                                 rgb888 background_colour,
                                 const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid(base) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }
  ZHDMI_print_string_xy_internal(string, base.x, base.y, colour, true,
                                 background_colour, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_print_string_on_background_xy(const char *string, uint16_t base_x,
                                    uint16_t base_y, rgb888 colour,
                                    rgb888 background_colour,
                                    const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (ZHDMI_verify_coordinate_is_valid_xy(base_x, base_y) != ZHDMI_SUCCESS) {
    printf("Base coordinate for string write invalid\n");
    return ZHDMI_FAILURE;
  }

  ZHDMI_print_string_xy_internal(string, base_x, base_y, colour, true,
                                 background_colour, f);
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS ZHDMI_draw_image(ZHDMI_pixel_coordinate image_origin,
                                     const ZHDMI_image *image) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (image == NULL) {
    printf("ZHDMI_image provided to ZHDMI_draw_image is NULL\n");
    return ZHDMI_FAILURE;
  }
  if (ZHDMI_verify_coordinate_is_valid(image_origin) != ZHDMI_SUCCESS) {
    printf("Base coordinate for image draw is invalid\n");
    return ZHDMI_FAILURE;
  }
  uint16_t width = image->width;
  uint16_t height = image->height;

  uint16_t offset_x = image->offset_x;
  uint16_t offset_y = image->offset_y;
  if (offset_x >= width || offset_y >= height) {
    printf("Image offset too large (x=%u, y=%u)\n", offset_x, offset_y);
    return ZHDMI_FAILURE;
  }

  uint16_t start_x = image_origin.x;
  uint16_t start_y = image_origin.y;

  uint16_t max_x = current_orientation.horizontal_axis_length_px;
  uint16_t max_y = current_orientation.vertical_axis_length_px;

  // clamp draw area to monitor bounds
  uint16_t draw_w = width - offset_x;
  uint16_t draw_h = height - offset_y;
  uint16_t end_x = (start_x + draw_w <= max_x) ? (start_x + draw_w) : max_x;
  uint16_t end_y = (start_y + draw_h <= max_y) ? (start_y + draw_h) : max_y;

  // cast byte array to u32
  const rgb888 *map = (u32 *)(image->map);
  u32 cpu_buffer = current_buffer_states[BUFFER_USING];

  for (int y = start_y; y < end_y; y++) {
    for (int x = start_x; x < end_x; x++) {
      uint16_t landscape_pixel_x;
      uint16_t landscape_pixel_y;

      // (y - start_y goes from 0 -> end of drawn region)
      uint16_t img_y = offset_y + y - start_y;
      uint16_t img_x = offset_x + x - start_x;

      switch (current_orientation.orientation_type) {
      case ZHDMI_LANDSCAPE_ORIENTATION:
        landscape_pixel_x = x;
        landscape_pixel_y = y;
        break;
      case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
        landscape_pixel_x = ZHDMI_WIDTH - 1 - x;
        landscape_pixel_y = ZHDMI_HEIGHT - 1 - y;
        break;
      case ZHDMI_PORTRAIT_ORIENTATION:
        landscape_pixel_x = y;
        landscape_pixel_y = ZHDMI_HEIGHT - 1 - x;
        break;
      case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
        landscape_pixel_x = ZHDMI_WIDTH - 1 - y;
        landscape_pixel_y = x;
        break;
      default:
        printf("ERROR: Invalid orientation\n");
        return ZHDMI_FAILURE;
      }
      size_t landscape_pixel =
          landscape_pixel_y * ZHDMI_WIDTH + landscape_pixel_x;
      vdma_frame_buffers[cpu_buffer][landscape_pixel] =
          map[img_y * width + img_x];
    }
  }
  return ZHDMI_SUCCESS;
}

static void ZHDMI_print_aligned_string_internal(
    const char *string, uint16_t base_y, ZHDMI_TEXT_ALIGNMENT alignment,
    rgb888 colour, bool draw_background, rgb888 background_colour,
    const ZHDMI_font *f) {
  switch (alignment) {
  case ZHDMI_ALIGN_LEFT:
    ZHDMI_print_string_xy_internal(string, 0, base_y, colour, draw_background,
                                   background_colour, f);
    break;
  case ZHDMI_ALIGN_CENTER:
  case ZHDMI_ALIGN_RIGHT:;
    char string_cpy[256];
    if (strlen(string) >= sizeof(string_cpy)) {
      printf("Warning: Truncating long string for alignment\n");
    }

    strncpy(string_cpy, string, sizeof(string_cpy) - 1);
    string_cpy[sizeof(string_cpy) - 1] = '\0';
    uint16_t string_length_px = 0;

    uint16_t width = current_orientation.horizontal_axis_length_px;
    char *token = strtok(string_cpy, "\n");
    if (token == NULL) {
      for (size_t i = 0; i < strlen(string_cpy); i++) {
        char c = string_cpy[i];
        string_length_px += f->glyph_descriptors[c - 31].adv_w;
      }
      string_length_px >>= 4; // account for scaling factor (16)
      uint16_t unused_px = width - string_length_px;
      uint16_t x_offset = unused_px;
      if (alignment == ZHDMI_ALIGN_CENTER) {
        x_offset /= 2;
      }
      ZHDMI_print_string_xy_internal(string, x_offset, base_y, colour,
                                     draw_background, background_colour, f);
      break;
    }
    while (token) {
      string_length_px = 0;
      // get pixel length of each substring
      const glyph_dsc_t *dsc;
      int16_t min_y = current_orientation.vertical_axis_length_px - 1;
      int16_t max_y = 0;
      for (size_t i = 0; i < strlen(token); i++) {
        char c = token[i];
        dsc = &(f->glyph_descriptors[c - 31]);
        int16_t glyph_y0 = base_y - dsc->box_h - dsc->ofs_y;
        int16_t glyph_y1 = glyph_y0 + dsc->box_h;

        if (glyph_y0 < min_y)
          min_y = glyph_y0;
        if (glyph_y1 > max_y)
          max_y = glyph_y1;
        string_length_px += f->glyph_descriptors[c - 31].adv_w;
      }
      int16_t text_height = max_y - min_y;
      string_length_px >>= 4; // account for scaling factor (16)

      uint16_t unused_px = width - string_length_px;
      uint16_t x_offset = unused_px;
      if (alignment == ZHDMI_ALIGN_CENTER) {
        x_offset /= 2;
      }
      ZHDMI_print_string_xy_internal(token, x_offset, base_y, colour,
                                     draw_background, background_colour, f);
      base_y += text_height;
      token = strtok(NULL, "\n");
    }
    break;
  default:
    return;
  }
}

ZHDMI_RETURN_STATUS
ZHDMI_print_aligned_string(const char *string, uint16_t base_y,
                           ZHDMI_TEXT_ALIGNMENT alignment, rgb888 colour,
                           const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (base_y >= current_orientation.vertical_axis_length_px) {
    printf(
        "String vertical allignment is invalid. Passed %u, must be below %u\n",
        base_y, current_orientation.vertical_axis_length_px);
    return ZHDMI_FAILURE;
  }
  if (!string || !f) {
    return ZHDMI_FAILURE;
  }
  switch (alignment) {
  case ZHDMI_ALIGN_LEFT:
  case ZHDMI_ALIGN_CENTER:
  case ZHDMI_ALIGN_RIGHT:
    ZHDMI_print_aligned_string_internal(string, base_y, alignment, colour,
                                        false, 0x0, f);
    return ZHDMI_SUCCESS;
  default:
    printf("Passed in invalid alignment option\n");
    return ZHDMI_FAILURE;
  }
  return ZHDMI_SUCCESS;
}

ZHDMI_RETURN_STATUS
ZHDMI_print_aligned_string_on_background(const char *string, uint16_t base_y,
                                         ZHDMI_TEXT_ALIGNMENT alignment,
                                         rgb888 colour,
                                         rgb888 background_colour,
                                         const ZHDMI_font *f) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (base_y >= current_orientation.vertical_axis_length_px) {
    printf(
        "String vertical allignment is invalid. Passed %u, must be below %u\n",
        base_y, current_orientation.vertical_axis_length_px);
    return ZHDMI_FAILURE;
  }
  if (!string || !f) {
    return ZHDMI_FAILURE;
  }
  switch (alignment) {
  case ZHDMI_ALIGN_LEFT:
  case ZHDMI_ALIGN_CENTER:
  case ZHDMI_ALIGN_RIGHT:
    ZHDMI_print_aligned_string_internal(string, base_y, alignment, colour, true,
                                        background_colour, f);
    return ZHDMI_SUCCESS;
  default:
    printf("Passed in invalid alignment option\n");
    return ZHDMI_FAILURE;
  }
}

ZHDMI_image lvgl_image_to_ZHDMI(const lv_image_dsc_t *lv_struct, uint16_t x_off,
                                uint16_t y_off) {
  if (lv_struct == NULL) {
    ZHDMI_image empty_img = {0};
    return empty_img;
  }
  ZHDMI_image img = {.width = lv_struct->header.w,
                     .height = lv_struct->header.h,
                     .offset_x = x_off,
                     .offset_y = y_off,
                     .data_size = lv_struct->data_size,
                     .map = (rgb888 *)(lv_struct->data)};
  return img;
}

ZHDMI_font lvgl_font_to_ZHDMI(const glyph_dsc_t *lv_struct,
                              const uint8_t *glyph_bitmap, const char *name,
                              size_t font_size) {
  if (lv_struct == NULL || glyph_bitmap == NULL) {
    ZHDMI_font empty_font = {0};
    return empty_font;
  }
  ZHDMI_font font = {.font_size = font_size,
                     .glyph_descriptors = lv_struct,
                     .glyph_bitmap = glyph_bitmap};
  strncpy(font.font_name, name, sizeof(font.font_name) - 1);
  font.font_name[sizeof(font.font_name) - 1] = '\0';
  return font;
}

static size_t pixel_coordinate_to_internal_index_landscape(uint16_t x,
                                                           uint16_t y) {
  return ((size_t)y * ZHDMI_WIDTH + x);
}

static size_t
pixel_coordinate_to_internal_index_inverted_landscape(uint16_t x, uint16_t y) {
  size_t converted_x, converted_y;
  converted_x = ZHDMI_WIDTH - 1 - x;
  converted_y = ZHDMI_HEIGHT - 1 - y;
  return (converted_y * ZHDMI_WIDTH + converted_x);
}

static size_t pixel_coordinate_to_internal_index_portrait(uint16_t x,
                                                          uint16_t y) {
  size_t converted_x, converted_y;
  converted_x = y;
  converted_y = ZHDMI_HEIGHT - 1 - x;
  return (converted_y * ZHDMI_WIDTH + converted_x);
}

static size_t pixel_coordinate_to_internal_index_inverted_portrait(uint16_t x,
                                                                   uint16_t y) {
  size_t converted_x, converted_y;
  converted_x = ZHDMI_WIDTH - 1 - y;
  converted_y = x;
  return (converted_y * ZHDMI_WIDTH + converted_x);
}

ZHDMI_RETURN_STATUS ZHDMI_printf(const char *format, ...) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  char buffer[256];
  buffer[sizeof(buffer) - 1] = '\0';

  if (printf_y >= current_orientation.vertical_axis_length_px) {
    printf_y = printf_font.font_size;
  }
  uint16_t starting_x_value = printf_x;

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer) - 1, format, args);
  va_end(args);

  rgb888 fg = ~current_background_colour; // simple bitwise inverse for contrast

  // all characters must be drawable
  for (size_t i = 0; i < strlen(buffer); i++) {
    char c = *(buffer + i);
    if (c > 127 || c < 32) {
      // special case: newlines are ok
      if (c == '\n' || c == '\r') {
        continue;
      }
      printf("ZHDMI_printf() string argument must have printable characters\n");
      return ZHDMI_FAILURE;
    }
  }
  const glyph_dsc_t *dsc;
  const char *string_cpy = buffer;

  int8_t y_offset = -3;
  uint16_t text_height = printf_font.font_size;
  // uint16_t text_height = get_font_height(string_cpy, &printf_font,
  // &y_offset);

  string_cpy = buffer;
  int cursor_x = printf_x << 4;
  int cursor_y = printf_y;
  int line_start_x = printf_x;
  int line_length_px = 0;

  uint16_t starting_y_value = printf_y;

  while (*string_cpy) {
    char c = *(string_cpy++);
    if (c == '\n' || c == '\r' ||
        ((cursor_x + printf_font.glyph_descriptors[c - 31].adv_w) >> 4) >=
            (current_orientation.horizontal_axis_length_px)) {

      // Draw background for this line
      ZHDMI_draw_rectangle_xy_internal(
          line_start_x, cursor_y - text_height - y_offset, line_length_px >> 4,
          text_height, 1, true, current_background_colour,
          current_background_colour);

      // Move to next line
      cursor_y += text_height;
      cursor_x = 0;
      line_start_x = 0;
      line_length_px = 0;
      if (cursor_y >= current_orientation.vertical_axis_length_px) {
        break;
      }
      if (c == '\n' || c == '\r')
        continue;
    }
    const glyph_dsc_t *dsc = &(printf_font.glyph_descriptors[c - 31]);
    line_length_px += dsc->adv_w;
    cursor_x += dsc->adv_w;
  }

  // draw background for last line if any text remains
  if (line_length_px > 0) {
    ZHDMI_draw_rectangle_xy_internal(
        line_start_x, cursor_y - text_height - y_offset, line_length_px >> 4,
        text_height, 1, true, current_background_colour,
        current_background_colour);
  }

  string_cpy = buffer;
  cursor_x = printf_x << 4;
  cursor_y = printf_y;
  while (*string_cpy) {
    char c = *(string_cpy++);
    if (c == '\n' || c == '\r' ||
        ((cursor_x + (printf_font.glyph_descriptors[c - 31].adv_w)) >> 4) >=
            (current_orientation.horizontal_axis_length_px)) {
      cursor_y += text_height;
      cursor_x = 0;
      if (c == '\n' || c == '\r') {
        continue;
      }
    }
    dsc = &(printf_font.glyph_descriptors[c - 31]);
    ZHDMI_draw_char_xy_internal(c, cursor_x >> 4, cursor_y, fg, false,
                                current_background_colour, &printf_font);
    cursor_x += (dsc->adv_w);
  }
  printf_x = cursor_x >> 4;
  printf_y = cursor_y;
  if (current_printf_mode == ZHDMI_PRINTF_MODE_OVERWRITE) {
    printf_x = starting_x_value;
    printf_y = starting_y_value;
  }
  return ZHDMI_refresh_display();
}

static uint16_t get_font_height(const char *string, const ZHDMI_font *f,
                                int8_t *y_offset) {
  if (string == NULL || f == NULL || f->glyph_descriptors == NULL) {
    if (y_offset)
      *y_offset = 0;
    printf("WARNING: NULL PTR passed to get_font_height() function\n");
    return 0;
  }

  uint8_t max_px_value = 0;
  int8_t min_px_value = INT8_MAX;

  while (*string) {
    char c = *(string++);
    if (c == '\n' || c == '\r')
      continue;

    int index = (int)c - 31;
    const lv_font_fmt_txt_glyph_dsc_t *dsc = &f->glyph_descriptors[index];

    int16_t glyph_over = (int)dsc->box_h + dsc->ofs_y;
    int8_t glyph_under = dsc->ofs_y;

    if (glyph_over > max_px_value) {
      max_px_value = glyph_over;
    }
    if (glyph_under < min_px_value) {
      min_px_value = glyph_under;
    }
  }

  if (y_offset)
    *y_offset = (int8_t)min_px_value;
  return (uint16_t)(max_px_value - min_px_value);
}

ZHDMI_RETURN_STATUS ZHDMI_set_printf_mode(ZHDMI_PRINTF_MODE mode) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  if (mode != ZHDMI_PRINTF_MODE_OVERWRITE && mode != ZHDMI_PRINTF_MODE_SCROLL) {
    printf("invalid printf mode selected!\n");
    return ZHDMI_FAILURE;
  }
  current_printf_mode = mode;
  return ZHDMI_SUCCESS;
}

ZHDMI_PRINTF_MODE ZHDMI_get_printf_mode(void) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_PRINTF_MODE_UNKNOWN;
  }
  return current_printf_mode;
}

ZHDMI_RETURN_STATUS ZHDMI_set_printf_cursor(ZHDMI_pixel_coordinate coord) {
  return ZHDMI_set_printf_cursor_xy(coord.x, coord.y);
}

ZHDMI_RETURN_STATUS ZHDMI_set_printf_cursor_xy(uint16_t cursor_x,
                                               uint16_t cursor_y) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return ZHDMI_ERR_NOT_INITIALIZED;
  }
  uint16_t max_x = current_orientation.horizontal_axis_length_px - 1;
  uint16_t max_y = current_orientation.vertical_axis_length_px - 1;
  if (cursor_x > max_x || cursor_y > max_y) {
    char msg[120];
    snprintf(msg, sizeof(msg) - 1,
             "Error: cannot set printf cursor to (%hu, %hu) because max bounds "
             "are (%hu, %hu)",
             cursor_x, cursor_y, max_x, max_y);
    log_error_message(msg);
    return ZHDMI_FAILURE;
  }
  if (cursor_y < printf_font.font_size) {
    cursor_y = printf_font.font_size;
  }
  printf_x = cursor_x;
  printf_y = cursor_y;
  return ZHDMI_SUCCESS;
}

// ZHDMI_image ZHDMI_read_BMP(const uint8_t *BMP_data, size_t BMP_data_length,
//                            uint8_t *map_destination_arr,
//                            size_t map_destination_size) {
//   ZHDMI_image image_to_return = {0};
//   if (BMP_data == NULL || map_destination_arr == NULL) {
//     printf("Passed NULL array into ZHDMI_read_BMP() function\n");
//     return image_to_return;
//   }
//   if (!ZHDMI_initialized) {
//     printf(
//         "Initialize with ZHDMI_init() before calling other ZHDMI
//         functions\n");
//     return image_to_return;
//   }
//   if (BMP_data_length <= sizeof(BMPHeader)) {
//     return image_to_return;
//   }
//   const BMPHeader *header = (const BMPHeader *)BMP_data;

//   // printf("Header signature: %x\n", header->signature);
//   // printf("Header file size: %lu\n", (long unsigned int)header->file_size);
//   // printf("Header image size: %lu\n", (long unsigned int)header->img_size);
//   // printf("Header pixel array offset: %lu\n",
//   //        (long unsigned int)header->pixel_offset);
//   // printf("Header DIB header size (>= 40): %02lu\n",
//   //        (long unsigned int)header->dib_header_size);
//   // printf("Header Width: %lu\n", (long unsigned int)header->width);
//   // printf("Header Height: %d\n", (int)header->height);
//   // printf("Header planes (should be 1): %hu\n", header->planes);
//   // printf("Header bits per pixel: %02hu\n", header->bits_per_pixel);
//   // printf("Header compression (should be 0): %lu\n",
//   //        (long unsigned int)header->compression);
//   // printf("Header colours used: %lu\n", (long unsigned
//   // int)header->colors_used); printf("Header important colours: %lu\n",
//   //        (long unsigned int)header->important_colors);

//   if (strncmp("BM", (const char *)&(header->signature), 2) != 0) {
//     printf("File is not a BMP file\n");
//     return image_to_return;
//   }
//   if (header->dib_header_size != 40) {
//     return image_to_return;
//   }
//   if (header->compression != 0) {
//     printf("Compression is a non-zero value -- this BMP cannot be
//     displayed\n"); return image_to_return;
//   }
//   size_t img_map_size = sizeof(rgb888) * header->width * header->height;
//   printf("image map size: %lu\nmap destination size: %lu\n",
//          (unsigned long int)img_map_size,
//          (unsigned long int)map_destination_size);
//   // check to make sure size of BMP is <= map_destination_size
//   if (img_map_size > map_destination_size) {
//     printf("Warning: image map size exceeds destination size\n");
//     return image_to_return;
//   }
//   uint32_t bits_per_pixel = header->bits_per_pixel;
//   if (bits_per_pixel != 1 && bits_per_pixel != 16 && bits_per_pixel != 24 &&
//       bits_per_pixel != 32) {
//     printf("Only 1/16/24/32-bit BMP supported.\n");
//     return image_to_return;
//   }

//   uint32_t width = header->width;
//   int32_t height = header->height;
//   switch (bits_per_pixel) {
//   case 1:
//   case 16:
//   case 24:
//   case 32:
//     // valid options
//     break;
//   default:
//     printf("Invalid number of bits per pixel detected (%d)\n",
//            (int)bits_per_pixel);
//     return image_to_return;
//   }

//   const uint8_t *pixel_data = BMP_data + header->pixel_offset;

//   uint16_t row_bytes_unpadded; // number of unpadded bytes
//   uint16_t row_bytes_padded;   // number of unpadded + padded bytes

//   // monochrome images
//   if (bits_per_pixel == 1) {

//     row_bytes_unpadded = (width + 7) / 8;
//     row_bytes_padded = 4 * ((row_bytes_unpadded + 3) / 4); // full length

//     const uint8_t *colour_table =
//         (uint8_t *)(BMP_data + 14 + header->dib_header_size);
//     // colour table is always 4 * num_colours bytes long
//     rgb888 colour_pallete[2];

//     uint8_t r, g, b;
//     b = colour_table[0];
//     g = colour_table[1];
//     r = colour_table[2];
//     colour_pallete[0] = RGB888(r, g, b);
//     // skip byte 4 (stuff byte)

//     b = colour_table[4];
//     g = colour_table[5];
//     r = colour_table[6];

//     // skip byte 8 (stuff byte)
//     colour_pallete[1] = RGB888(r, g, b);

//     uint32_t abs_height = height > 0 ? height : -height;
//     for (uint32_t row = 0; row < abs_height; row++) {

//       const uint8_t *bmp_row;
//       if (height > 0)
//         bmp_row = pixel_data + (height - 1 - row) * row_bytes_padded;
//       else
//         bmp_row = pixel_data + row * row_bytes_padded;

//       rgb888 *out_row =
//           (rgb888 *)(map_destination_arr + (row * width * sizeof(rgb888)));

//       for (uint32_t x = 0; x < width; x++) {
//         uint32_t byte_index = x / 8;
//         uint8_t bit_index = 7 - (x % 8);

//         uint8_t bit = (bmp_row[byte_index] >> bit_index) & 0x1;
//         out_row[x] = colour_pallete[bit];
//       }
//     }
//   }
//   // rgb888
//   else if (bits_per_pixel == 16) {

//   }
//   // RGB
//   else if (bits_per_pixel == 24) {
//     row_bytes_unpadded = 3 * width;
//     row_bytes_padded = row_bytes_unpadded;
//     while (row_bytes_padded % 4 != 0)
//       row_bytes_padded++;

//     typedef struct {
//       uint8_t b;
//       uint8_t g;
//       uint8_t r;
//     } BGR_block;

//     uint32_t abs_height = height > 0 ? height : -height;
//     for (uint32_t row = 0; row < abs_height; row++) {

//       const uint8_t *bmp_row;
//       if (height > 0)
//         bmp_row = pixel_data + (height - 1 - row) * row_bytes_padded;
//       else
//         bmp_row = pixel_data + row * row_bytes_padded;

//       rgb888 *out_row =
//           (rgb888 *)(map_destination_arr + (row * width * sizeof(rgb888)));

//       for (uint32_t x = 0; x < width; x++) {
//         BGR_block *block = (BGR_block *)&bmp_row[3 * x];
//         rgb888 colour = RGB888(block->r, block->g, block->b);
//         out_row[x] = colour;
//       }
//     }
//   }
//   // RGBA
//   else {
//   }
//   // set map to user provided array start point
//   image_to_return.map = (const rgb888 *)map_destination_arr;
//   image_to_return.data_size = img_map_size;
//   image_to_return.width = width;
//   image_to_return.height = height ? height : -height;
//   return image_to_return;
// }

void print_output(const char *fmt, ...) {
  if (!ZHDMI_initialized) {
    printf(
        "Initialize with ZHDMI_init() before calling other ZHDMI functions\n");
    return;
  }
  va_list args;
  char buffer[256];

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  printf("%s", buffer);       // Print to UART
  ZHDMI_printf("%s", buffer); // Print to screen
}

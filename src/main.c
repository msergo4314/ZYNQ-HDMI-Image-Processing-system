#include <sleep.h>
#include <xgpio.h>
#include <xil_cache.h>
#include <xil_exception.h>
#include <xil_printf.h>
#include <xinterrupt_wrap.h>
#include <xscutimer.h>
#include <xstatus.h>

#include "lvgl_compat.h"
#include "zhdmi.h" // HDMI graphics library

static const ZHDMI_DRAWING_MODE drawing_mode = ZHDMI_DRAW_INCREMENTAL;

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

#define NUM_FILTERS 7

void print_filter_type(int filter_num) {
  char filter_name[50];
  switch (filter_num) {
  case 0:
    strcpy(filter_name, "No filter");
    break;
  case 1:
    strcpy(filter_name, "Grayscale transform");
    break;
  case 2:
    strcpy(filter_name, "Inversion transform");
    break;
  case 3:
    strcpy(filter_name, "Laplacian edge detection transform");
    break;
  case 4:
    strcpy(filter_name, "Blur transform");
    break;
  case 5:
    strcpy(filter_name, "Sharpen transform");
    break;
  case 6:
    strcpy(filter_name, "Horizontal edge detection transform");
    break;
  case 7:
    strcpy(filter_name, "Vertical edge detection transform");
    break;
  default:
    strcpy(filter_name, "Unknown filter type");
    break;
  }
  printf("Applied filter: %s\n", filter_name);
}

void GpioHandler(void *CallbackRef) {
  static u32 previous_button_pressed_value = 0;
  static u32 filter_val = 0;

  XGpio *GpioPtr = CallbackRef;

  XGpio_InterruptClear(GpioPtr, XGPIO_IR_CH1_MASK);

  // Button is active low, so invert it
  u32 button_pressed = !XGpio_DiscreteRead(GpioPtr, 1);

  // simple software debounce
  msleep(25);
  u32 check = !XGpio_DiscreteRead(GpioPtr, 1);
  if (check != button_pressed) {
    return;
  }

  // Rising edge of button_pressed = physical button press
  if (!previous_button_pressed_value && button_pressed) {

    if (filter_val < NUM_FILTERS) {
      filter_val++;
    } else {
      filter_val = 0;
    }
    XGpio_DiscreteWrite(GpioPtr, 2, filter_val);

    print_filter_type(filter_val);
  }

  previous_button_pressed_value = button_pressed;
}

extern lv_image_dsc_t doggo;
extern lv_image_dsc_t dark_souls_aura_farm;
int main(void) {

  setup_timer();
  s32 Status;

  // GPIO setup first
  XGpio filter;

  /* ---------------- GPIO ---------------- */

  Status = XGpio_Initialize(&filter, XPAR_XGPIO_0_BASEADDR);

  if (Status != XST_SUCCESS)
    return XST_FAILURE;

  XGpio_SetDataDirection(&filter, 1, 1); // input button
  XGpio_SetDataDirection(&filter, 2, 0);

  /* ---------------- GIC ---------------- */
  Status = XSetupInterruptSystem(
      &filter, (void *)GpioHandler, XPAR_FABRIC_AXI_GPIO_0_INTR,
      XPAR_AXI_GPIO_0_INTERRUPT_PARENT, XINTERRUPT_DEFAULT_PRIORITY);

  XGpio_InterruptClear(&filter, XGPIO_IR_CH1_MASK);

  XGpio_InterruptEnable(&filter, XGPIO_IR_CH1_MASK);

  XGpio_InterruptGlobalEnable(&filter);

  /* ---------------- Initialize HDMI driver ---------------- */

  ZHDMI_ERROR_CHECK(ZHDMI_init(drawing_mode, ZHDMI_LANDSCAPE_ORIENTATION, TAN));

  ZHDMI_image image_to_display;
  ZHDMI_image image_to_display_2;

  image_to_display = lvgl_image_to_ZHDMI(&dark_souls_aura_farm, 0, 0);
  image_to_display_2 = lvgl_image_to_ZHDMI(&doggo, 0, 0);

  // ZHDMI_ORIENTATION orientation = 0;
  ZHDMI_set_background_colour(0x181818);
  while (1) {

    if (drawing_mode != ZHDMI_DRAW_INCREMENTAL) {
      u32 t1, t2;
      XScuTimer_RestartTimer(&TimerInstance);
      TIME_SECTION(t1, t2, ZHDMI_draw_background());
      printf("Time to fill framebuffer: %.3f us\n", (double)(t1 - t2) /
      333.0);
    }

    u32 t1, t2;

    TIME_SECTION(
        t1, t2,
        ZHDMI_ERROR_CHECK(ZHDMI_draw_image(
            (ZHDMI_pixel_coordinate){.x = 0, .y = 0}, &image_to_display)));

    TIME_SECTION(
        t1, t2,
        ZHDMI_ERROR_CHECK(ZHDMI_draw_image(
            (ZHDMI_pixel_coordinate){.x = 10, .y = 10}, &image_to_display_2)));

    // ZHDMI_draw_vline(0, 0, 1079, RED);
    // ZHDMI_draw_vline(1919, 0, 1079, GREEN);

    // ZHDMI_draw_hline(0, 0, 1919, BLUE);
    // ZHDMI_draw_hline(1079, 0, 1919, PURPLE);

    // ZHDMI_draw_filled_rectangle_xy(1, 1, 1919, 1079, 1, RED, 0x181818);
    // ZHDMI_draw_hline(0, 0, 1919, BLUE);

    // ZHDMI_ERROR_CHECK(ZHDMI_set_orientation(orientation));

    ZHDMI_ERROR_CHECK(ZHDMI_print_aligned_string_on_background(
        "HELLO THERE\nI am some text", 30, ZHDMI_ALIGN_CENTER, WHITE, BLACK,
        &printf_font));

    XScuTimer_RestartTimer(&TimerInstance);

    ZHDMI_ERROR_CHECK(
        ZHDMI_draw_filled_triangle_xy(20, 550, 100, 700, 500, 1000, RED,
        BLUE));

    ZHDMI_ERROR_CHECK(
        ZHDMI_draw_filled_circle_xy(48, 800, 123, PURPLE, YELLOW));
    ZHDMI_ERROR_CHECK(
        ZHDMI_draw_unfilled_rectangle_xy(500, 600, 120, 72, 8, LIME));

    printf("Time to place image into buffer: %.3f us\n",
           (double)(t1 - t2) / 333.0);
    XScuTimer_RestartTimer(&TimerInstance);
    TIME_SECTION(t1, t2, ZHDMI_ERROR_CHECK(ZHDMI_refresh_display()));

    printf("Time to update display: %.3f us\n", (double)(t1 - t2) / 333.0);
    msleep(3000);

    // switch (orientation) {
    // case ZHDMI_LANDSCAPE_ORIENTATION:
    //   orientation = ZHDMI_INVERTED_PORTRAIT_ORIENTATION;
    //   break;
    // case ZHDMI_INVERTED_PORTRAIT_ORIENTATION:
    //   orientation = ZHDMI_INVERTED_LANDSCAPE_ORIENTATION;
    //   break;
    // case ZHDMI_INVERTED_LANDSCAPE_ORIENTATION:
    //   orientation = ZHDMI_PORTRAIT_ORIENTATION;
    //   break;
    // case ZHDMI_PORTRAIT_ORIENTATION:
    //   orientation = ZHDMI_LANDSCAPE_ORIENTATION;
    //   break;
    // default:
    //   orientation = ZHDMI_UNKNOWN_ORIENTATION;
    //   break;
    // }
  }
  return 0;
}

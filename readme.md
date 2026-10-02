# Zynq HDMI Image Processing

Hardware-accelerated real-time image processing on a Xilinx Zynq-7020 FPGA, with a software-controlled framebuffer and graphics library.

This project implements a 1080p HDMI video pipeline on the Smart Zynq SP development board. Video frames are stored in DDR memory and transferred to the programmable logic using AXI VDMA, where a custom RTL image-processing pipeline applies configurable pixel transformations before the processed stream is sent to an HDMI output.

The project combines a bare-metal C graphics library running on the Zynq Processing System (PS) with custom Verilog image-processing hardware in the Programmable Logic (PL).

The project is an extension of two earlier projects:

- [ZLCD graphics](https://github.com/msergo4314/ZLCD_graphics)
- [Zynq HDMI Framebuffer](https://github.com/msergo4314/Zynq-HDMI-Framebuffer)

The graphics API from the LCD project was adapted to operate on DDR-backed VDMA framebuffers rather than an SPI-connected display, while the HDMI framebuffer architecture was extended with a hardware image-processing stage.

## Features

- 1920×1080 HDMI output
- Real-time hardware image processing at the pixel clock rate (148 Mhz for 1080p)
- Configurable image transformations implemented in FPGA logic
- 3×3 convolution-based spatial filtering
- Line-buffered image processing using FPGA block RAM
- DSP-based parallel arithmetic for convolution operations
- AXI VDMA framebuffer management
- Triple-buffered framebuffer rendering
- Bare-metal C graphics library for drawing directly into the framebuffer
- Basic text rendering using LVGL fonts
- Multiple framebuffer rendering modes to accomodate both full frames and small updates
- Landscape, inverted landsape, portrait, and inverted portrait display orientations
- Software-controlled filter selection at runtime
- PS/PL integration using AXI peripherals
- Hardware-accelerated image processing combined with software-rendered graphics

## Hardware and Software

### Hardware Needed

- **Development board:** Smart Zynq SP [(See here)](http://www.hellofpga.com/index.php/2023/04/27/smart-zynq-sp/) or another board based on `xc7z020clg484-1`
- **Video output:** HDMI monitor with cable

## Design Goals

The main purpose of the project was to explore the division of work between the Zynq Processing System and Programmable Logic when implementing a real-time graphics and image-processing application.

The project was designed around several goals:

1. Build a complete HDMI video pipeline from framebuffer memory to physical HDMI output.
2. Use the Zynq PS for relatively high-level tasks such as graphics rendering, framebuffer management, and user input.
3. Move computationally intensive per-pixel image processing into custom FPGA hardware.
4. Explore streaming image processing and line-buffer architectures.
5. Learn how FPGA memory resources and DSP blocks can be used to implement parallel image-processing operations.
6. Develop a reusable graphics API rather than tying the application directly to the underlying VDMA implementation.
7. Experiment with the practical timing and resource constraints involved in processing 1080p video.

## Design Overview

The project is divided into two major parts:

### 1: Processing System

The Zynq Processing System runs a bare-metal C application responsible for:

- Initializing the hardware peripherals
- Managing the VDMA framebuffers
- Rendering graphics and text
- Handling user input
- Selecting the active image transformation via AXI GPIO writes
- Updating framebuffer contents

The PS does not perform the per-pixel filtering operation itself. Instead, it prepares the VDMA framebuffer and flushes it at the appropriate time so it can be processed in hardware.

### 2: Programmable Logic

The PL contains the real-time video pipeline.

The framebuffer is read from DDR memory through AXI VDMA and converted into an AXI4-Stream video signal. This allows for the CPU to control the contents of the display with minimal overhead (DMA allows the memory contents to be read directly instead of wasting clock cycles on constant transfers). The pixel stream is passed through the custom image-processing module and then sent to the RGB2DVI HDMI output circuitry to be encoded as TMDS for HDMI output.

Because the image-processing logic operates directly on the streaming pixel data, an entire frame does not need to be processed by the CPU before being displayed.

## General Pipeline

The high-level video path is:

```text
DDR Framebuffer
      │
      ▼
   AXI VDMA
      │
      ▼
AXI4-Stream Video
      │
      ▼
Video Timing / Stream Interface
      │
      ▼
Pixel Transform
      │
      ▼
   RGB2DVI
      │
      ▼
     HDMI
```

The video timing is generated for 1080p output with a video timing generator IP block. The VDMA reads 32-bit framebuffer entries from DDR memory and produces the video stream consumed by the programmable logic.

The framebuffer uses 32-bit pixels even though the image-processing pipeline operates on 24-bit RGB data. This simplifies the software interface and VDMA configuration while providing a convenient word-aligned framebuffer representation (pixels can be treated as uint32_t instead of a raw byte array).

The unused portion of the 32-bit pixel is discarded (sliced) when the pixel enters the axi4 to video out block.

A pushbutton connected to the PS is used to cycle through the available transformations. The PS debounces the button and updates the transformation-selection GPIO output accordingly. When a new frame is rendered,
the transformation selection is read to configure various matrices and weights before the pixels come in.

## Image filtering overview

Most of the image transformations are based on applying an operation independently to each incoming pixel or on performing a 3×3 spatial convolution around the current pixel.

For a convolution filter on an input pixel p11, the output pixel is calculated from the surrounding 3×3 neighbourhood:

```text
P00 P01 P02
P10 P11 P12
P20 P21 P22
```

Each pixel channel is multiplied by the corresponding kernel coefficient and the results are accumulated. The result for each channel is then divided by a constant and assigned to the corresponding output channel.

For example, the convolution kernel for a blur is given by:

```text
[1, 1, 1]
[1, 1, 1]
[1, 1, 1]
```

with a division value of 9. This has the effect of averaging the current pixel with all of its neighbors.

Some transformations (like greyscale) do not depend on nearby pixels (only the current pixel), so they instead use an identity matrix:

```text
[0, 0, 0]
[0, 1, 0]
[0, 0, 0]
```

Though the multiply and accumulate operation is essential for any filter, it is also useful to have more parameters. For example, the formula for greyscale is given by:

Grey_pixel = (x, x, x) where x = (0.3R + 0.587G + 0.115B)

Since each channel is the same in this case, it is necessary to describe each channel of the output pixel as a weighted sum of all the other channels, so an additional matrix operation is added to account for this.

Constant channel offsets are also added for filters like a simple brightness increase/decrease and colour inversion.

The resulting values are then scaled and clamped as appropriate for the 8-bit output channels. An overview can be found [on wikipedia](https://en.wikipedia.org/wiki/Kernel_(image_processing))

## Edge Handling

A convolution requires pixels outside the active image at the boundaries. This implementation extends the edge pixels rather than introducing an additional colour or treating out-of-bounds pixels as zero.

For example, if the first row starts with pixels A and B and the second row has pixels C and D, the first pixel (A) has the following kernel:

```text
A A B
A A B
C C D
```

Note that since pixel A has no neighbor to the left or above, A is extended to the left and the top row is a duplicate of the current row. This is true for all pixels that form the border of the image and allows the convolution pipeline to process the first and last pixels without requiring special cases in the arithmetic pipeline.

## Filters

The project currently demonstrates several transformations, including:

- Unfiltered/original image
- Grayscale
- Inversion
- Box blur
- Laplacian edge detection
- Sharpening
- Horizontal edge detection
- Vertical edge detection

The convolution-based filters are implemented using configurable kernel coefficients in addition to colour channel contribution matrices and scalar offsets

Note that the edge detection filters use a greyscale conversion at the end to make the output less visually noisy.

## Graphics library

The ZHDMI graphics library provides a software abstraction over the HDMI framebuffer.

It was originally derived from the graphics functionality developed for the ZLCD project, but was adapted for a memory-backed HDMI framebuffer.

Instead of transmitting individual pixels over SPI, drawing operations modify a framebuffer in DDR memory. The VDMA subsequently reads the completed framebuffer and supplies it to the video pipeline.

The library features two drawing modes: a full frame rendering mode and an incremental update mode. This is because some applications require a completely new frame (for example, video streaming) but others may want to preserve the framebuffer in between frames (a user interface where only some regions are modified between frames). Since the incremental mode involves copying the contents of a framebuffer when updating, it is significantly slower for full frames, but the performance cost can still be preferable to redrawing every element.

### Drawing Functions

The library provides primitives for common 2D graphics operations, including:

- Individual pixels
- Lines
- Arbitrary lines
- Rectangles
- Circles
- Triangles
- Text strings
- Images
- Background colours

Coordinates are validated against the current display dimensions before being written to the framebuffer.

The library also supports different display orientations by converting user-facing coordinates into the landscape-oriented memory layout used internally.

Text rendering uses font bitmap and glyph-descriptor data generated using LVGL-compatible font structures.

The built-in font data for strings is stored directly in the application rather than being loaded from a filesystem at runtime. however, fonts can still be loaded at any time, as long as they are first converted into C arrays with the LVGL font converter tool.

### Triple Buffering

The graphics library uses three framebuffer regions in DDR memory.

At any given time, the buffers have three logical states:

```text
PARKED -- currently displayed and unavialable to draw in
USING -- currently being modified by the CPU
FREE -- available for future rendering
```

After a refresh, the roles rotate:

```text
Current USING → PARKED
Current PARKED → FREE
Current FREE   → USING
```

The implementation waits for the VDMA to finish using the currently displayed buffer before changing the buffer assignments, which prevents the CPU from modifying a framebuffer while the video hardware is simultaneously reading it.

## Image Processing Architecture

The image-processing pipeline operates on the streaming video rather than on framebuffer memory.

A spatial 3×3 filter requires pixels from three consecutive image rows. The design therefore uses 3 line buffers to retain previously received rows.
Horizontal shift registers then construct the three-pixel-wide window.

### Line Buffer design

An initial implementation attempted to read three adjacent pixels from a single line-buffer memory on every clock. While this is a simple solution conceptually, it made block-RAM inference difficult because the requested memory behaviour required multiple independent reads from the same memory during one clock cycle.

The final architecture instead reads one pixel from each line buffer per clock and constructs the horizontal window using shift registers. The BRAM reads require one cycle to produce an output.

### Image processing pipeline

image processing is implemented as follows:

- input timing is delayed by two lines of stream data to accomodate two line buffers being ready for the first row (normally three rows are needed, but row 0 has no row above it and can be duplicated into the top of the kernel)
- input pixel data is stored into the line buffers as it streams in
- line buffers are read based on the delayed display enable signal which ensures the buffers are filled by the time the first output pixel is produced.

- stage 1: one cycle delay to allow the read result of the line buffer to be ready
- stage 2: one cycle delay to register the line buffer output into the side of the kernel
- stage 3: one cylce of delay to shift the current pixel from the right of the kernel to the center. The previous pixel will have been shifted to the left at this point, except in the case of the first pixel of a row, but in that case the left side of the kernel gets a copy of the middle anyway.
- stage 4: the kernel values are determined and set based on the row count and pixel count (edge pixels require duplication to avoid using stale/incorrect data)
- stage 5: individual colour channels are extracted from each pixel in the kernel
- stage 6: the kernel is multiplied by the weights matrix for the desired filter
- stage 7: each channel of each pixel is multiplied by the corresponding weight of the filter matrix
- stages 8-10: adder tree is used to sum the multiplied channels
- stage 11-12: each colour channel is computed as a linear combination of the three colour channels (multiply then sum values)
- stage 13: constant offsets are added to each colour channel
- stage 14: each colour channel is divided by the kernel division value
- stage 15: colour channels are signed or unsigned depending on the filter
- stage 16: colour channel values are clamped to the range (0-255)
- stage 17: colour channels are packed into the output pixel as (r/b/g) along with delayed timing signals

## Design implementation

### Software

- [zhdmi.c](/src/zhdmi.c)
Main implementation of the ZHDMI graphics library.

Responsibilities include:

VDMA initialization
Framebuffer allocation
Triple-buffer management
Pixel operations
Line and shape drawing
Text rendering
Image rendering
Display orientation
Framebuffer refresh
Dirty-region tracking
Display state management

The library maintains the current framebuffer state internally and exposes a higher-level drawing API to the application.

- [zhdmi.h](/src/zhdmi.h)

Public interface for the graphics library.

It defines the data structures, enumerations, configuration values, and function prototypes required by applications using the ZHDMI library.

- [lvgl_compat.c](/src/lvgl_compat.c)

A simple LVGL compatibility layer for fonts and images that are generated using the [LVGL online converter](https://lvgl.io/tools/imageconverter)

- [main.c](/src/main.c)

loads the VDMA frame buffers with images and shapes to test filter functionality

### FPGA RTL

- [pixel_transform.v](/src/pixel_transform.v)

Custom image-processing pipeline.

This module receives the RGB video stream and applies the selected image transformation before forwarding the processed pixels to the HDMI output pipeline.

- [line_buffer.v](/src/line_buffer.v)
A simple array inferred as block ram that stores and reads a row of pixel data from the input stream

### Hardware design

The block diagram for the hardware configuration can be seen here:

![block design](/ZYNQ_CORE.pdf)

## Performance

The video pipeline is designed to process one pixel per pixel-clock cycle after pipeline filling. The initail fill takes ~4400 clock cycles, but this is a one-time cost. Once the pipeline is filled, an output pixel will arrive every clock cylce.

Software rendering performance depends on the amount of framebuffer data modified by the application. Drawing operations that modify relatively small portions of the framebuffer can avoid rewriting the entire display.

Compiler optimization also has a significant effect on software rendering performance. The rendering times are as follows:

Frame mode with no optimizations:

![Optimization_0_FRAME_MODE](/images/Optimization_0_FRAME_MODE.png)

Frame mode with o1 optimizations:

![Optimization_1_FRAME_MODE](/images/Optimization_1_FRAME_MODE.png)

Incremental mode with no optimizations:

![Optimization_0_INCREMENTAL_MODE](/images/Optimization_0_INCREMENTAL_MODE.png)

Incremental mode with o1 optimizations:

![Optimization_1_INCREMENTAL_MODE](/images/Optimization_1_INCREMENTAL_MODE.png)

In general, the incremental drawing mode requires ~30 ms to perform a memcpy operation, so refreshing the display always takes at least that long, plus the time to flush the pixels that have updated since the last frame refresh (up to 13 ms for a full frame). Using the full frame rendering mode avoids the expensive memcpy and always flushes the full frame, so pushing a complete frame takes ~13 ms. As can be seen, the drawing times depend heavily on optimization level, so optimizations should be enabled at at least the o1 level. This reduces frame buffer writes by > 75% regardless of the mode used.

## Results

The following results are obtained by toggling the push button after loading a frame into memory using the graphics libary. In this case, there are two images, some simple text, and some shapes.

### Unfiltered Image

This is the unmodified output of the VDMA controller. The monitor menu confirms the resolution and timing
![unfiltered_image](/images/unfiltered.jpg)

### Grayscale transform

![grayscale_image](/images/grayscale.jpg)

### Inversion transform

![inverted_image](/images/inverted.jpg)

### Laplacian edge detection

![laplacian_edge_detect_image](/images/laplacian_edge.jpg)

### Box blur filter

![box_blur_image](/images/box_blur.jpg)

### Sharpen filter

![sharpened_image](/images/sharpened.jpg)

### Horizontal edge detection

![hor_edge_image](/images/hor_edge.jpg)

### Vertical edge detection

![ver_edge_image](/images/ver_edge.jpg)

## Building the project

## Resource Utilization

![resource_utilization](/images/resource_utilization.png)

Since the design was able to infer BRAM for the line buffers and DSPs for the multiply and accumualte operations, resource utilization remains relatively low for LUTs, LUTRAM, and FFs. When BRAM was not inferred for the line buffers, almost all of the LUTRAM was wasted implementing them.

### Vivado

1. Open Vivado 2025.x
2. Create a project targeting XC7Z020 (clg484 - 1)
3. Add the RTL sources from /src.
4. Add the XDC constraints.
5. Recreate or import the Vivado block design.
6. Validate the block design.
7. Generate the HDL wrapper.
8. Generate the bitstream.
9. Export the generated hardware platform for use with Vitis.

### Vitis

1. Export the hardware platform from Vivado.
2. Create a standalone Vitis application.
3. Add the C source files and graphics library.
4. Add any generated image/font data required by the application.
5. Build the application.
6. Program the FPGA and launch the application.

## Lessons learned and development issues

This project involved several practical FPGA design issues that were not apparent from smaller RTL exercises.

### FPGA Memory Inference

The way a memory is accessed has a significant effect on the hardware that Vivado can infer.

Attempting to obtain multiple adjacent pixels from a single line-buffer memory made block-RAM inference impossible to achieve. Changing the architecture to read one pixel per cycle and construct the 3×3 window using shift registers resulted in a much more resource-efficient BRAM implementation. With multiple reads per clock cycle, the design implemented the line buffers with LUTs and Flip flops, wasting most of the available resources.

### DSP Inference

The convolution requires a large number of parallel multiplications. Explicitly encouraging Vivado to map these operations onto DSP48 resources substantially improved the timing of the multiplication stages.
This was preferable to attempting to replace the arbitrary kernel multiplications with shift/add logic, since the kernel coefficients are configurable.

### Pipeline Design

At a 148.5 MHz pixel clock, every stage has approximately 6.73 ns in which to complete its work.

Large arithmetic operations therefore need to be distributed across multiple pipeline stages. Registering intermediate results makes it possible to maintain one-pixel-per-clock throughput while allowing each arithmetic stage to meet its individual timing requirement.

### Division Latency

A conventional division operation by a constant introduced significant timing delay (in particular, division by 9 for the blur filter).

The division was replaced with a fixed-point reciprocal multiplication approach for the applicable filter operation, allowing the operation to be implemented using FPGA-friendly arithmetic. Other filters used powers of 2 for the division (such as 256) or 1, so in thoses cases the division could be simplified to an arithmetic right shift or nothing in the case of a division value of 1.

### Timing

The image-processing datapath was optimized until the major computational timing bottlenecks were addressed.

The remaining timing warnings are associated primarily with peripheral portions of the design rather than the core filtering datapath.

#### AXI GPIO Clock-Domain Crossing

The filter configuration is controlled through AXI GPIO running in a separate clock domain from the 148.5 MHz pixel-processing pipeline.

The current implementation therefore has a clock-domain crossing between the AXI clock and pixel clock.

A more robust implementation could replace this with a proper configuration handshake or synchronized update mechanism. Since the configuration changes infrequently and the filtering pipeline itself is functionally verified, this was left as a documented limitation.

#### RGB2DVI Serial Clock

The RGB2DVI output pipeline generates a 5× serial clock for HDMI output.

At 148.5 MHz pixel clock this produces a 742.5 MHz serial clock.

The generated serial clock produces minimum-period violations in portions of the RGB2DVI clock/serializer infrastructure.

An alternative module may be necessary for converting the serial output to HDMI output.

## future work

Several extensions could be natural next steps:

- Videos could be decoded and streamed through VDMA for real-time filtering. Reading video files is easy to do with an SD card, but decoding the frames to fill the framebuffers is non-trivial.

The current pipeline processes frames from RAM. A future version could accept a live video source and process it directly in hardware.

Potential sources include:

- Camera input
- Video decoder IP
- SD-card video playback
- SD Card Image/Video Loading

Improved Configuration Interface

The GPIO-based filter selection could be replaced with a more structured AXI-Lite configuration interface.

This would allow the PS to modify:

- Filter selection
- Kernel coefficients
- Channel scaling
- Other image-processing parameters

A configuration handshake could then safely transfer updated parameters into the pixel-processing clock domain.

### Improved HDMI Clocking

The RGB2DVI clocking architecture could be revisited to eliminate the remaining high-speed serializer timing violations on the target device.

### Additional Filters

The configurable convolution architecture could support additional filters without substantially changing the surrounding video pipeline. These would be very easy to add to the RTL code since the structure is already implemented and highly modular

Possible additions include:

- Gaussian blur
- Emboss
- Custom user-defined kernels
- Sobel operators
- Prewitt operators
- Colour-channel transformations

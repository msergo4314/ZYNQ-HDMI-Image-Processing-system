`default_nettype none
`timescale 1ns / 1ps

//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 09/02/2026 02:53:58 PM
// Design Name: 
// Module Name: pixel_transform
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module pixel_transform#(parameter WIDTH = 1920,
                        parameter HEIGHT = 1080,
                        parameter LINE_PERIOD = 2200)(
                        
    input wire [23:0] rgb_pixel_in, // format: RR GG BB 
    input wire pixel_clock_in, // pixel clock (148 Mhz)
    input wire pixel_de_in, // display enable / valid pixel signal
    input wire v_sync_in,
    input wire h_sync_in,
    input wire [4:0] transform_select, // 0 is off, 1 is grayscale, 2 is invert, 3 is blur, 4 is edge detect, etc.
    input wire reset_in_n, // active low reset
    
    // IMPORTANT: RGB to DVI block expects R/B/G NOT R/G/B
    output reg [23:0] rbg_pixel_out, // final output pixel
    output reg pixel_de_out,
    output reg v_sync_out,
    output reg h_sync_out
    );
    
    // delay the timing signals as needed to fill buffers
    // subtract 1 because the registered outputs take 1 cyle to read
    localparam INITIAL_DELAY = (2 * LINE_PERIOD) - 1;
    
    
    localparam WIDTH_BITS = $clog2(WIDTH - 1);
    localparam HEIGHT_BITS = $clog2(HEIGHT - 1);
    localparam LINE_BITS = $clog2(LINE_PERIOD - 1);
    localparam CONTORL_BITS = $clog2(INITIAL_DELAY);
    
    localparam NO_TRANSFORM = 0;
    localparam GRAYSCALE_TRANSFORM = 1;
    localparam INVERSION_TRANSFORM = 2;
    localparam EDGE_DETECTION_TRANSFORM = 3;
    localparam BLUR_TRANSFORM = 4;
    localparam SHARPEN_TRANSFORM = 5;
    localparam HOR_EDGE_DETECT_TRANSFORM = 6;
    localparam VER_EDGE_DETECT_TRANSFORM = 7;
    
    reg [WIDTH_BITS - 1: 0] input_pixel_count;
    
    reg [WIDTH_BITS - 1: 0] output_pixel_count;
    reg [HEIGHT_BITS - 1: 0] output_row_count;
    
    // Note: GPIO inputs come from a different clock domain (CDC timing violation)
    
    // left to right order: DE, H_SYNC, V_SYNC
    reg [2:0] control_signals_delayed_pipe[0:14];
    
    reg signed [7:0] weights_matrix[0:8]; // Descibes how each pixel in the centered 3x3 window affects the output
    reg signed [9:0] division_value; // divide each channel sum by this value
    // NOTE: inner dimension is channel, outer dimension is weights for channel
    reg signed [8:0] colour_channel_matrix [0:2][0:2]; // scale r/g/b by these values for resulting r/g/b
    reg signed [8:0] colour_channel_offset  [0:2]; // signed scalar offset for each colour channel 
    
    (* ram_style = "block" *)
    reg [2:0] control_signals_delayed [0:INITIAL_DELAY]; // add one element so read/write pointers are never the same
    
    reg [CONTORL_BITS - 1 : 0] control_write_index = 0; // counts from 0 to 2201, starts at 0
    reg [CONTORL_BITS - 1 : 0] control_read_index = 1; // counts from 0 to 2201, starts at 1
    
    reg [23:0] kernel_pixels[0:8]; // pixels used for filter operations
    
    // idices of the three arrays
    reg [1:0] buffer_to_fill; // buffer to write current pixel to
    
    reg [1:0] oldest_buffer; // row y - 1 of window
    reg [1:0] middle_buffer; // row y of window
    reg [1:0] most_recent_buffer; // row y + 1 of window
    
    reg v_sync_delayed_last;
    reg use_absolute_value;
    
    integer i; // loop variable
    reg delayed_de, delayed_v_sync, delayed_h_sync;
    
    wire bram_write_enables [0:3];
    wire bram_read_enables [0:3];
    
    genvar j;
    for (j = 0; j <= 3; j = j + 1) begin
        assign bram_read_enables [j] = (buffer_to_fill != j) & delayed_de;
        assign bram_write_enables [j] = (buffer_to_fill == j) & pixel_de_in;
    end
    
    wire [23:0] line_buffer_outputs[0:3];
    // 4 line buffers
    generate 
        for (j = 0; j <= 3; j = j + 1) begin : line_buffers
            line_buffer #(.WIDTH(WIDTH))
            line_buffer (
            .clk_in(pixel_clock_in),
            .reset_in_n(reset_in_n),
            .input_pixel(rgb_pixel_in), // input pixel is 24 bits
            .write_enable(bram_write_enables[j]),
            .read_enable(bram_read_enables[j]),
            .read_address(output_pixel_count),
            .write_address(input_pixel_count),
            .pixel_out(line_buffer_outputs[j])
            );
        end
    endgenerate
    
    // latch in control signals with fixed delay
    always@(posedge pixel_clock_in) begin
        if (~reset_in_n) begin
            control_write_index  <= 0;
            control_read_index  <= 1;
            
            delayed_de <= 0;
            delayed_v_sync <= 0;
            delayed_h_sync <= 0;
            v_sync_delayed_last <= 0;
        end
        else begin
            if (control_write_index  == INITIAL_DELAY)
                control_write_index  <= 0;
            else 
                control_write_index  <= control_write_index  + 1;
                
                
            v_sync_delayed_last <= delayed_v_sync;
            // always store packed control signals
            control_signals_delayed[control_write_index] <= {pixel_de_in, h_sync_in, v_sync_in};
            {delayed_de, delayed_h_sync, delayed_v_sync} <= control_signals_delayed[control_read_index];
            
            if (control_read_index == INITIAL_DELAY)
                // roll over to 0
                control_read_index <= 0;
            else
                control_read_index <= control_read_index + 1;
        end 
    end

    // input pixel counter and row counters
    always@(posedge pixel_clock_in) begin
        if (~reset_in_n) begin
                input_pixel_count <= 'b0;
                buffer_to_fill <= 2'b0;
        end
        else if (pixel_de_in && input_pixel_count < (WIDTH - 1)) begin
            input_pixel_count <= input_pixel_count + 1;
        end
        else if (pixel_de_in) begin
            input_pixel_count <= 0;
            // increment the buffer to fill
            if (buffer_to_fill < 3)
                buffer_to_fill <= buffer_to_fill + 1;
            else 
                buffer_to_fill <= 0;
        end
    end
    
    always@(posedge pixel_clock_in) begin
        if (~reset_in_n) begin
            oldest_buffer <= 3;
            middle_buffer <= 0;
            most_recent_buffer <= 1;
            
            output_pixel_count <= 0;
            output_row_count <= 0;
        end
        else if (delayed_de && output_pixel_count < (WIDTH - 1)) begin
            output_pixel_count <= output_pixel_count + 1;
        end
        else if (delayed_de) begin
            output_pixel_count <= 0;
            if (output_row_count == (HEIGHT - 1)) begin
                output_row_count <= 0;
            end
            else begin
                output_row_count <= output_row_count + 1;
            end
            oldest_buffer <= middle_buffer;
            middle_buffer <= most_recent_buffer;
            
            if (most_recent_buffer < 3)
                most_recent_buffer <= most_recent_buffer + 1;
            else
                most_recent_buffer <= 0;
        end
    end
    
    //wire read_data_available = control_signals_delayed_pipe[0][2];
    reg [HEIGHT_BITS - 1: 0] output_row_count_delayed[0:2];
    reg [WIDTH_BITS - 1: 0] output_pixel_count_delayed[0:2];
    
    // ordering: 0 is old, 1 is middle, 2 is most recent (inner dimension)
    reg [1:0] delayed_buffers[0:2];
    
    // idices 0/1/2 correspond to top/middle/bottom of kernel
    reg [23:0] kernel_pixel_left [0:2]; // left pixels of 3x3 kernel
    reg [23:0] kernel_pixel_middle [0:2]; // middle pixels
    reg [23:0] kernel_pixel_right [0:2]; // right pixels
    
    // stages 1-3 of pipeline: use line buffer outputs to form shift registers for the kernel
    
    // delay 3 clock cycles -- 1 for line buffer read operation, 1 to register read result, 1 for shifting to middle position
    always @(posedge pixel_clock_in) begin
        // shift in pixel based on previous cycle's logical buffer roles
        for (i = 0; i <= 2; i = i + 1) begin
            case (delayed_buffers[i])
                0: kernel_pixel_right[i] <= line_buffer_outputs[0];
                1: kernel_pixel_right[i] <= line_buffer_outputs[1];
                2: kernel_pixel_right[i] <= line_buffer_outputs[2];
                3: kernel_pixel_right[i] <= line_buffer_outputs[3];
            endcase
            kernel_pixel_middle[i] <= kernel_pixel_right[i];
            kernel_pixel_left[i] <= kernel_pixel_middle[i];
        end
        
        for (i = 2; i >= 1; i = i - 1) begin
            output_row_count_delayed[i] <= output_row_count_delayed[i - 1];
            output_pixel_count_delayed[i] <= output_pixel_count_delayed[i - 1];
            control_signals_delayed_pipe[i] <= control_signals_delayed_pipe[i - 1];
        end
        
        output_row_count_delayed[0] <= output_row_count;
        output_pixel_count_delayed[0] <= output_pixel_count;
        
        control_signals_delayed_pipe[0] <= {delayed_de, delayed_h_sync, delayed_v_sync};
        {delayed_buffers[0], delayed_buffers[1], delayed_buffers[2]} <= {oldest_buffer, middle_buffer, most_recent_buffer};
    end
    
    // stage 4 of pipeline: construct actual kernel based on row/pixel indices
    always@(posedge pixel_clock_in) begin
        if (output_row_count_delayed[2] == 0) begin
            // if first pixel, left side of matrix gets a copy of the middle column since it is currently junk data
            if (output_pixel_count_delayed[2] == 0) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_middle[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_middle[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_middle[2], kernel_pixel_middle[2], kernel_pixel_right[2]};
            end
            // if last pixel, right side of matrix gets a copy of the middle column
            else if (output_pixel_count_delayed[2] == WIDTH - 1) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_middle[1]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_middle[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[2], kernel_pixel_middle[2], kernel_pixel_middle[2]};
            end
            // general case: top row is a copy of the middle row, but no other changes
            else begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[2], kernel_pixel_middle[2], kernel_pixel_right[2]};
            end
        end
        else if (output_row_count_delayed[2] == HEIGHT - 1) begin
            // if first pixel, left side of matrix gets a copy of the middle column since it is currently junk data
            if (output_pixel_count_delayed[2] == 0) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_middle[0], kernel_pixel_middle[0], kernel_pixel_right[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_middle[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_middle[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
            end
            // if last pixel, right side of matrix gets a copy of the middle column
            else if (output_pixel_count_delayed[2] == WIDTH - 1) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[0], kernel_pixel_middle[0], kernel_pixel_middle[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_middle[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_middle[1]};
            end
            // general case: bottom row is a copy of the middle row, but no other changes
            else begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[0], kernel_pixel_middle[0], kernel_pixel_right[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
            end
        end
        else begin
            if (output_pixel_count_delayed[2] == 0) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_middle[0], kernel_pixel_middle[0], kernel_pixel_right[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_middle[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_middle[2], kernel_pixel_middle[2], kernel_pixel_right[2]};
            end
            else if (output_pixel_count_delayed[2] == WIDTH - 1) begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[0], kernel_pixel_middle[0], kernel_pixel_middle[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_middle[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[2], kernel_pixel_middle[2], kernel_pixel_middle[2]};
            end
            // general assignment for all non-border pixels
            else begin
                {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]} <= {kernel_pixel_left[0], kernel_pixel_middle[0], kernel_pixel_right[0]};
                {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]} <= {kernel_pixel_left[1], kernel_pixel_middle[1], kernel_pixel_right[1]};
                {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]} <= {kernel_pixel_left[2], kernel_pixel_middle[2], kernel_pixel_right[2]};
            end
        end
        control_signals_delayed_pipe[3] <= control_signals_delayed_pipe[2];
    end

  
  // signals for simulation monitoring only
    wire [23:0] kernel_top [0:2];
    wire [23:0] kernel_mid [0:2];
    wire [23:0] kernel_bottom [0:2];
    
    assign {kernel_top[0], kernel_top[1], kernel_top[2]} = {kernel_pixels[0], kernel_pixels[1], kernel_pixels[2]};
    assign {kernel_mid[0], kernel_mid[1], kernel_mid[2]} = {kernel_pixels[3], kernel_pixels[4], kernel_pixels[5]};
    assign {kernel_bottom[0], kernel_bottom[1], kernel_bottom[2]} = {kernel_pixels[6], kernel_pixels[7], kernel_pixels[8]};

    // individual colour channels for each pixel in the 3x3 window
    reg [7:0] r [0:8];
    reg [7:0] g [0:8];
    reg [7:0] b [0:8];
    
    // stage 4 of pipeline: extract colour channels from matrix
    always@(posedge pixel_clock_in) begin
        for (i = 0; i <= 8; i = i + 1) begin
            r[i] <= kernel_pixels[i][23:16];
            g[i] <= kernel_pixels[i][15:8];
            b[i] <= kernel_pixels[i][7:0];
        end
        
        control_signals_delayed_pipe[4] <= control_signals_delayed_pipe[3];
    end
    
    // product is result of 8 bit x 8 bit values
    reg signed [16:0] multiplied_channels[0:2][0:8]; // pixels used for multiply/accumulate
    
    // combinational multiply to infer DSPs
    always@(*) begin
        (* use_dsp = "yes" *)
        for (i = 0; i <= 8; i = i + 1) begin
            multiplied_channels[0][i] = $signed(weights_matrix[i]) * $signed({1'b0, r[i]});
            multiplied_channels[1][i] = $signed(weights_matrix[i]) * $signed({1'b0, g[i]});
            multiplied_channels[2][i] = $signed(weights_matrix[i]) * $signed({1'b0, b[i]});
        end
    end
    
    reg signed [16:0] multiplied_channels_reg[0:2][0:8]; // pixels used for multiply/accumulate
    
    // stage 5 of pipeline: register multiplication outputs
    always@(posedge pixel_clock_in) begin
        for (i = 0; i <= 8; i = i + 1) begin
            multiplied_channels_reg[0][i] <= multiplied_channels[0][i];
            multiplied_channels_reg[1][i] <= multiplied_channels[1][i];
            multiplied_channels_reg[2][i] <= multiplied_channels[2][i];
        end
        control_signals_delayed_pipe[5] <= control_signals_delayed_pipe[4];
    end
    
    /* PER CHANNEL PIXEL SUM IS PIPELINED AS FOLLOWS (3 stages): */
    
    //P0 ─┐
    //P1 ─┴─> A0 ─┐
    //P2 ─┐       │
    //P3 ─┴─> A1 ─┤
    //            ├─> B0 ─┐
    //P4 ─┐       │       │
    //P5 ─┴─> A2 ─┘       │
    //                    ├─> C0 ──> SUM
    //P6 ─┐               │
    //P7 ─┴─> A3 ─┐       │
    //P8 ─────────┴─> B1 ─┘
    
    // NOTE: Pixel 8 is registered for stage 2 (B)
    
    // stage 6 of pipeline: sum each colour channel (A)
    reg signed [19:0] summed_channels_stage_A [0:2][0:3];
    
    reg signed [19:0] pixel_8_pipe[0:2];
    
    always @(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            summed_channels_stage_A[i][0] <= multiplied_channels_reg[i][0] + multiplied_channels_reg[i][1];
            summed_channels_stage_A[i][1] <= multiplied_channels_reg[i][2] + multiplied_channels_reg[i][3];
            summed_channels_stage_A[i][2] <= multiplied_channels_reg[i][4] + multiplied_channels_reg[i][5];
            summed_channels_stage_A[i][3] <= multiplied_channels_reg[i][6] + multiplied_channels_reg[i][7];
            
            // propegate last value into next stage of pipeline
            pixel_8_pipe[i] <= multiplied_channels_reg[i][8];
        end 
    
        control_signals_delayed_pipe[6] <= control_signals_delayed_pipe[5];
    end
    
    reg signed [19:0] summed_channels_stage_B [0:2][0:1];
    // stage 7 of pipeline: sum each colour channel (B)
    always @(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            summed_channels_stage_B[i][0] <= summed_channels_stage_A[i][0] + summed_channels_stage_A[i][1] + summed_channels_stage_A[i][2];
            
            summed_channels_stage_B[i][1] <= summed_channels_stage_A[i][3] + pixel_8_pipe[i];
        end 
    
        control_signals_delayed_pipe[7] <= control_signals_delayed_pipe[6];
    end
    
    reg signed [19:0] summed_channels_total[0:2];
    // stage 8 of pipeline: sum each colour channel (C)
    always @(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            summed_channels_total[i] <= summed_channels_stage_B[i][0] + summed_channels_stage_B[i][1];
        end
        control_signals_delayed_pipe[8] <= control_signals_delayed_pipe[7];
       
    end
    
    reg signed [28:0] scaled_colour_channels[0:2][0:2];
    
    // stage 9 of pipeline: scale the total sums by the colour channel matrix (part A - multiply matrices)
    always @(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            scaled_colour_channels[i][0] <= summed_channels_total[0] * colour_channel_matrix[i][0];
            scaled_colour_channels[i][1] <= summed_channels_total[1] * colour_channel_matrix[i][1];
            scaled_colour_channels[i][2] <= summed_channels_total[2] * colour_channel_matrix[i][2];
        end 
    
        control_signals_delayed_pipe[9] <= control_signals_delayed_pipe[8];
    end
    
    reg signed [29:0] scaled_and_added_colour_channels[0:2];
    // stage 10 of pipeline: scale the total sums by the colour channel matrix (part B - sum channels)
    always @(posedge pixel_clock_in)begin
        for (i = 0; i <= 2; i = i + 1) begin
            scaled_and_added_colour_channels[i] <= scaled_colour_channels[i][0] + scaled_colour_channels[i][1] + scaled_colour_channels[i][2];
        end 
    
        control_signals_delayed_pipe[10] <= control_signals_delayed_pipe[9];
    end
    
    
    reg signed [31:0] offset_colour_channels[0:2];
    // stage 11 of pipeline: add constant channel offsets
    always @(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            offset_colour_channels[i] <= scaled_and_added_colour_channels[i] + colour_channel_offset[i];
        end 
    
        control_signals_delayed_pipe[11] <= control_signals_delayed_pipe[10];
    end
    
    reg signed [31:0] divided_value [0:2];
    
    // stage 12: divide each channel by constant and pack as rgb
    always@(posedge pixel_clock_in) begin
        // all division values should be accounted for here
        case (division_value)
            10'sd256: begin
                // preserve sign with arithmetic shift
                divided_value[0] <= (offset_colour_channels[0]) >>> 8;
                divided_value[1] <= (offset_colour_channels[1]) >>> 8;
                divided_value[2] <= (offset_colour_channels[2]) >>> 8;
            end
            10'sd9: begin
                // since division by 9 is non-trivial, approximate as:
                // x/9  ~= 7282 * x / 65536
                divided_value[0] <= (offset_colour_channels[0] * 16'd7282) >>> 16;
                divided_value[1] <= (offset_colour_channels[1] * 16'd7282) >>> 16;
                divided_value[2] <= (offset_colour_channels[2] * 16'd7282) >>> 16;
            end
            // default to division by 1
            default: begin
                divided_value[0] <= (offset_colour_channels[0]);
                divided_value[1] <= (offset_colour_channels[1]);
                divided_value[2] <= (offset_colour_channels[2]);
            end
        endcase
        
        
        control_signals_delayed_pipe[12] <= control_signals_delayed_pipe[11];
    end

    reg signed [31:0] processed_colour_channel_values[0:2];
    
//    // stage 13: deal with signed values as needed depending on the filter
    always@(posedge pixel_clock_in) begin
    
        for (i = 0; i <= 2; i = i + 1) begin
            if (use_absolute_value) begin
                if (divided_value[i] < 0)
                    processed_colour_channel_values[i] <= -divided_value[i];
                else
                    processed_colour_channel_values[i] <= divided_value[i];
            end
            else begin
                processed_colour_channel_values[i] <= divided_value[i];
            end
        end
        
        
        control_signals_delayed_pipe[13] <= control_signals_delayed_pipe[12];
    end
    
    reg [7:0] final_colour_channel_values [0:2];
    
    // stage 14: clamp values to 0-255;
    always@(posedge pixel_clock_in) begin
        for (i = 0; i <= 2; i = i + 1) begin
            if (processed_colour_channel_values[i] < 0)
                final_colour_channel_values[i] <= 8'd0;
            else if (processed_colour_channel_values[i] > 255)
                final_colour_channel_values[i] <= 8'd255;
            else
                final_colour_channel_values[i] <= processed_colour_channel_values[i][7:0];
        end
        
        control_signals_delayed_pipe[14] <= control_signals_delayed_pipe[13];
    end
    
        // reset on new frame
    always@(posedge pixel_clock_in) begin
        if ((~delayed_v_sync & v_sync_delayed_last) || ~reset_in_n) begin
            case(transform_select)
                default: begin
                // do nothing, pass outputs
                use_absolute_value = 1'b0;
                division_value <= 10'sd1;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd1;
                colour_channel_matrix[0][1] <= 9'sd0;
                colour_channel_matrix[0][2] <= 9'sd0;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd0;
                colour_channel_matrix[1][1] <= 9'sd1;
                colour_channel_matrix[1][2] <= 9'sd0;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd0;
                colour_channel_matrix[2][1] <= 9'sd0;
                colour_channel_matrix[2][2] <= 9'sd1;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd0, 8'd0, 8'd0};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd0, 8'd1, 8'd0};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd0, 8'd0, 8'd0};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
                GRAYSCALE_TRANSFORM: begin
                // grayscale filter
                // grayscale formula: Y = 0.299R + 0.587G + 0.114B (each channel is the same)
                // approximate as Y = (77R + 150G + 29B) / 256
                use_absolute_value = 1'b0;
                division_value <= 10'sd256;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd77;
                colour_channel_matrix[0][1] <= 9'sd150;
                colour_channel_matrix[0][2] <= 9'sd29;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd77;
                colour_channel_matrix[1][1] <= 9'sd150;
                colour_channel_matrix[1][2] <= 9'sd29;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd77;
                colour_channel_matrix[2][1] <= 9'sd150;
                colour_channel_matrix[2][2] <= 9'sd29;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd0, 8'd0, 8'd0};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd0, 8'd1, 8'd0};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd0, 8'd0, 8'd0};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
                INVERSION_TRANSFORM: begin
                // invert
                use_absolute_value = 1'b0;
                division_value <= 10'sd1;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd1;
                colour_channel_matrix[0][1] <= 9'sd0;
                colour_channel_matrix[0][2] <= 9'sd0;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd0;
                colour_channel_matrix[1][1] <= 9'sd1;
                colour_channel_matrix[1][2] <= 9'sd0;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd0;
                colour_channel_matrix[2][1] <= 9'sd0;
                colour_channel_matrix[2][2] <= 9'sd1;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd0, 8'd0, 8'd0};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd0, -8'sd1, 8'd0};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd0, 8'd0, 8'd0};
                
                colour_channel_offset[0] <= 'd255; // red offset
                colour_channel_offset[1] <= 'd255; // green offset
                colour_channel_offset[2] <= 'd255; // blue offset
                end
                BLUR_TRANSFORM: begin
                // blur
                use_absolute_value = 1'b0;
                division_value <= 10'sd9;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd1;
                colour_channel_matrix[0][1] <= 9'sd0;
                colour_channel_matrix[0][2] <= 9'sd0;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd0;
                colour_channel_matrix[1][1] <= 9'sd1;
                colour_channel_matrix[1][2] <= 9'sd0;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd0;
                colour_channel_matrix[2][1] <= 9'sd0;
                colour_channel_matrix[2][2] <= 9'sd1;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd1, 8'd1, 8'd1};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd1, 8'd1, 8'd1};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd1, 8'd1, 8'd1};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
                EDGE_DETECTION_TRANSFORM: begin
                // simple edge detection
                use_absolute_value = 1'b1;
                division_value <= 10'sd256;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd77;
                colour_channel_matrix[0][1] <= 9'sd150;
                colour_channel_matrix[0][2] <= 9'sd29;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd77;
                colour_channel_matrix[1][1] <= 9'sd150;
                colour_channel_matrix[1][2] <= 9'sd29;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd77;
                colour_channel_matrix[2][1] <= 9'sd150;
                colour_channel_matrix[2][2] <= 9'sd29;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd0, 8'd1, 8'd0};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd1, -8'sd4, 8'd1};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd0, 8'd1, 8'd0};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
                SHARPEN_TRANSFORM: begin
                // sharpen
                use_absolute_value = 1'b0;
                division_value <= 10'sd1;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd1;
                colour_channel_matrix[0][1] <= 9'sd0;
                colour_channel_matrix[0][2] <= 9'sd0;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd0;
                colour_channel_matrix[1][1] <= 9'sd1;
                colour_channel_matrix[1][2] <= 9'sd0;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd0;
                colour_channel_matrix[2][1] <= 9'sd0;
                colour_channel_matrix[2][2] <= 9'sd1;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {8'd0, -8'sd1, 8'd0};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {-8'sd1, 8'd5, -8'sd1};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd0, -8'sd1, 8'd0};
                
                colour_channel_offset[0] <= 'sd0; // red offset
                colour_channel_offset[1] <= 'sd0; // green offset
                colour_channel_offset[2] <= 'sd0; // blue offset
                end
                HOR_EDGE_DETECT_TRANSFORM: begin
                // horizontal edge
                use_absolute_value = 1'b1;
                division_value <= 10'sd256;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd77;
                colour_channel_matrix[0][1] <= 9'sd150;
                colour_channel_matrix[0][2] <= 9'sd29;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd77;
                colour_channel_matrix[1][1] <= 9'sd150;
                colour_channel_matrix[1][2] <= 9'sd29;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd77;
                colour_channel_matrix[2][1] <= 9'sd150;
                colour_channel_matrix[2][2] <= 9'sd29;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {-8'sd1, -8'sd2, -8'sd1};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {8'd0, 8'd0, 8'd0};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {8'd1, 8'd2, 8'd1};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
                VER_EDGE_DETECT_TRANSFORM: begin
                // vertical edge
                use_absolute_value = 1'b0;
                division_value <= 10'sd256;
                
                // red channel scaling
                colour_channel_matrix[0][0] <= 9'sd77;
                colour_channel_matrix[0][1] <= 9'sd150;
                colour_channel_matrix[0][2] <= 9'sd29;
                // green channel scaling
                colour_channel_matrix[1][0] <= 9'sd77;
                colour_channel_matrix[1][1] <= 9'sd150;
                colour_channel_matrix[1][2] <= 9'sd29;
                // blue channel scaling
                colour_channel_matrix[2][0] <= 9'sd77;
                colour_channel_matrix[2][1] <= 9'sd150;
                colour_channel_matrix[2][2] <= 9'sd29;
                
                {weights_matrix[0], weights_matrix[1], weights_matrix[2]} <= {-8'sd1, 8'd0, 8'd1};
                {weights_matrix[3], weights_matrix[4], weights_matrix[5]} <= {-8'sd2, 8'd0, 8'd2};
                {weights_matrix[6], weights_matrix[7], weights_matrix[8]} <= {-8'd1, 8'd0, 8'd1};
                
                colour_channel_offset[0] <= 'd0; // red offset
                colour_channel_offset[1] <= 'd0; // green offset
                colour_channel_offset[2] <= 'd0; // blue offset
                end
            endcase
        end
    end
    
    // stage 14: send to output as rbg
    always@(posedge pixel_clock_in) begin
        if (~reset_in_n) begin
            pixel_de_out <= 0;
            h_sync_out <= 0;
            v_sync_out <= 0;
            rbg_pixel_out <= 0;
        end
        else begin
           pixel_de_out <= control_signals_delayed_pipe[14][2];
           h_sync_out <= control_signals_delayed_pipe[14][1];
           v_sync_out <= control_signals_delayed_pipe[14][0];
           
           rbg_pixel_out <= {final_colour_channel_values[0], final_colour_channel_values[2], final_colour_channel_values[1]};
        end
    end
endmodule

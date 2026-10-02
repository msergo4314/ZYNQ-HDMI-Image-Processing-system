`timescale 1ns / 1ps

module pixel_transform_tb;

    reg [23:0] rgb_pixel_in;
    reg        pixel_clock_in;
    reg        pixel_de_in;
    reg        v_sync_in;
    reg        h_sync_in;
    reg [4:0]  transform_select;
    reg        reset_in_n;

    wire [23:0] rbg_pixel_out;
    wire        pixel_de_out;
    wire        v_sync_out;
    wire        h_sync_out;
    
    // expected latency is roughly 2 x the width
    
    localparam simulated_width = 16;
    localparam simualated_height = 4;
    localparam simulated_blanking_period = 10;
    localparam simulated_line_period = simulated_blanking_period  + simulated_width ;
    

    // dramatically reduce frame sizes
    pixel_transform #(.WIDTH(simulated_width),
                      .HEIGHT(simualated_height),
                      .LINE_PERIOD(simulated_line_period))
    uut (
        .rgb_pixel_in(rgb_pixel_in),
        .pixel_clock_in(pixel_clock_in),
        .pixel_de_in(pixel_de_in),
        .v_sync_in(v_sync_in),
        .h_sync_in(h_sync_in),
        .transform_select(transform_select),
        .reset_in_n(reset_in_n),

        .rbg_pixel_out(rbg_pixel_out),
        .pixel_de_out(pixel_de_out),
        .v_sync_out(v_sync_out),
        .h_sync_out(h_sync_out)
    );

    // 148 MHz-ish clock
    // 6.756 ns period
    initial begin
        pixel_clock_in = 0;
        forever #3.378 pixel_clock_in = ~pixel_clock_in;
    end

    integer cycle_count;
    integer input_pulse_cycle;
    integer output_pulse_cycle;

    always @(posedge pixel_clock_in) begin
        cycle_count <= cycle_count + 1;

        if (pixel_de_in)
            input_pulse_cycle <= cycle_count;

        if (pixel_de_out) begin
            output_pulse_cycle <= cycle_count;

            $display(
                "OUTPUT DE at cycle %0d, input was at cycle %0d, delay = %0d cycles",
                cycle_count,
                input_pulse_cycle,
                cycle_count - input_pulse_cycle
            );
        end
    end

    // ----------------------------------------------------------------
    // Task to send one complete row
    // ----------------------------------------------------------------
    task send_row;
        input [23:0] row_colour;
        input [23:0] left_colour;
        input [23:0] right_colour;

        integer x;

        begin
            @(negedge pixel_clock_in);

            pixel_de_in = 1;

            // Pixel 0
            rgb_pixel_in = left_colour;

            // Pixels 1 through 1918
            for (x = 1; x < simulated_width - 1; x = x + 1) begin
                @(negedge pixel_clock_in);
                rgb_pixel_in = row_colour;
            end

            // last pixel
            @(negedge pixel_clock_in);
            rgb_pixel_in = right_colour;

            // End of active video
            @(negedge pixel_clock_in);
            pixel_de_in = 0;
            rgb_pixel_in = 24'h000000;

            // horizontal blanking
            repeat (simulated_blanking_period-1) @(negedge pixel_clock_in);
        end
    endtask

    // ----------------------------------------------------------------
    // Test
    // ----------------------------------------------------------------
    initial begin
        cycle_count       = 0;
        input_pulse_cycle = -1;
        output_pulse_cycle = -1;

        rgb_pixel_in     = 24'h000000;
        pixel_de_in      = 0;
        h_sync_in        = 0;
        v_sync_in        = 0;
        transform_select = 0;

        // Assert reset
        reset_in_n = 0;

        // Hold reset for a few clocks
        repeat (5) @(posedge pixel_clock_in);

        // Release reset
        reset_in_n = 1;

        // Wait a clock
        repeat (1) @(posedge pixel_clock_in);

        // ============================================================
        // Row 0
        // Main colour: RED
        // Left edge:   WHITE
        // Right edge:  YELLOW
        // ============================================================
        send_row(
            24'hFF0000,       // normal pixels
            24'hFFFFFF,       // pixel 0
            24'hFFFF00        // last pixel
        );

        // ============================================================
        // Row 1
        // Main colour: GREEN
        // Left edge:   MAGENTA
        // Right edge:  CYAN
        // ============================================================
        send_row(
            24'h00FF00,
            24'hFF00FF,
            24'h00FFFF
        );

        // ============================================================
        // Row 2
        // Main colour: BLUE
        // Left edge:   BLACK
        // Right edge:  WHITE
        // ============================================================
        send_row(
            24'h0000FF,
            24'h000000,
            24'hFFFFFF
        );

        // ============================================================
        // Row 3
        // Main colour: YELLOW
        // Left edge:   RED
        // Right edge:  GREEN
        // ============================================================
        send_row(
            24'hFFFF00,
            24'hFF0000,
            24'h00FF00
        );

        // ============================================================
        // Row 4
        // Main colour: CYAN
        // Left edge:   BLUE
        // Right edge:  MAGENTA
        // ============================================================
        send_row(
            24'h00FFFF,
            24'h0000FF,
            24'hFF00FF
        );
        
        send_row(
            24'h000000,
            24'h00ff00,
            24'h000002
        );

        $display("Finished sending 6 rows.");

        // Allow pipeline to drain
        repeat (150) @(posedge pixel_clock_in);

        $display("Simulation complete.");
        $finish;
    end 

endmodule
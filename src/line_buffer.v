`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 09/13/2026 06:08:04 PM
// Design Name: 
// Module Name: line_buffer
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


module line_buffer#(
                    parameter WIDTH = 1920)(
    input wire clk_in,
    input wire reset_in_n,
    input wire [23:0] input_pixel, // input pixel is 24 bits
    input wire write_enable,
    input wire read_enable,
    input wire [$clog2(WIDTH) - 1 :0] read_address,
    input wire [$clog2(WIDTH) - 1 :0] write_address,
    
    output reg [23:0] pixel_out
    );
    
    localparam ADDR_WIDTH = $clog2(WIDTH);
    
    // model the RAM
//    (* ram_style = "block" *)
    reg [23:0] line [0 : WIDTH - 1];
    
    always@(posedge clk_in) begin
        if (write_enable && (write_address <= WIDTH - 1)) begin
            line[write_address] <= input_pixel;
        end
    end
    
    // note: read enable determines if the output changes (on the next edge), and the output will otherwise be whatever was last read (latched)
    always@(posedge clk_in) begin
        if (~reset_in_n) begin
            pixel_out <= 24'b0;
        end
        else if (read_enable && (read_address <= WIDTH - 1)) begin
            pixel_out <= line[read_address];
        end
     end
    
endmodule

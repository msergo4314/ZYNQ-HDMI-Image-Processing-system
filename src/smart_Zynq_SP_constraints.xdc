## Smart Zynq SP constraints file

#reminder: memory part is MT41K256M16RE-125, width is 16 bits

## Buttons
# note these are driven low on press (idle high) per the PCB schematic and must be inverted for a press to be a logic '1'
set_property -dict { PACKAGE_PIN K21 IOSTANDARD LVCMOS33 } [get_ports {BUTTON_1}]
#set_property -dict { PACKAGE_PIN J20 IOSTANDARD LVCMOS33 } [get_ports {BUTTON_2}]

## LEDs (green colour)
#set_property -dict { PACKAGE_PIN P20 IOSTANDARD LVCMOS33 } [get_ports {LED_1}]
set_property -dict { PACKAGE_PIN P21 IOSTANDARD LVCMOS33 } [get_ports {LED_2}]

## UART
set_property -dict { PACKAGE_PIN L17 IOSTANDARD LVCMOS33 } [get_ports {UART_0_0_txd}]
set_property -dict { PACKAGE_PIN M17 IOSTANDARD LVCMOS33 } [get_ports {UART_0_0_rxd}]

## PL Clock (50 MHz)
#set_property -dict { PACKAGE_PIN M19 IOSTANDARD LVCMOS33 } [get_ports {clk_50Mhz}]
#create_clock -name clk_50Mhz -period 20.0 [get_ports {clk_50Mhz}]

## 320x172 LCD display
#(LCD BLK : default high level by external pull-up resistance )
#set_property -dict { PACKAGE_PIN P15 IOSTANDARD LVCMOS33 } [get_ports {LCD_CS}]
#set_property -dict { PACKAGE_PIN N15 IOSTANDARD LVCMOS33 } [get_ports {LCD_SCL}]
#set_property -dict { PACKAGE_PIN M15 IOSTANDARD LVCMOS33 } [get_ports {LCD_SDA}]
#set_property -dict { PACKAGE_PIN R15 IOSTANDARD LVCMOS33 } [get_ports {LCD_DC} ]
#set_property -dict { PACKAGE_PIN L16 IOSTANDARD LVCMOS33 } [get_ports {LCD_Reset}]
#set_property -dict { PACKAGE_PIN T16 IOSTANDARD LVCMOS33 } [get_ports {LCD_Backlight}]

## EEPROM
#set_property -dict { PACKAGE_PIN R20 IOSTANDARD LVCMOS33 } [get_ports {SCL}]
#set_property -dict { PACKAGE_PIN R21 IOSTANDARD LVCMOS33 } [get_ports {SDA}]

## GigE phy (Ethernet)
#set_property -dict { PACKAGE_PIN E21 IOSTANDARD LVCMOS33 } [get_ports {ETH_TD0}]
#set_property -dict { PACKAGE_PIN F21 IOSTANDARD LVCMOS33 } [get_ports {ETH_TD1}]
#set_property -dict { PACKAGE_PIN F22 IOSTANDARD LVCMOS33 } [get_ports {ETH_TD2}]
#set_property -dict { PACKAGE_PIN G20 IOSTANDARD LVCMOS33 } [get_ports {ETH_TD3}]
#set_property -dict { PACKAGE_PIN G22 IOSTANDARD LVCMOS33 } [get_ports {ETH_TX_CTL}]
#set_property -dict { PACKAGE_PIN D21 IOSTANDARD LVCMOS33 } [get_ports {ETH_TXC}]
#set_property -dict { PACKAGE_PIN A22 IOSTANDARD LVCMOS33 } [get_ports {ETH_RD0}]
#set_property -dict { PACKAGE_PIN A18 IOSTANDARD LVCMOS33 } [get_ports {ETH_RD1}]
#set_property -dict { PACKAGE_PIN A19 IOSTANDARD LVCMOS33 } [get_ports {ETH_RD2}]
#set_property -dict { PACKAGE_PIN B20 IOSTANDARD LVCMOS33 } [get_ports {ETH_RD3}]
#set_property -dict { PACKAGE_PIN A21 IOSTANDARD LVCMOS33 } [get_ports {ETH_RX_CTL}]
#set_property -dict { PACKAGE_PIN B19 IOSTANDARD LVCMOS33 } [get_ports {ETH_RXC}]
#set_property -dict { PACKAGE_PIN H22 IOSTANDARD LVCMOS33 } [get_ports {ETH_MDIO}]
#set_property -dict { PACKAGE_PIN G21 IOSTANDARD LVCMOS33 } [get_ports {ETH_MDC}]
#set_property -dict { PACKAGE_PIN H18 IOSTANDARD LVCMOS33 } [get_ports {ETH_INT}]

## HDMI
set_property -dict { PACKAGE_PIN N22} [get_ports {TMDS_0_clk_p}]
set_property -dict { PACKAGE_PIN P22} [get_ports {TMDS_0_clk_n}]
set_property -dict { PACKAGE_PIN M21} [get_ports {TMDS_0_data_p[0]}]
set_property -dict { PACKAGE_PIN M22} [get_ports {TMDS_0_data_n[0]}]
set_property -dict { PACKAGE_PIN L21} [get_ports {TMDS_0_data_p[1]}]
set_property -dict { PACKAGE_PIN L22} [get_ports {TMDS_0_data_n[1]}]
set_property -dict { PACKAGE_PIN J21} [get_ports {TMDS_0_data_p[2]}]
set_property -dict { PACKAGE_PIN J22} [get_ports {TMDS_0_data_n[2]}]

## GPIO Headers on the board rails

## you can read the PCB itself for the package pins for each GPIO pin and add here...
#set_property -dict { PACKAGE_PIN G15 IOSTANDARD LVCMOS33 } [get_ports {G15}]
#set_property -dict { PACKAGE_PIN G16 IOSTANDARD LVCMOS33 } [get_ports {G16}]
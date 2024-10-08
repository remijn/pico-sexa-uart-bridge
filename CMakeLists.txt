# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13)

# include(pico-sdk/pico_sdk_init.cmake)

include(pico_sdk_import.cmake)

project(pico_uart_bridge)

pico_sdk_init()
  
add_executable(uart_bridge uart-bridge.c usb-descriptors.c)

pico_generate_pio_header(uart_bridge ${CMAKE_CURRENT_LIST_DIR}/uart_rx.pio)
pico_generate_pio_header(uart_bridge ${CMAKE_CURRENT_LIST_DIR}/uart_tx.pio)

target_include_directories(uart_bridge PUBLIC
	./
	../pico-sdk/lib/tinyusb/src)

target_link_libraries(uart_bridge
	pico_multicore
	pico_stdlib
	hardware_flash
	tinyusb_device
        hardware_pio)

# pico_enable_stdio_uart(uart_bridge 1)

pico_add_extra_outputs(uart_bridge)

cmake_minimum_required(VERSION 3.12)

# build for the Raspberry Pi Debug Probe by default
set(PICO_BOARD debug_probe CACHE STRING "Board type")

set(CMAKE_BUILD_TYPE RelWithDebInfo)

include(pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(FreeRTOS_Kernel_import.cmake)

project(debugprobe)

pico_sdk_init()

add_executable(debugprobe
        src/probe_config.c
        src/main.c
        src/usb_descriptors.c
        src/probe.c
        src/cdc_uart.c
        src/get_serial.c
        src/sw_dp_pio.c
        src/tusb_edpt_handler.c
        src/autobaud.c
        src/pio_alloc.c
)

target_sources(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
        #CMSIS_DAP/CMSIS/DAP/Firmware/Source/SW_DP.c
        )

target_include_directories(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Include/
        CMSIS_DAP/CMSIS/Core/Include/
        include/
        )

target_compile_options(debugprobe PRIVATE -Wall)

pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe_oen.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/autobaud.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/uart_pio.pio)

target_include_directories(debugprobe PRIVATE src)

# add version
add_custom_target(version
        ${CMAKE_SOURCE_DIR}/get-version.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(debugprobe PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        )

add_dependencies(debugprobe version)

# Number of independent SWD channels, each exposed as its own CMSIS-DAP v2 USB interface
set(PROBE_NUM_CHANNELS 1 CACHE STRING "Number of SWD channels (CMSIS-DAP interfaces)")
# Number of target UARTs, each exposed as its own CDC-ACM port
set(PROBE_NUM_UARTS 1 CACHE STRING "Number of target UARTs (CDC-ACM ports)")

target_compile_definitions (debugprobe PRIVATE
	PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PROBE_NUM_CHANNELS=${PROBE_NUM_CHANNELS}
    PROBE_NUM_UARTS=${PROBE_NUM_UARTS}
)

# Firmware debug log on the board's default UART (TX only, 115200 8N1):
# 0 = off, 1 = info, 2 = + debug, 3 = + packet dumps
set(PROBE_DEBUG_LOG 0 CACHE STRING "Firmware debug log level (0-3)")
target_compile_definitions (debugprobe PRIVATE PROBE_DEBUG_LOG=${PROBE_DEBUG_LOG})

if (PICO_BOARD STREQUAL "waveshare_rp2350_pizero")
    # The SDK default (uart1 TX on GP4) clashes with SWD channel 0 nRESET;
    # put the debug log on GP24 (header pin 18) instead
    target_compile_definitions (debugprobe PRIVATE
        PICO_DEFAULT_UART=1
        PICO_DEFAULT_UART_TX_PIN=24
    )
endif()

set(PROBE_OUTPUT_NAME "debugprobe")
if (NOT PICO_BOARD STREQUAL "debug_probe")
    set(PROBE_OUTPUT_NAME "${PROBE_OUTPUT_NAME}_on_${PICO_BOARD}")
endif ()
if (NOT PROBE_NUM_CHANNELS EQUAL 1)
    set(PROBE_OUTPUT_NAME "${PROBE_OUTPUT_NAME}_${PROBE_NUM_CHANNELS}ch")
endif ()
if (NOT PROBE_NUM_UARTS EQUAL 1)
    set(PROBE_OUTPUT_NAME "${PROBE_OUTPUT_NAME}_${PROBE_NUM_UARTS}uart")
endif ()
set_target_properties(debugprobe PROPERTIES OUTPUT_NAME "${PROBE_OUTPUT_NAME}")

target_link_libraries(debugprobe PRIVATE
        pico_multicore
        pico_stdlib
        pico_usb_reset
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_pio
        hardware_uart
        hardware_dma
        hardware_irq
        hardware_clocks
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap1
)

pico_set_binary_type(debugprobe copy_to_ram)

pico_add_extra_outputs(debugprobe)

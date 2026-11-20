/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef PROBE_CONFIG_H_
#define PROBE_CONFIG_H_

#include "FreeRTOS.h"
#include "task.h"

#ifndef PROBE_DEBUG_LOG
#define PROBE_DEBUG_LOG 0
#endif

#if PROBE_DEBUG_LOG >= 1
#define probe_info(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_info(format,...) ((void)0)
#endif


#if PROBE_DEBUG_LOG >= 2
#define probe_debug(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_debug(format,...) ((void)0)
#endif

#if PROBE_DEBUG_LOG >= 3
#define probe_dump(format,args...)\
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_dump(format,...) ((void)0)
#endif

/*
 * Pin assignment of one SWD channel. Unused pins are -1.
 *  - swclk:   SWCLK output (PIO side-set)
 *  - swdio:   SWDIO data pin (PIO out/set, and PIO input unless swdi is used)
 *  - swdi:    separate level-shifted SWDIO input (PROBE_IO_SWDI / PROBE_IO_OEN)
 *  - swdioen: SWDIO output-enable for an external buffer (PROBE_IO_OEN only),
 *             must be the GPIO directly below swclk (two side-set pins)
 *  - reset:   target nRESET, driven open-drain
 */
typedef struct {
    int8_t swclk;
    int8_t swdio;
    int8_t swdi;
    int8_t swdioen;
    int8_t reset;
} probe_pins_t;

#define PROBE_PINS(swclk_, swdio_, swdi_, swdioen_, reset_) \
    { .swclk = (swclk_), .swdio = (swdio_), .swdi = (swdi_), .swdioen = (swdioen_), .reset = (reset_) }

/*
 * Pin assignment of one target UART (CDC-ACM port). Unused pins are -1.
 * If tx/rx sit on a hardware UART's TX/RX pin pair (GPIO 4n / 4n+1) and that
 * UART is still free, the hardware UART is used; otherwise the UART runs on
 * PIO (8N1 only, two state machines). cts/rts with hwfc=true use hardware
 * flow control (hardware UART only); otherwise rts and dtr are plain GPIO
 * outputs mirroring the host's line state (active low).
 */
typedef struct {
    int8_t tx;
    int8_t rx;
    int8_t cts;
    int8_t rts;
    int8_t dtr;
    bool hwfc;
} probe_uart_pins_t;

#define PROBE_UART_PINS_FULL(tx_, rx_, cts_, rts_, dtr_, hwfc_) \
    { .tx = (tx_), .rx = (rx_), .cts = (cts_), .rts = (rts_), .dtr = (dtr_), .hwfc = (hwfc_) }
#define PROBE_UART(tx_, rx_) PROBE_UART_PINS_FULL(tx_, rx_, -1, -1, -1, false)

/* Number of target UARTs, each exposed as its own CDC-ACM port. Normally set from CMake. */
#ifndef PROBE_NUM_UARTS
#define PROBE_NUM_UARTS 1
#endif

#include "board_config.h"

/*
 * Number of independent SWD channels. Each one is exposed as its own
 * CMSIS-DAP v2 interface on the USB device, driven by its own PIO state
 * machine, so separate debugger instances (e.g. OpenOCD) can use them
 * concurrently. Normally set from CMake (-DPROBE_NUM_CHANNELS=n).
 */
#ifndef PROBE_NUM_CHANNELS
#define PROBE_NUM_CHANNELS 1
#endif

/* Bounded by spare USB endpoints (EP6..EP15) for channels 1..n-1 */
#define PROBE_MAX_CHANNELS 11

#if (PROBE_NUM_CHANNELS < 1) || (PROBE_NUM_CHANNELS > PROBE_MAX_CHANNELS)
#error "PROBE_NUM_CHANNELS must be between 1 and PROBE_MAX_CHANNELS"
#endif

#if (PROBE_NUM_UARTS < 1)
#error "PROBE_NUM_UARTS must be at least 1"
#endif

/*
 * Beyond the hardware UARTs, every UART needs two PIO state machines. With the
 * debug log enabled, one hardware UART may be reserved for it.
 */
#define PROBE_NUM_TARGET_HW_UARTS (PROBE_DEBUG_LOG > 0 ? 1 : 2)
#define PROBE_NUM_PIO_UARTS_MIN (PROBE_NUM_UARTS > PROBE_NUM_TARGET_HW_UARTS ? \
                                 PROBE_NUM_UARTS - PROBE_NUM_TARGET_HW_UARTS : 0)

/*
 * USB endpoint numbers 1..15: EP1-EP5 are used by DAP channel 0 and UART 0,
 * each further DAP channel needs one, each further UART two (data + notification).
 */
#if (5 + (PROBE_NUM_CHANNELS - 1) + 2 * (PROBE_NUM_UARTS - 1)) > 15
#error "Not enough USB endpoints for PROBE_NUM_CHANNELS and PROBE_NUM_UARTS"
#endif

/* UART 0 as described by the legacy PROBE_UART_* macros */
#ifdef PROBE_UART_CTS
#define PROBE_LEGACY_UART_CTS PROBE_UART_CTS
#else
#define PROBE_LEGACY_UART_CTS -1
#endif
#ifdef PROBE_UART_RTS
#define PROBE_LEGACY_UART_RTS PROBE_UART_RTS
#else
#define PROBE_LEGACY_UART_RTS -1
#endif
#ifdef PROBE_UART_DTR
#define PROBE_LEGACY_UART_DTR PROBE_UART_DTR
#else
#define PROBE_LEGACY_UART_DTR -1
#endif
#ifdef PROBE_UART_HWFC
#define PROBE_LEGACY_UART_HWFC true
#else
#define PROBE_LEGACY_UART_HWFC false
#endif
#define PROBE_LEGACY_UART \
    PROBE_UART_PINS_FULL(PROBE_UART_TX, PROBE_UART_RX, PROBE_LEGACY_UART_CTS, \
                         PROBE_LEGACY_UART_RTS, PROBE_LEGACY_UART_DTR, PROBE_LEGACY_UART_HWFC)

/* Boards that don't provide a UART table get a single UART from the legacy pin macros */
#ifndef PROBE_UART_PIN_TABLE
#define PROBE_UART_PIN_TABLE { PROBE_LEGACY_UART }
#endif

/* Boards that don't provide a channel table get a single channel from the legacy pin macros */
#ifndef PROBE_CHANNEL_PINS
#ifndef PROBE_PIN_SWDI
#define PROBE_LEGACY_SWDI -1
#else
#define PROBE_LEGACY_SWDI PROBE_PIN_SWDI
#endif
#ifndef PROBE_PIN_SWDIOEN
#define PROBE_LEGACY_SWDIOEN -1
#else
#define PROBE_LEGACY_SWDIOEN PROBE_PIN_SWDIOEN
#endif
#ifndef PROBE_PIN_RESET
#define PROBE_LEGACY_RESET -1
#else
#define PROBE_LEGACY_RESET PROBE_PIN_RESET
#endif
#define PROBE_CHANNEL_PINS { \
    PROBE_PINS(PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, PROBE_LEGACY_SWDI, PROBE_LEGACY_SWDIOEN, PROBE_LEGACY_RESET) \
}
#endif

// Add the configuration to binary information
void bi_decl_config();

// True if any enabled SWD channel or target UART uses this GPIO
bool probe_config_pin_in_use(int pin);

#define PROTO_DAP_V1 1
#define PROTO_DAP_V2 2

// Interface config
#ifndef PROBE_DEBUG_PROTOCOL
#define PROBE_DEBUG_PROTOCOL PROTO_DAP_V2
#endif

#if (PROBE_NUM_CHANNELS > 1) && (PROBE_DEBUG_PROTOCOL != PROTO_DAP_V2)
#error "Multiple SWD channels require PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2"
#endif

#endif

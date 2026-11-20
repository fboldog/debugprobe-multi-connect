/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
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

/*
 * Waveshare RP2350-PiZero (RP2350B, Raspberry Pi Zero form factor).
 *
 * The 40-pin header carries GPIO0..27 with the usual Raspberry Pi BCM
 * numbering (header "BCM n" == RP2350 GPIOn). GPIO28 and up are used on board
 * (PIO-USB 28/29, TF card 30/31/40-43, DVI 32-39/44-46, PSRAM CS 47) and are
 * not used here. The board has no user LED (LED1 is a power LED).
 *
 * The probe connects to the host through the native USB-C port (J4, "USB"),
 * not the PIO-USB port.
 *
 * Firmware debug log (-DPROBE_DEBUG_LOG=1..3): uart1 TX on GP24 (header pin
 * 18, GND on pin 20), 115200 8N1, no RX. Set in CMakeLists.txt, because the
 * SDK board default (GP4) is SWD channel 0 nRESET here. GP24 is free in the
 * paired layout and in the SWD-only layout up to 4 channels; channel 4 uses
 * it as nRESET, which turns the log off. With the log on, uart1 is kept for
 * it and target UART 1 runs on PIO.
 */

#ifndef BOARD_WAVESHARE_RP2350_PIZERO_CONFIG_H_
#define BOARD_WAVESHARE_RP2350_PIZERO_CONFIG_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// Channel 0 SWD / reset (also used for binary info)
#define PROBE_PIN_SWCLK 2  // header pin 3
#define PROBE_PIN_SWDIO 3  // header pin 5
#define PROBE_PIN_RESET 4  // header pin 7

// UART 0: the Raspberry Pi console pins, hardware uart0 (UART_AUX function)
#define PROBE_UART_TX 14   // header pin 8
#define PROBE_UART_RX 15   // header pin 10
#define PROBE_UART_INTERFACE uart0
#define PROBE_UART_BAUDRATE 115200

#if PROBE_NUM_UARTS > 1
/*
 * Paired layout: each target gets a block of adjacent header pins.
 * UART 0/1 use hardware uart0/uart1 (UART_AUX pins), UART 2/3 run on PIO.
 *
 *         SWCLK      SWDIO      RESET      UART TX    UART RX    GND
 * pair 0: GP2  (3)   GP3  (5)   GP4  (7)   GP14 (8)   GP15 (10)  6, 9
 * pair 1: GP17 (11)  GP27 (13)  GP18 (12)  GP22 (15)  GP23 (16)  14
 * pair 2: GP10 (19)  GP9  (21)  GP11 (23)  GP25 (22)  GP8  (24)  20, 25
 * pair 3: GP19 (35)  GP26 (37)  GP16 (36)  GP20 (38)  GP21 (40)  39
 */
#define PROBE_CHANNEL_PINS { \
    PROBE_PINS(PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, -1, -1, PROBE_PIN_RESET), \
    PROBE_PINS(17, 27, -1, -1, 18), \
    PROBE_PINS(10,  9, -1, -1, 11), \
    PROBE_PINS(19, 26, -1, -1, 16), \
}
#define PROBE_UART_PIN_TABLE { \
    PROBE_LEGACY_UART, \
    PROBE_UART(22, 23), \
    PROBE_UART(25,  8), \
    PROBE_UART(20, 21), \
}
#else
/*
 * SWD-only layout: up to 9 channels, single UART on GP14/GP15 (pins 8/10).
 * The first PROBE_NUM_CHANNELS entries are used.
 *
 *                 SWCLK  SWDIO  SWDI SWDIOEN RESET     header pins
 */
#define PROBE_CHANNEL_PINS { \
    PROBE_PINS(PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, -1, -1, PROBE_PIN_RESET), /* 3, 5, 7    */ \
    PROBE_PINS(17, 27, -1, -1, 18), /* 11, 13, 12 */ \
    PROBE_PINS(10,  9, -1, -1, 11), /* 19, 21, 23 */ \
    PROBE_PINS(19, 26, -1, -1, 16), /* 35, 37, 36 */ \
    PROBE_PINS(22, 23, -1, -1, 24), /* 15, 16, 18 */ \
    PROBE_PINS(25,  8, -1, -1,  7), /* 22, 24, 26 */ \
    PROBE_PINS( 5,  6, -1, -1, 13), /* 29, 31, 33 */ \
    PROBE_PINS(20, 21, -1, -1, 12), /* 38, 40, 32 */ \
    PROBE_PINS( 0,  1, -1, -1, -1), /* 27, 28, -  */ \
}
#endif

#define PROBE_PRODUCT_STRING "Debugprobe on RP2350-PiZero (CMSIS-DAP)"

#endif

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

#ifndef BOARD_PICO_CONFIG_H_
#define BOARD_PICO_CONFIG_H_

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// PIO config
#define PROBE_PIN_OFFSET 2
#define PROBE_PIN_SWCLK (PROBE_PIN_OFFSET + 0) // 2
#define PROBE_PIN_SWDIO (PROBE_PIN_OFFSET + 1) // 3
// Target reset config
#define PROBE_PIN_RESET 1

#if PROBE_NUM_UARTS > 1
/*
 * Paired layout: SWD channel n and UART n form one block of pins per target.
 * UART 0 and 1 use the hardware UARTs, UART 2 and 3 run on PIO.
 * Up to 4 pairs (RP2040: 3 pairs, limited by PIO state machines).
 *
 *        SWCLK SWDIO RESET  UART TX  UART RX
 * pair 0:  GP2   GP3   GP1    GP4      GP5    (uart1, classic pinout)
 * pair 1:  GP9   GP10  GP11   GP12     GP13   (uart0)
 * pair 2:  GP14  GP15  GP16   GP17     GP18   (PIO)
 * pair 3:  GP19  GP20  GP21   GP22     GP26   (PIO)
 */
#define PROBE_CHANNEL_PINS { \
    PROBE_PINS(PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, -1, -1, PROBE_PIN_RESET), \
    PROBE_PINS( 9, 10, -1, -1, 11), \
    PROBE_PINS(14, 15, -1, -1, 16), \
    PROBE_PINS(19, 20, -1, -1, 21), \
}
#define PROBE_UART_PIN_TABLE { \
    PROBE_LEGACY_UART, \
    PROBE_UART(12, 13), \
    PROBE_UART(17, 18), \
    PROBE_UART(22, 26), \
}
#else
/*
 * SWD-only layout: as many SWD channels as possible, with the single UART on
 * GP4/GP5. Channel 0 keeps the classic pinout; the first PROBE_NUM_CHANNELS
 * entries are used. GPIO 0/1 (stdio UART), 4/5 (target UART) and 23/24/25
 * (Pico power/VBUS/LED) are avoided.
 *
 *                 SWCLK            SWDIO            SWDI SWDIOEN RESET
 */
#define PROBE_CHANNEL_PINS { \
    PROBE_PINS(PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, -1, -1, PROBE_PIN_RESET), /* ch0 */ \
    PROBE_PINS( 6,  7, -1, -1,  8), /* ch1 */ \
    PROBE_PINS( 9, 10, -1, -1, 11), /* ch2 */ \
    PROBE_PINS(12, 13, -1, -1, 14), /* ch3 */ \
    PROBE_PINS(16, 17, -1, -1, 18), /* ch4 */ \
    PROBE_PINS(19, 20, -1, -1, 21), /* ch5 */ \
    PROBE_PINS(22, 26, -1, -1, 27), /* ch6 */ \
    PROBE_PINS(15, 28, -1, -1, -1), /* ch7 (RP2350 only: needs a 3rd PIO) */ \
}
#endif

// UART config
#define PROBE_UART_TX 4
#define PROBE_UART_RX 5
#define PROBE_UART_INTERFACE uart1
#define PROBE_UART_BAUDRATE 115200

#define PROBE_USB_CONNECTED_LED 25

#define PROBE_PRODUCT_STRING "Debugprobe on Pico (CMSIS-DAP)"

#endif

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

#include <pico/stdlib.h>
#include <hardware/uart.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "autobaud.h"

#include "probe_config.h"
#include "pio_alloc.h"
#include "cdc_uart.h"
#include "uart_pio.pio.h"

/*
 * Each target UART is bridged to its own CDC-ACM interface (CDC instance n
 * <-> UART n). A UART uses a hardware UART when its pins allow it, otherwise
 * a pair of PIO state machines (8N1 only). One thread polls all UARTs, at
 * an interval that scales with the fastest active line rate.
 * Autobaud and the UART TX/RX LEDs belong to UART 0.
 */

TaskHandle_t uart_taskhandle;
static TickType_t last_wake;

// Actually s^-1 so 25ms
#define DEBOUNCE_MS 40

#define TX_CHUNK 16

typedef struct {
    probe_uart_pins_t pins;
    uart_inst_t *hw;            // hardware UART, or NULL when implemented on PIO
    PIO pio_tx, pio_rx;
    uint sm_tx, sm_rx;
    uint off_tx, off_rx;
    TickType_t interval;        // ticks to fill half the RX FIFO
    uint debounce_ticks;
    volatile bool active;       // host has opened the port (DTR asserted)
    volatile bool timed_break;
    volatile TickType_t break_expiry;
    bool was_connected;
    uint8_t tx_buf[TX_CHUNK];   // host -> target bytes not yet in the UART FIFO
    uint8_t tx_len, tx_pos;
    volatile uint tx_led_debounce;
    uint rx_led_debounce;
} cdc_uart_t;

static const probe_uart_pins_t uart_pins[] = PROBE_UART_PIN_TABLE;
static_assert(count_of(uart_pins) >= PROBE_NUM_UARTS,
              "PROBE_UART_PIN_TABLE has fewer entries than PROBE_NUM_UARTS");
static_assert(CFG_TUD_CDC == PROBE_NUM_UARTS, "CFG_TUD_CDC must match PROBE_NUM_UARTS");

static cdc_uart_t uarts[PROBE_NUM_UARTS];

/* ---- LEDs (UART 0 only) ---- */

static inline void led_tx(cdc_uart_t *u, bool on)
{
#ifdef PROBE_UART_TX_LED
    if (u == &uarts[0])
        gpio_put(PROBE_UART_TX_LED, on);
#endif
}

static inline void led_rx(cdc_uart_t *u, bool on)
{
#ifdef PROBE_UART_RX_LED
    if (u == &uarts[0])
        gpio_put(PROBE_UART_RX_LED, on);
#endif
}

/* ---- Low level UART access, hardware or PIO ---- */

static float pio_uart_div(uint32_t baudrate)
{
    return (float)clock_get_hz(clk_sys) / (8.0f * MAX(baudrate, 1));
}

/*
 * Hardware UART whose TX/RX function sits on these pins: GPIO 4n -> TX,
 * 4n+1 -> RX; on RP2350 also GPIO 4n+2 -> TX, 4n+3 -> RX (UART_AUX). The
 * instance alternates every 4 pins and the pattern flips every 8:
 * GPIO 0 uart0, 4 uart1, 8 uart1, 12 uart0, 16 uart0, 20 uart1, ...
 */
static uart_inst_t *hw_uart_for_pins(int tx, int rx)
{
    if (tx < 0 || rx != tx + 1 || rx >= (int)NUM_BANK0_GPIOS)
        return NULL;
#if PICO_RP2040
    if ((tx & 3) != 0)
        return NULL;
#else
    if ((tx & 1) != 0)
        return NULL;
#endif
    return UART_INSTANCE(((tx >> 2) ^ (tx >> 3)) & 1);
}

static uint cdc_uart_read(cdc_uart_t *u, uint8_t *buf, uint max)
{
    uint n = 0;
    if (u->hw) {
        while (n < max && uart_is_readable(u->hw))
            buf[n++] = uart_getc(u->hw);
    } else {
        while (n < max && !pio_sm_is_rx_fifo_empty(u->pio_rx, u->sm_rx))
            buf[n++] = pio_sm_get(u->pio_rx, u->sm_rx) >> 24;
    }
    return n;
}

/* Push buffered TX data into the UART FIFO without blocking */
static void cdc_uart_push_tx(cdc_uart_t *u)
{
    while (u->tx_pos < u->tx_len) {
        if (u->hw) {
            if (!uart_is_writable(u->hw))
                break;
            uart_putc_raw(u->hw, u->tx_buf[u->tx_pos++]);
        } else {
            if (pio_sm_is_tx_fifo_full(u->pio_tx, u->sm_tx))
                break;
            pio_sm_put(u->pio_tx, u->sm_tx, u->tx_buf[u->tx_pos++]);
        }
    }
}

static void cdc_uart_break(cdc_uart_t *u, bool on)
{
    if (u->hw) {
        uart_set_break(u->hw, on);
    } else if (on) {
        // Take TX away from the PIO and hold it low
        gpio_put(u->pins.tx, 0);
        gpio_set_dir(u->pins.tx, GPIO_OUT);
        gpio_set_function(u->pins.tx, GPIO_FUNC_SIO);
    } else {
        pio_gpio_init(u->pio_tx, u->pins.tx);
    }
}

static void cdc_uart_set_interval(cdc_uart_t *u, uint32_t baudrate)
{
    /* Set the tick thread interval to the amount of time it takes to
     * fill up half a FIFO. Millis is too coarse for integer divide.
     * Hardware FIFOs are 32 deep, the (joined) PIO RX FIFO 8 deep.
     */
    uint32_t half_fifo = u->hw ? 16 : 4;
    uint32_t micros = (1000 * 1000 * half_fifo * 10) / MAX(baudrate, 1);
    u->interval = MAX(1, micros / ((1000 * 1000) / configTICK_RATE_HZ));
    u->debounce_ticks = MAX(1, configTICK_RATE_HZ / (u->interval * DEBOUNCE_MS));
    probe_info("UART%u new baud rate %ld micros %ld interval %lu\n",
               (uint)(u - uarts), baudrate, micros, u->interval);
}

static void cdc_uart_set_baud(cdc_uart_t *u, uint32_t baudrate)
{
    uint idx = u - uarts;

    cdc_uart_set_interval(u, baudrate);
    tud_cdc_n_write_clear(idx);
    tud_cdc_n_read_flush(idx);
    u->tx_len = u->tx_pos = 0;

    if (u->hw) {
        uart_deinit(u->hw);
        uart_init(u->hw, baudrate);
    } else {
        float div = pio_uart_div(baudrate);
        pio_sm_set_clkdiv(u->pio_tx, u->sm_tx, div);
        pio_sm_set_clkdiv(u->pio_rx, u->sm_rx, div);
        pio_sm_clear_fifos(u->pio_rx, u->sm_rx);
    }
}

static void cdc_uart_set_format(cdc_uart_t *u, uint data_bits, uint stop_bits, uart_parity_t parity)
{
    if (u->hw)
        uart_set_format(u->hw, data_bits, stop_bits, parity);
    else if (data_bits != 8 || stop_bits != 1 || parity != UART_PARITY_NONE)
        probe_info("UART%u: PIO UART only supports 8N1\n", (uint)(u - uarts));
}

static void cdc_uart_init_one(cdc_uart_t *u, uint idx, uint *hw_claimed)
{
    const probe_uart_pins_t *p = &u->pins;

    u->hw = hw_uart_for_pins(p->tx, p->rx);
    if (u->hw && (*hw_claimed & (1u << UART_NUM(u->hw))))
        u->hw = NULL;

    if (u->hw) {
        *hw_claimed |= 1u << UART_NUM(u->hw);
        gpio_set_function(p->tx, UART_FUNCSEL_NUM(u->hw, p->tx));
        gpio_set_function(p->rx, UART_FUNCSEL_NUM(u->hw, p->rx));
        gpio_set_pulls(p->tx, 1, 0);
        gpio_set_pulls(p->rx, 1, 0);
        uart_init(u->hw, PROBE_UART_BAUDRATE);
    } else {
        float div = pio_uart_div(PROBE_UART_BAUDRATE);
        if (!pio_alloc_claim(&uart_tx_program, &u->pio_tx, &u->sm_tx, &u->off_tx) ||
            !pio_alloc_claim(&uart_rx_program, &u->pio_rx, &u->sm_rx, &u->off_rx))
            panic("No free PIO state machines for UART %u", idx);
        gpio_set_pulls(p->tx, 1, 0);
        uart_tx_program_init(u->pio_tx, u->sm_tx, u->off_tx, p->tx, div);
        uart_rx_program_init(u->pio_rx, u->sm_rx, u->off_rx, p->rx, div);
    }

    /* Hardware flow control only with TX/RX on the primary pins: in UART_AUX
     * mode the TX/RX functions occupy the CTS/RTS pins */
    if (p->hwfc && u->hw && p->cts >= 0 && p->rts >= 0 &&
        UART_FUNCSEL_NUM(u->hw, p->tx) == GPIO_FUNC_UART) {
        /* HWFC implies that hardware flow control is implemented and the
         * UART operates in "full-duplex" mode (See USB CDC PSTN120 6.3.12).
         * Default to pulling in the active direction, so an unconnected CTS
         * behaves the same as if CTS were not enabled. */
        gpio_set_pulls(p->cts, 0, 1);
        gpio_set_function(p->rts, GPIO_FUNC_UART);
        gpio_set_function(p->cts, GPIO_FUNC_UART);
        uart_set_hw_flow(u->hw, true, true);
    } else {
        u->pins.hwfc = false;
        if (p->rts >= 0) {
            gpio_init(p->rts);
            gpio_set_dir(p->rts, GPIO_OUT);
            gpio_put(p->rts, 1);
        }
    }

    if (p->dtr >= 0) {
        gpio_init(p->dtr);
        gpio_set_dir(p->dtr, GPIO_OUT);
        gpio_put(p->dtr, 1);
    }

    cdc_uart_set_interval(u, PROBE_UART_BAUDRATE);
}

void cdc_uart_init(void) {
    uint hw_claimed = 0;
#if PROBE_DEBUG_LOG > 0
    /* Keep the debug log's UART for it, unless its TX pin is in use anyway */
    if (!probe_config_pin_in_use(PICO_DEFAULT_UART_TX_PIN))
        hw_claimed |= 1u << UART_NUM(PICO_DEFAULT_UART_INSTANCE());
#endif

    for (uint i = 0; i < PROBE_NUM_UARTS; i++) {
        uarts[i].pins = uart_pins[i];
        cdc_uart_init_one(&uarts[i], i, &hw_claimed);
    }

#ifdef PROBE_UART_TX_LED
    gpio_init(PROBE_UART_TX_LED);
    gpio_set_dir(PROBE_UART_TX_LED, GPIO_OUT);
#endif
#ifdef PROBE_UART_RX_LED
    gpio_init(PROBE_UART_RX_LED);
    gpio_set_dir(PROBE_UART_RX_LED, GPIO_OUT);
#endif
}

bool cdc_uart_hw_in_use(uart_inst_t *hw)
{
    for (uint i = 0; i < PROBE_NUM_UARTS; i++)
        if (uarts[i].hw == hw)
            return true;
    return false;
}

static bool any_active(void)
{
    for (uint i = 0; i < PROBE_NUM_UARTS; i++)
        if (uarts[i].active)
            return true;
    return false;
}

/* ---- Bridging ---- */

static bool cdc_task_one(uint idx)
{
    cdc_uart_t *u = &uarts[idx];
    /* Max 1 FIFO worth of data */
    uint8_t rx_buf[32];
    bool keep_alive = false;

    // Consume uart fifo regardless even if not connected
    uint rx_len = cdc_uart_read(u, rx_buf, sizeof(rx_buf));

    if (tud_cdc_n_connected(idx)) {
        u->was_connected = true;
        /* Implicit overflow if we don't write all the bytes to the host.
         * Also throw away bytes if we can't write... */
        if (rx_len) {
            led_rx(u, 1);
            u->rx_led_debounce = u->debounce_ticks;
            uint written = MIN(tud_cdc_n_write_available(idx), rx_len);
            if (written > 0) {
                tud_cdc_n_write(idx, rx_buf, written);
                tud_cdc_n_write_flush(idx);
            }
        } else {
            if (u->rx_led_debounce)
                u->rx_led_debounce--;
            else
                led_rx(u, 0);
        }

        /* Reading from a firehose and writing to a FIFO - never block, other UARTs need servicing too */
        bool sent = false;
        for (uint pass = 0; pass < 2; pass++) {
            if (u->tx_pos == u->tx_len) {
                uint32_t avail = MIN(tud_cdc_n_available(idx), TX_CHUNK);
                if (avail == 0)
                    break;
                u->tx_len = tud_cdc_n_read(idx, u->tx_buf, avail);
                u->tx_pos = 0;
            }
            sent = true;
            cdc_uart_push_tx(u);
            if (u->tx_pos != u->tx_len)
                break;
        }
        if (sent) {
            led_tx(u, 1);
            u->tx_led_debounce = u->debounce_ticks;
        } else {
            if (u->tx_led_debounce)
                u->tx_led_debounce--;
            else
                led_tx(u, 0);
        }
        if (u->tx_pos != u->tx_len)
            keep_alive = true;

        /* Pending break handling */
        if (u->timed_break) {
            if (((int)u->break_expiry - (int)xTaskGetTickCount()) < 0) {
                u->timed_break = false;
                cdc_uart_break(u, false);
                u->tx_led_debounce = 0;
            } else {
                keep_alive = true;
            }
        }
    } else if (u->was_connected) {
        tud_cdc_n_write_clear(idx);
        cdc_uart_break(u, false);
        u->timed_break = false;
        u->was_connected = false;
        u->tx_len = u->tx_pos = 0;
        u->tx_led_debounce = 0;
    }
    return keep_alive;
}

bool cdc_task(void)
{
    bool keep_alive = false;
    for (uint i = 0; i < PROBE_NUM_UARTS; i++)
        keep_alive |= cdc_task_one(i);
    return keep_alive;
}

void cdc_thread(void *ptr)
{
    BaseType_t delayed;
    BaudInfo_t baud_info;
    last_wake = xTaskGetTickCount();
    bool keep_alive;
    /* Threaded with a polling interval that scales according to the fastest active linerate */
    while (1) {
        keep_alive = cdc_task();
        if (!keep_alive) {
            TickType_t interval = 100;
            for (uint i = 0; i < PROBE_NUM_UARTS; i++)
                if (uarts[i].active)
                    interval = MIN(interval, uarts[i].interval);
            delayed = xTaskDelayUntil(&last_wake, interval);
            if (delayed == pdFALSE)
                last_wake = xTaskGetTickCount();
            if (autobaud_running) {
                // Receive baud information from autobaud thread (UART 0)
                if (xQueueReceive(baudQueue, &baud_info, 0) == pdTRUE) {
                    cdc_uart_set_baud(&uarts[0], baud_info.baud);
                    // Assume 8N1
                    cdc_uart_set_format(&uarts[0], 8, 1, UART_PARITY_NONE);
                }
            }
        } else {
            // TX backlog or break pending: come back soon, but let USB and DAP tasks run
            vTaskDelay(1);
            last_wake = xTaskGetTickCount();
        }
    }
}

/* ---- TinyUSB CDC callbacks ---- */

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* line_coding)
{
    if (itf >= PROBE_NUM_UARTS)
        return;
    cdc_uart_t *u = &uarts[itf];

    if (itf == 0) {
        if (line_coding->bit_rate == MAGIC_BAUD) {
            if (!autobaud_running)
                autobaud_start();
            return;
        }
        else if (autobaud_running) {
            autobaud_wait_stop();
        }
    }
    uart_parity_t parity;
    uint data_bits, stop_bits;

    /* Modifying state, so park the thread before changing it. */
    bool running = any_active();
    if (running)
        vTaskSuspend(uart_taskhandle);

    cdc_uart_set_baud(u, line_coding->bit_rate);

    switch (line_coding->parity) {
    case CDC_LINE_CODING_PARITY_ODD:
        parity = UART_PARITY_ODD;
        break;
    case CDC_LINE_CODING_PARITY_EVEN:
        parity = UART_PARITY_EVEN;
        break;
    default:
        probe_info("invalid parity setting %u\n", line_coding->parity);
        /* fallthrough */
    case CDC_LINE_CODING_PARITY_NONE:
        parity = UART_PARITY_NONE;
        break;
    }

    switch (line_coding->data_bits) {
    case 5:
    case 6:
    case 7:
    case 8:
        data_bits = line_coding->data_bits;
        break;
    default:
        probe_info("invalid data bits setting: %u\n", line_coding->data_bits);
        data_bits = 8;
        break;
    }

    /* The PL011 only supports 1 or 2 stop bits. 1.5 stop bits is translated to 2,
     * which is safer than the alternative. */
    switch (line_coding->stop_bits) {
    case CDC_LINE_CONDING_STOP_BITS_1_5:
    case CDC_LINE_CONDING_STOP_BITS_2:
        stop_bits = 2;
        break;
    default:
        probe_info("invalid stop bits setting: %u\n", line_coding->stop_bits);
        /* fallthrough */
    case CDC_LINE_CONDING_STOP_BITS_1:
        stop_bits = 1;
        break;
    }

    cdc_uart_set_format(u, data_bits, stop_bits, parity);
    /* Windows likes to arbitrarily set/get line coding after dtr/rts changes, so
     * don't resume if we shouldn't */
    if (running)
        vTaskResume(uart_taskhandle);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    if (itf >= PROBE_NUM_UARTS)
        return;
    cdc_uart_t *u = &uarts[itf];

    if (!u->pins.hwfc && u->pins.rts >= 0)
        gpio_put(u->pins.rts, !rts);
    if (u->pins.dtr >= 0)
        gpio_put(u->pins.dtr, !dtr);

    /* CDC drivers use linestate as a bodge to activate/deactivate the interface.
     * Poll while any port is open, stop when none is */
    u->active = dtr;
    if (!dtr) {
        led_rx(u, 0);
        u->rx_led_debounce = 0;
        led_tx(u, 0);
        u->tx_led_debounce = 0;
    }
    if (any_active())
        vTaskResume(uart_taskhandle);
    else
        vTaskSuspend(uart_taskhandle);
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t wValue)
{
    if (itf >= PROBE_NUM_UARTS)
        return;
    cdc_uart_t *u = &uarts[itf];

    switch(wValue) {
    case 0:
        cdc_uart_break(u, false);
        u->timed_break = false;
        u->tx_led_debounce = 0;
        break;
    case 0xffff:
        cdc_uart_break(u, true);
        u->timed_break = false;
        led_tx(u, 1);
        u->tx_led_debounce = 1 << 30;
        break;
    default:
        cdc_uart_break(u, true);
        u->timed_break = true;
        led_tx(u, 1);
        u->tx_led_debounce = 1 << 30;
        u->break_expiry = xTaskGetTickCount() + (wValue * (configTICK_RATE_HZ / 1000));
        break;
    }
}

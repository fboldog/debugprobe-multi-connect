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
#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "probe_config.h"
#include "probe.h"
#include "DAP_config.h"
#include "DAP.h"
#include "tusb.h"
#include "pio_alloc.h"

#define DIV_ROUND_UP(m, n)	(((m) + (n) - 1) / (n))

// Only want to set / clear one gpio per event so go up in powers of 2
enum _dbg_pins {
    DBG_PIN_WRITE = 1,
    DBG_PIN_WRITE_WAIT = 2,
    DBG_PIN_READ = 4,
    DBG_PIN_PKT = 8,
};

CU_REGISTER_DEBUG_PINS(probe_timing)

// Uncomment to enable debug
//CU_SELECT_DEBUG_PINS(probe_timing)

/*
 * One SWD channel: its pins, the PIO state machine driving them, and the
 * CMSIS-DAP state belonging to the debugger session on this channel.
 *
 * The CMSIS-DAP core (DAP.c) keeps its state in globals, so the DAP thread
 * calls probe_select_channel() before executing a command for a channel,
 * which swaps that state in and out.
 */
typedef struct {
    probe_pins_t pins;
    PIO pio;
    uint sm;
    uint offset;
    bool initted;
    bool led_connected;
    bool led_running;
    // Saved DAP context while another channel is selected
    DAP_Data_t dap_data;
    uint32_t cached_delay;
    uint8_t transfer_abort;
} probe_channel_t;

static const probe_pins_t channel_pins[] = PROBE_CHANNEL_PINS;
static_assert(count_of(channel_pins) >= PROBE_NUM_CHANNELS,
              "PROBE_CHANNEL_PINS has fewer entries than PROBE_NUM_CHANNELS");

static_assert(PROBE_NUM_CHANNELS + PROBE_NUM_PIO_UARTS_MIN * 2 <= PIO_ALLOC_MAX_SMS,
              "SWD channels + PIO UARTs exceed the available PIO state machines");

static probe_channel_t channels[PROBE_NUM_CHANNELS];
static probe_channel_t *probe = &channels[0];
static uint probe_channel_idx;

void probe_set_swclk_freq(uint freq_khz) {
        uint clk_sys_freq_khz = clock_get_hz(clk_sys) / 1000;
        probe_info("Set swclk freq %dKHz sysclk %dkHz\n", freq_khz, clk_sys_freq_khz);
        // Round up (otherwise fast swclks get faster)
        uint32_t divider = (((clk_sys_freq_khz + freq_khz - 1)/ freq_khz) + 3) / 4;
        if (divider == 0)
            divider = 1;
        if (divider > 65535)
            divider = 65535;

        pio_sm_set_clkdiv_int_frac(probe->pio, probe->sm, divider, 0);
}

void probe_assert_reset(bool state)
{
    /* Change the direction to out to drive pin to 0 or to in to emulate open drain */
    if (probe->pins.reset >= 0)
        gpio_set_dir(probe->pins.reset, state == 0 ? GPIO_OUT : GPIO_IN);
}

int probe_reset_level(void)
{
    if (probe->pins.reset >= 0)
        return gpio_get(probe->pins.reset);
    return 0;
}

typedef enum probe_pio_command {
    CMD_WRITE = 0,
    CMD_SKIP,
    CMD_TURNAROUND,
    CMD_READ
} probe_pio_command_t;

static inline uint32_t fmt_probe_command(uint bit_count, bool out_en, probe_pio_command_t cmd) {
    uint cmd_addr =
        cmd == CMD_WRITE      ? probe->offset + probe_offset_write_cmd :
        cmd == CMD_SKIP       ? probe->offset + probe_offset_get_next_cmd :
        cmd == CMD_TURNAROUND ? probe->offset + probe_offset_turnaround_cmd :
                                probe->offset + probe_offset_read_cmd;
    return ((bit_count - 1) & 0xff) | ((uint)out_en << 8) | (cmd_addr << 9);
}

void probe_write_bits(uint bit_count, uint32_t data_byte) {
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE);
    pio_sm_put_blocking(probe->pio, probe->sm, fmt_probe_command(bit_count, true, CMD_WRITE));
    pio_sm_put_blocking(probe->pio, probe->sm, data_byte);
    probe_dump("Write %d bits 0x%x\n", bit_count, data_byte);
    // Return immediately so we can cue up the next command whilst this one runs
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_WRITE);
}

void probe_hiz_clocks(uint bit_count) {
    pio_sm_put_blocking(probe->pio, probe->sm, fmt_probe_command(bit_count, false, CMD_TURNAROUND));
    pio_sm_put_blocking(probe->pio, probe->sm, 0);
}

uint32_t probe_read_bits(uint bit_count) {
    DEBUG_PINS_SET(probe_timing, DBG_PIN_READ);
    pio_sm_put_blocking(probe->pio, probe->sm, fmt_probe_command(bit_count, false, CMD_READ));
    uint32_t data = pio_sm_get_blocking(probe->pio, probe->sm);
    uint32_t data_shifted = data;
    if (bit_count < 32) {
        data_shifted = data >> (32 - bit_count);
    }

    probe_dump("Read %d bits 0x%x (shifted 0x%x)\n", bit_count, data, data_shifted);
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_READ);
    return data_shifted;
}

static void probe_wait_idle() {
    probe->pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + probe->sm);
    while (!(probe->pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + probe->sm))))
        ;
}

void probe_read_mode(void) {
    pio_sm_put_blocking(probe->pio, probe->sm, fmt_probe_command(0, false, CMD_SKIP));
    probe_wait_idle();
}

void probe_write_mode(void) {
    pio_sm_put_blocking(probe->pio, probe->sm, fmt_probe_command(0, true, CMD_SKIP));
    probe_wait_idle();
}

static void probe_gpio_init(const probe_channel_t *ch)
{
    const probe_pins_t *p = &ch->pins;
    if (p->reset >= 0) {
        // Target reset pin: pull up, input to emulate open drain pin
        gpio_pull_up(p->reset);
        // gpio_init will leave the pin cleared and set as input
        gpio_init(p->reset);
    }
    // Funcsel pins
#if defined(PROBE_IO_OEN)
    pio_gpio_init(ch->pio, p->swdioen);
#endif
    pio_gpio_init(ch->pio, p->swclk);
    pio_gpio_init(ch->pio, p->swdio);
    // Make sure SWDIO has a pullup on it. Idle state is high
    gpio_pull_up(p->swdio);
#if defined(PROBE_IO_OEN)
    gpio_pull_up(p->swdioen);
#endif
}

// DAP interface says all pins have to be High-Z when disabled
static void probe_gpio_deinit(const probe_channel_t *ch)
{
    const probe_pins_t *p = &ch->pins;
    if (p->reset >= 0) {
        gpio_deinit(p->reset);
        gpio_disable_pulls(p->reset);
    }
    gpio_deinit(p->swclk);
    gpio_disable_pulls(p->swclk);
#if defined(PROBE_IO_OEN)
    // Make sure the driver output enable is removed before removing drive - leave pull-up enabled
    gpio_deinit(p->swdioen);
#endif
    gpio_deinit(p->swdio);
    gpio_disable_pulls(p->swdio);
}

static void probe_sm_init(const probe_channel_t *ch)
{
    const probe_pins_t *p = &ch->pins;
    pio_sm_config sm_config = probe_program_get_default_config(ch->offset);
    uint32_t out_mask = (1u << p->swclk) | (1u << p->swdio);

#if defined(PROBE_IO_OEN)
    // SWDIOEN and SWCLK are the (consecutive) sideset pins
    sm_config_set_sideset_pins(&sm_config, p->swdioen);
    out_mask |= 1u << p->swdioen;
#else
    // Set SWCLK as a sideset pin
    sm_config_set_sideset_pins(&sm_config, p->swclk);
#endif

    // Set SWDIO offset
    sm_config_set_out_pins(&sm_config, p->swdio, 1);
    sm_config_set_set_pins(&sm_config, p->swdio, 1);
    sm_config_set_in_pins(&sm_config, p->swdi >= 0 ? p->swdi : p->swdio);

    // Set clock and data pins as output to start. SWDIO direction is then driven by the SM
    pio_sm_set_pindirs_with_mask(ch->pio, ch->sm, out_mask, out_mask);

    // shift output right, autopull off, autopull threshold
    sm_config_set_out_shift(&sm_config, true, false, 0);
    // shift input right as swd data is lsb first, autopush off
    sm_config_set_in_shift(&sm_config, true, false, 0);

    pio_sm_init(ch->pio, ch->sm, ch->offset, &sm_config);
}

void probe_init() {
    if (!probe->initted) {
        probe_gpio_init(probe);
        probe_sm_init(probe);

        // Set up divisor
        probe_set_swclk_freq(1000);

        // Jump SM to command dispatch routine, and enable it
        pio_sm_exec(probe->pio, probe->sm, probe->offset + probe_offset_get_next_cmd);
        pio_sm_set_enabled(probe->pio, probe->sm, 1);
        probe->initted = 1;
    }
}

void probe_deinit(void)
{
  if (probe->initted) {
    probe_read_mode();
    pio_sm_set_enabled(probe->pio, probe->sm, 0);

    probe_assert_reset(1);	// de-assert nRESET
    probe_gpio_deinit(probe);
    probe->initted = 0;
  }
}

void probe_channels_init(void)
{
    for (uint i = 0; i < PROBE_NUM_CHANNELS; i++) {
        probe_channel_t *ch = &channels[i];
        ch->pins = channel_pins[i];
        ch->initted = 0;
        // Every channel starts from the default DAP state set up by DAP_Setup()
        ch->dap_data = DAP_Data;
        ch->cached_delay = 0;
        ch->transfer_abort = 0;
        // Claimed up front, so runtime PIO users (autobaud) can't starve us
        if (!pio_alloc_claim(&probe_program, &ch->pio, &ch->sm, &ch->offset))
            panic("No free PIO state machine for SWD channel %u", i);
    }
    probe = &channels[0];
    probe_channel_idx = 0;
}

void probe_select_channel(uint idx)
{
    if (idx == probe_channel_idx || idx >= PROBE_NUM_CHANNELS)
        return;

    probe->dap_data = DAP_Data;
    probe->cached_delay = cached_delay;
    probe->transfer_abort = DAP_TransferAbort;

    probe = &channels[idx];
    probe_channel_idx = idx;

    DAP_Data = probe->dap_data;
    cached_delay = probe->cached_delay;
    DAP_TransferAbort = probe->transfer_abort;
}

uint probe_current_channel(void)
{
    return probe_channel_idx;
}

/* Status LEDs are shared: lit while any channel requests it */
void probe_led_connected(bool on)
{
    probe->led_connected = on;
#ifdef PROBE_DAP_CONNECTED_LED
    bool any = false;
    for (uint i = 0; i < PROBE_NUM_CHANNELS; i++)
        any |= channels[i].led_connected;
    gpio_put(PROBE_DAP_CONNECTED_LED, any);
#endif
}

void probe_led_running(bool on)
{
    probe->led_running = on;
#ifdef PROBE_DAP_RUNNING_LED
    bool any = false;
    for (uint i = 0; i < PROBE_NUM_CHANNELS; i++)
        any |= channels[i].led_running;
    gpio_put(PROBE_DAP_RUNNING_LED, any);
#endif
}

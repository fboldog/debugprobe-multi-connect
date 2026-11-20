/*
 * SPDX-License-Identifier: MIT
 */

#include <pico/stdlib.h>
#include "pio_alloc.h"

#define MAX_LOADED 8

static struct {
    const pio_program_t *prog;
    PIO pio;
    uint offset;
} loaded[MAX_LOADED];
static uint num_loaded;
static uint pio0_claimed;

static int find_or_load(const pio_program_t *prog, PIO pio)
{
    for (uint i = 0; i < num_loaded; i++)
        if (loaded[i].prog == prog && loaded[i].pio == pio)
            return loaded[i].offset;
    if (num_loaded == MAX_LOADED || !pio_can_add_program(pio, prog))
        return -1;
    loaded[num_loaded].prog = prog;
    loaded[num_loaded].pio = pio;
    loaded[num_loaded].offset = pio_add_program(pio, prog);
    return loaded[num_loaded++].offset;
}

bool pio_alloc_claim(const pio_program_t *prog, PIO *pio_out, uint *sm_out, uint *offset_out)
{
    PIO const order[] = {
#if NUM_PIOS > 2
        pio2,
#endif
        pio1, pio0
    };

    for (uint i = 0; i < count_of(order); i++) {
        PIO pio = order[i];

        if (pio == pio0 && pio0_claimed >= NUM_PIO_STATE_MACHINES - 1)
            continue;
        int sm = pio_claim_unused_sm(pio, false);
        if (sm < 0)
            continue;
        int offset = find_or_load(prog, pio);
        if (offset < 0) {
            pio_sm_unclaim(pio, sm);
            continue;
        }
        if (pio == pio0)
            pio0_claimed++;
        *pio_out = pio;
        *sm_out = sm;
        *offset_out = offset;
        return true;
    }
    return false;
}

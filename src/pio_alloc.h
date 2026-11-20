/*
 * SPDX-License-Identifier: MIT
 */

#ifndef PIO_ALLOC_H_
#define PIO_ALLOC_H_

#include <hardware/pio.h>

/*
 * Claim a free PIO state machine with `prog` loaded, sharing a single copy
 * of the program per PIO block. Higher numbered PIO blocks are used first,
 * and one state machine on pio0 is always left free for autobaud.
 * Returns false if no PIO block has both a free SM and room for the program.
 */
bool pio_alloc_claim(const pio_program_t *prog, PIO *pio, uint *sm, uint *offset);

/* PIO state machines available to pio_alloc_claim() */
#define PIO_ALLOC_MAX_SMS (NUM_PIOS * NUM_PIO_STATE_MACHINES - 1)

#endif

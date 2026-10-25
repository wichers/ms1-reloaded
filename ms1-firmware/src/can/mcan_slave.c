/*==========================================================================
 * can/mcan_slave.c - the slave side of the master's view, and the slave's requests
 *
 * Functions:
 *   mcan_request_advance    (0x5E31) - as slave: the master answered a request
 *   mcan_slave_release_all  (0x5EF1) - as master: the slave no longer controls any slot
 *   mcan_slave_lost         (0x5F19) - as master: the slave went silent
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/mcan.h"

/* As slave: if the slot state's request (see mcan_lokstack_sync) is state, the master has
 * answered it: step on to the next request (0x1xx continues at 0x300 + xx + 1, 0x3xx at
 * 0x1xx + 1, others at state + 1) and send it at once (timer 0). */
void mcan_request_advance(lok_state_t *d, u16 state)
{
    u16 s;

    if (is_master != 0 || d->request != state)
        return;
    s = state;
    if ((s & 0xFF00) == 0x100)
        s += 0x1FF;
    else if ((s & 0xFF00) == 0x300)
        s -= 0x200;
    d->request = s + 1;
    d->timer = 0;
}

/* The slots the slave controlled (flags bit 2) are free again, each announced. */
void mcan_slave_release_all(void)
{
    lok_state_t *d = lok_states;
    u8 flags;
    u16 i;

    mcan_peer_handle = 0;
    for (i = 0; i < 10; i++, d++) {
        flags = d->flags;
        if (flags & 0x04) {
            d->flags = flags & ~0x04;
            HI8(d->can_pending) |= 0x04;
        }
    }
}

/* The slave's slots are released and its registration forgotten. */
void mcan_slave_lost(void)
{
    mcan_slave_release_all();
    mcan_peer_uid_lo = 0;
    mcan_peer_uid_hi = 0;
    mcan_peer_node = 0;
    mcan_reg_step = 0;
}

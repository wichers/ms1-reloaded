/*==========================================================================
 * lib/runtime.c - the original's compiler runtime helpers that C code here calls
 *========================================================================*/

#include "../include/hcs12.h"

/* d << x (the original's helper at 0xE109) */
u16 compiler_shift_left_d_by_x(u16 d, u16 x)
{
    return d << x;
}

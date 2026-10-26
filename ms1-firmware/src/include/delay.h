/*==========================================================================
 * delay.h - busy waits (sys/delay.c), timed for the 24 MHz bus
 *========================================================================*/
#ifndef DELAY_H
#define DELAY_H

#include "hcs12.h"

void delay_long(u16 s);                                             /* 0xE072 */
void delay_short(u16 ms);                                           /* 0xE084 */

#endif /* DELAY_H */

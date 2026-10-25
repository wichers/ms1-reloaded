#ifndef TUI_H
#define TUI_H

#include "board.h"
#include "mcu.h"

/* Run interactively until the user quits; the terminal is restored on exit. */
int tui_run(mcu_t *m, ms1_board_t *b);

#endif

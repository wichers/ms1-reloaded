/* The MS1 board around the MC9S12D64: buttons, rotary encoder, LCD driver, analog inputs, track
 * output and the CAN bus. */
#ifndef BOARD_H
#define BOARD_H

#include <stdio.h>

#include "canbus.h"
#include "decoder.h"
#include "mcu.h"

/* LCD driver frame: an address byte with CE low, then a 10-byte block with CE high; four blocks,
 * told apart by the block code in their last two bits. */
#define LCD_BLOCKS 4
#define LCD_BLOCK_BYTES 10
#define LCD_COLS 38
#define LCD_ROWS 5
#define LCD_BAR 14

typedef struct ms1_board {
	mcu_t *m;
	/* inputs: pin levels driven from outside (buttons active low, 1 = released) */
	uint8_t porta, portb, ptt;
	uint16_t atd[8];                 /* ATD0 channel inputs, 10-bit */
	/* LCD driver */
	uint8_t lcd[LCD_BLOCKS][LCD_BLOCK_BYTES];
	uint8_t lcd_rx[16];
	int lcd_n;
	bool lcd_ce;                     /* CE level at the previous byte */
	uint8_t lcd_addr;                /* CCB address of the frame being received */
	unsigned lcd_frames, lcd_bad;
	/* encoder: Gray-code position of the A/B pair, transitions still to make */
	int enc_phase;
	int enc_pending;
	/* track output */
	uint8_t track_level;             /* PT3-PT5 as last driven */
	unsigned long track_edges[3];
	FILE *track_log;                 /* every PT3-PT5 change: time, levels */
	int track_pol;                   /* track voltage: +1 PT4 high, -1 PT5 high, 0 equal */
	decoder_t *decoders[8];          /* locomotive decoders on the track (decoder.c) */
	int ndecoders;
	/* mfx feedback receiver (board.c): RDCL on PT0, RDDA on PTT 1, QUAL on PB5 */
	int rds_carriers;                /* decoders sending the carrier now */
	bool rds_clk;                    /* RDCL level */
	bool rds_hold;                   /* a decoder's bit set the last rising edge */
	/* CAN */
	canbus_t *bus;                   /* attached bus, or NULL */
	bool can_unplugged;              /* cable pulled: no frames go out or come in */
	struct ms1_board *peer;          /* a board in this process on the same bus, or NULL */
	uint64_t enc_next, poll_next, rds_next;   /* board event times (UINT64_MAX: none) */
	FILE *can_log;
	unsigned long can_tx, can_rx;
} ms1_board_t;

void board_init(ms1_board_t *b, mcu_t *m, FILE *can_log);
/* Attach a CAN bus: the MS1's frames go out on it, and it is polled every 0.5 ms of emulated
 * time for frames to the MS1. */
void board_attach_bus(ms1_board_t *b, canbus_t *bus);
/* Attach a locomotive decoder: it sees every change of the track voltage. */
void board_attach_decoder(ms1_board_t *b, decoder_t *d);
/* The feedback path for decoders created for this board: the MS1's mfx feedback receiver. */
decoder_feedback_t board_feedback(ms1_board_t *b);
/* Render the LCD (38x5 matrix, speed bar, other lit segments) as text. */
void board_lcd_print(ms1_board_t *b, FILE *out);
/* Lit icons and arrows, space-separated ('*' marks the "on" variant of a function icon). */
void board_lcd_icons(ms1_board_t *b, char *out, size_t size);
/* The matrix read back as text with the firmware's font. */
void board_lcd_text(ms1_board_t *b, char *out, size_t size);
/* One pixel of the 38x5 matrix / one speed bar segment, as the glass shows it now. */
bool board_lcd_pixel(ms1_board_t *b, int row, int col);
bool board_lcd_bar(ms1_board_t *b, int i);
/* Buttons: index 0-7 = PA0-PA7, 8-12 = PB0-PB4. */
void board_button(ms1_board_t *b, int index, bool pressed);
/* Turn the encoder by detents (positive = clockwise). A detent is half a quadrature cycle -
 * two transitions, ENC_TRANSITION_S apart - as on the MS1, where the firmware adds
 * encoder_step per transition and moves one menu step per 4 counts
 * (ui_knob_adjust). */
void board_encoder_step(ms1_board_t *b, int dir);
/* A frame from the CAN bus to the MS1 (false: not taken). */
bool board_can_receive(ms1_board_t *b, const uint8_t idr[4], uint8_t dlc, const uint8_t *data);
/* Pull (false) or plug in (true) the CAN cable: frames to and from the MS1 are lost while it
 * is out; the MS1's controller sees an error-free bus as before. */
void board_can_plug(ms1_board_t *b, bool plugged);

#endif

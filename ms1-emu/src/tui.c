/* Interactive front end: runs the emulation in step with the wall clock, draws the LCD in the
 * terminal (ANSI escapes) and turns key presses into MS1 control inputs. Terminals report no
 * key releases, so a button is released 150 ms after its key. */
#define _POSIX_C_SOURCE 200809L
#include "tui.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios saved;
static bool raw_on;

static void restore(void)
{
	if (raw_on) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		fputs("\033[?25h\033[?1049l", stdout);   /* cursor on, main screen */
		fflush(stdout);
		raw_on = false;
	}
}

static void on_signal(int sig)
{
	restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void raw_mode(void)
{
	struct termios t;
	tcgetattr(STDIN_FILENO, &saved);
	t = saved;
	t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
	raw_on = true;
	atexit(restore);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	fputs("\033[?1049h\033[?25l\033[2J", stdout);   /* alternate screen, cursor off */
}

static double wall(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define NBUTTONS 13
static double release_at[NBUTTONS];

static void press(ms1_board_t *b, int idx, double now)
{
	board_button(b, idx, true);
	release_at[idx] = now + 0.15;
}

static const char *const button_label[NBUTTONS] = {
	"PA0", "PA1", "PA2", "PA3", "PA4", "PA5", "PA6", "PA7", "ESC", "OK", "LIGHT", "STOP", "KNOB",
};

static void draw(mcu_t *m, ms1_board_t *b, double ratio, bool fast)
{
	char buf[8192];
	int n = 0;
	n += snprintf(buf + n, sizeof buf - (size_t)n, "\033[H MS1 emulator — t=%.1fs  %s%.2fx real time   \033[K\n\n",
		mcu_seconds(m, m->now), fast ? "fast " : "", ratio);
	n += snprintf(buf + n, sizeof buf - (size_t)n, "   +");
	for (int c = 0; c < LCD_COLS; c++)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "--");
	n += snprintf(buf + n, sizeof buf - (size_t)n, "+\n");
	for (int r = 0; r < LCD_ROWS; r++) {
		n += snprintf(buf + n, sizeof buf - (size_t)n, "   |");
		for (int c = 0; c < LCD_COLS; c++)
			n += snprintf(buf + n, sizeof buf - (size_t)n, "%s", board_lcd_pixel(b, r, c) ? "██" : " ·");
		n += snprintf(buf + n, sizeof buf - (size_t)n, "|\n");
	}
	n += snprintf(buf + n, sizeof buf - (size_t)n, "   +");
	for (int c = 0; c < LCD_COLS; c++)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "--");
	char text[64];
	board_lcd_text(b, text, sizeof text);
	n += snprintf(buf + n, sizeof buf - (size_t)n, "+  \"%s\"\033[K\n   speed [", text);
	for (int i = 0; i < LCD_BAR; i++)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "%s", board_lcd_bar(b, i) ? "█" : " ");
	char icons[512];
	board_lcd_icons(b, icons, sizeof icons);
	n += snprintf(buf + n, sizeof buf - (size_t)n, "]   icons: %s\033[K\n   buttons held:", icons[0] ? icons : "-");
	for (int i = 0; i < NBUTTONS; i++)
		if (release_at[i] > 0)
			n += snprintf(buf + n, sizeof buf - (size_t)n, " %s", button_label[i]);
	n += snprintf(buf + n, sizeof buf - (size_t)n, "\033[K\n\n   CAN tx %lu rx %lu   track edges PT4 %lu   PC %04X%s\033[K\n",
		b->can_tx, b->can_rx, b->track_edges[1], m->cpu.pc, m->cpu.state == CPU12_HALT ? " HALTED" : "");
	for (int i = 0; i < b->ndecoders; i++) {
		char line[160];
		decoder_status(b->decoders[i], line, sizeof line);
		n += snprintf(buf + n, sizeof buf - (size_t)n, "   decoder %s\033[K\n", line);
	}
	n += snprintf(buf + n, sizeof buf - (size_t)n, "\n");
	n += snprintf(buf + n, sizeof buf - (size_t)n,
		"   ←/→ or -/+  turn knob      space  push knob     enter/o  Locomotive OK\n"
		"   m or esc     Menu/ESC       s      STOP          l        headlights\n"
		"   1-8          PORTA keys     f      fast/real time            q  quit\033[K\n");
	fwrite(buf, 1, (size_t)n, stdout);
	fflush(stdout);
}

int tui_run(mcu_t *m, ms1_board_t *b)
{
	raw_mode();
	double w0 = wall(), e0 = mcu_seconds(m, m->now), last_draw = 0, rate_w = w0, rate_e = e0, ratio = 1;
	bool fast = false, quit = false;
	while (!quit) {
		double w = wall();
		/* keys */
		unsigned char k[16];
		ssize_t got = read(STDIN_FILENO, k, sizeof k);
		for (ssize_t i = 0; i < got; i++) {
			unsigned char c = k[i];
			if (c == 0x1B && i + 2 < got && k[i + 1] == '[') {        /* arrow keys */
				if (k[i + 2] == 'C') board_encoder_step(b, 1);
				else if (k[i + 2] == 'D') board_encoder_step(b, -1);
				i += 2;
				continue;
			}
			switch (c) {
			case 0x1B: case 'm': press(b, 8, w); break;
			case '+': case '=': board_encoder_step(b, 1); break;
			case '-': case '_': board_encoder_step(b, -1); break;
			case ' ': press(b, 12, w); break;
			case '\n': case '\r': case 'o': press(b, 9, w); break;
			case 's': press(b, 11, w); break;
			case 'l': press(b, 10, w); break;
			case 'f': fast = !fast; w0 = w; e0 = mcu_seconds(m, m->now); break;
			case 'q': case 3: quit = true; break;
			default:
				if (c >= '1' && c <= '8')
					press(b, c - '1', w);
			}
		}
		for (int i = 0; i < NBUTTONS; i++)
			if (release_at[i] > 0 && w >= release_at[i]) {
				board_button(b, i, false);
				release_at[i] = 0;
			}
		/* run: to the wall-clock target, or 50 ms of emulated time per round when fast */
		double target = fast ? mcu_seconds(m, m->now) + 0.05 : e0 + (w - w0);
		if (target > mcu_seconds(m, m->now) && m->cpu.state != CPU12_HALT)
			mcu_run(m, mcu_ticks(m, target));
		if (w - rate_w >= 0.5) {
			ratio = (mcu_seconds(m, m->now) - rate_e) / (w - rate_w);
			rate_w = w;
			rate_e = mcu_seconds(m, m->now);
		}
		if (w - last_draw >= 0.05) {
			draw(m, b, ratio, fast);
			last_draw = w;
		}
		if (!fast) {
			struct timespec ts = { 0, 5000000 };
			nanosleep(&ts, 0);
		}
	}
	restore();
	return 0;
}

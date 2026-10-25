/* A CAN master for the emulated MS1: the ms1-master library (../ms1-master, an M-CAN master
 * for MS1 slaves) on a CAN bus the emulator also attaches to (-C), in real time. It offers one
 * locomotive and assigns it to every slave that completes registration; protocol events are
 * printed.
 *   master_bridge BUS [seconds]       BUS as for ms1emu -C (can0, vcan0, udp:PORT)
 * Build: make build/master_bridge (MS1_MASTER = path to ms1-master). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/canbus.h"
#include "ms1_master.h"

static canbus_t bus;
static ms1_master_t master;
static struct timespec t0;

static double elapsed(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}

static void print_frame(const char *dir, const ms1_can_frame_t *f)
{
	printf("%10.6f %s %08X [%u]", elapsed(), dir, (unsigned)f->id, f->dlc);
	for (int i = 0; i < f->dlc && i < 8; i++)
		printf(" %02X", f->data[i]);
	printf("\n");
}

static bool can_send(const ms1_can_frame_t *f)
{
	canbus_send(&bus, f->id, true, f->dlc, f->data);
	print_frame("TX", f);
	return true;
}

static bool can_receive(ms1_can_frame_t *f)
{
	uint32_t id;
	bool ext;
	uint8_t dlc;
	if (!canbus_recv(&bus, &id, &ext, &dlc, f->data))
		return false;
	f->id = id;
	f->dlc = dlc;
	print_frame("RX", f);
	return true;
}

static uint32_t millis(void) { return (uint32_t)(elapsed() * 1000); }
static void log_msg(const char *msg) { printf("%10.6f master: %s\n", elapsed(), msg); }

static void on_connected(void *ctx, uint8_t slave)
{
	(void)ctx;
	printf("%10.6f slave %u connected; assigning lok 0\n", elapsed(), slave);
	ms1_master_assign_lok(&master, slave, 0);
}

static void on_speed(void *ctx, uint8_t slave, uint8_t lok, uint8_t sd)
{
	(void)ctx;
	printf("%10.6f slave %u lok %u speed/dir %02X\n", elapsed(), slave, lok, sd);
}

static void on_function(void *ctx, uint8_t slave, uint8_t lok, uint8_t fn, uint8_t v)
{
	(void)ctx;
	printf("%10.6f slave %u lok %u function %u = %u\n", elapsed(), slave, lok, fn, v);
}

static void on_stop(void *ctx, uint8_t slave) { (void)ctx; printf("%10.6f slave %u STOP\n", elapsed(), slave); }
static void on_go(void *ctx, uint8_t slave) { (void)ctx; printf("%10.6f slave %u GO\n", elapsed(), slave); }

int main(int argc, char **argv)
{
	char err[256];
	if (argc < 2) {
		fprintf(stderr, "usage: master_bridge BUS [seconds]\n");
		return 2;
	}
	double seconds = argc > 2 ? atof(argv[2]) : 1e9;
	if (canbus_open(&bus, argv[1], err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	setvbuf(stdout, 0, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	static ms1_platform_t platform = { can_send, can_receive, millis, log_msg };
	ms1_master_config_t cfg = {
		.uid = { 0x00, 0x11, 0x22, 0x33 }, .hw_version = 0x4010, .sw_version = 0x0301,
		.vendor_id = MS1_VENDOR_ESU, .master_type = MS1_MASTER_TYPE_CS1, .node_base = 42,
	};
	ms1_master_callbacks_t cb = { 0 };
	cb.on_slave_connected = on_connected;
	cb.on_lok_speed = on_speed;
	cb.on_lok_function = on_function;
	cb.on_stop = on_stop;
	cb.on_go = on_go;
	ms1_master_init(&master, &platform, &cfg, &cb);
	ms1_lok_t lok = { 0 };
	strcpy(lok.name, "BR 44");
	lok.type = MS1_LOK_TYPE_STEAM;
	lok.format = MS1_RAIL_MM;
	lok.func_count = 2;
	lok.funcs[0] = (ms1_lok_func_t){ .number = 1, .activation = MS1_FUNC_SPEED };
	lok.funcs[1] = (ms1_lok_func_t){ .number = 2, .activation = MS1_FUNC_MAINTAINED, .icon = MS1_ICON_01 };
	ms1_master_set_lok(&master, 0, &lok);
	while (elapsed() < seconds) {
		ms1_master_poll(&master);
		struct timespec ts = { 0, 500000 };
		nanosleep(&ts, 0);
	}
	return 0;
}

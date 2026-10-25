/* A CAN master for the emulated MS1: the ms1-master library (../ms1-master, an M-CAN master
 * for MS1 slaves) on a CAN bus the emulator also attaches to (-C), in real time. It offers two
 * locomotives and assigns both to every slave that completes registration; protocol events are
 * printed, and a script can act as the master's operator.
 *   master_bridge [-n NODE_BASE] BUS [SECONDS [SCRIPT]]
 *     BUS        as for ms1emu -C (can0, vcan0, udp:PORT)
 *     NODE_BASE  master node of the first slave (default 42, as an ECoS; 0 as an MS1 master)
 *     SCRIPT     lines 'SECONDS ACTION', in time order ('#' starts a comment):
 *                  stop | go                      system Stop / Go
 *                  speed LOK VALUE                drive a lok (VALUE: bit 7 reverse)
 *                  function LOK NUMBER 0|1        switch a function
 *                  assign SLAVE LOK | unassign SLAVE LOK
 *                  rename SLAVE NAME
 * Build: make build/master_bridge (MS1_MASTER = path to ms1-master). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
	ms1_slave_info_t info;
	ms1_master_get_slave_info(&master, slave, &info);
	printf("%10.6f slave %u connected: UID %02X%02X%02X%02X, nodes %u/%u; assigning loks 0 and 1\n",
		elapsed(), slave, info.uid[0], info.uid[1], info.uid[2], info.uid[3], info.master_node,
		info.slave_node);
	ms1_master_assign_lok(&master, slave, 0);
	ms1_master_assign_lok(&master, slave, 1);
}

static void on_disconnected(void *ctx, uint8_t slave)
{
	(void)ctx;
	printf("%10.6f slave %u disconnected\n", elapsed(), slave);
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

static void on_selected(void *ctx, uint8_t slave, int8_t lok)
{
	(void)ctx;
	if (lok < 0)
		printf("%10.6f slave %u released its lok\n", elapsed(), slave);
	else
		printf("%10.6f slave %u took lok %d\n", elapsed(), slave, lok);
}

static void on_stop(void *ctx, uint8_t slave) { (void)ctx; printf("%10.6f slave %u STOP\n", elapsed(), slave); }
static void on_go(void *ctx, uint8_t slave) { (void)ctx; printf("%10.6f slave %u GO\n", elapsed(), slave); }

/* One script action; 0 if the line is not understood */
static int act(const char *line)
{
	char what[16], name[32];
	unsigned a, b, c;
	if (sscanf(line, "%15s", what) != 1)
		return 0;
	if (!strcmp(what, "stop") || !strcmp(what, "go")) {
		ms1_master_set_stop(&master, what[0] == 's');
	} else if (!strcmp(what, "speed") && sscanf(line, "%*s %u %x", &a, &b) == 2) {
		ms1_master_drive_lok(&master, (uint8_t)a, (uint8_t)b);
	} else if (!strcmp(what, "function") && sscanf(line, "%*s %u %u %u", &a, &b, &c) == 3) {
		ms1_master_drive_function(&master, (uint8_t)a, (uint8_t)b, (uint8_t)c);
	} else if (!strcmp(what, "assign") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
		ms1_master_assign_lok(&master, (uint8_t)a, (uint8_t)b);
	} else if (!strcmp(what, "unassign") && sscanf(line, "%*s %u %u", &a, &b) == 2) {
		ms1_master_unassign_lok(&master, (uint8_t)a, (uint8_t)b);
	} else if (!strcmp(what, "rename") && sscanf(line, "%*s %u %31[^\n]", &a, name) == 2) {
		ms1_master_rename_slave(&master, (uint8_t)a, name);
	} else {
		return 0;
	}
	printf("%10.6f script: %s\n", elapsed(), line);
	return 1;
}

int main(int argc, char **argv)
{
	char err[256];
	int node_base = 42, opt;
	while ((opt = getopt(argc, argv, "n:")) != -1) {
		if (opt != 'n') {
			fprintf(stderr, "usage: master_bridge [-n NODE_BASE] BUS [SECONDS [SCRIPT]]\n");
			return 2;
		}
		node_base = atoi(optarg);
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: master_bridge [-n NODE_BASE] BUS [SECONDS [SCRIPT]]\n");
		return 2;
	}
	double seconds = optind + 1 < argc ? atof(argv[optind + 1]) : 1e9;
	FILE *script = 0;
	if (optind + 2 < argc && !(script = fopen(argv[optind + 2], "r"))) {
		perror(argv[optind + 2]);
		return 1;
	}
	if (canbus_open(&bus, argv[optind], err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	setvbuf(stdout, 0, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	static ms1_platform_t platform = { can_send, can_receive, millis, log_msg };
	ms1_master_config_t cfg = {
		.uid = { 0x00, 0x11, 0x22, 0x33 }, .hw_version = 0x4010, .sw_version = 0x0301,
		.vendor_id = MS1_VENDOR_ESU, .master_type = MS1_MASTER_TYPE_ECOS, .node_base = (uint8_t)node_base,
	};
	ms1_master_callbacks_t cb = { 0 };
	cb.on_slave_connected = on_connected;
	cb.on_slave_disconnected = on_disconnected;
	cb.on_lok_speed = on_speed;
	cb.on_lok_function = on_function;
	cb.on_lok_selected = on_selected;
	cb.on_stop = on_stop;
	cb.on_go = on_go;
	ms1_master_init(&master, &platform, &cfg, &cb);

	ms1_lok_t lok = { 0 };
	strcpy(lok.name, "BR 44");
	lok.type = MS1_LOK_TYPE_STEAM;
	lok.format = MS1_RAIL_MM;
	lok.func_count = 3;
	lok.funcs[0] = (ms1_lok_func_t){ .number = 1, .activation = MS1_FUNC_SPEED };
	lok.funcs[1] = (ms1_lok_func_t){ .number = 2, .activation = MS1_FUNC_MAINTAINED, .icon = MS1_ICON_01 };
	lok.funcs[2] = (ms1_lok_func_t){ .number = 3, .activation = MS1_FUNC_MAINTAINED, .icon = MS1_ICON_10 };
	ms1_master_set_lok(&master, 0, &lok);
	memset(&lok, 0, sizeof lok);
	strcpy(lok.name, "V 200 TEST LOK");
	lok.type = MS1_LOK_TYPE_DIESEL;
	lok.format = MS1_RAIL_MFX;
	lok.func_count = 2;
	lok.funcs[0] = (ms1_lok_func_t){ .number = 1, .activation = MS1_FUNC_SPEED };
	lok.funcs[1] = (ms1_lok_func_t){ .number = 2, .activation = MS1_FUNC_MOMENTARY, .icon = MS1_ICON_02 };
	ms1_master_set_lok(&master, 1, &lok);

	char line[128];
	double next = -1;
	for (;;) {
		while (script && next < 0 && fgets(line, sizeof line, script)) {
			char *hash = strchr(line, '#');
			if (hash)
				*hash = 0;
			line[strcspn(line, "\n")] = 0;
			int pos;
			if (sscanf(line, "%lf %n", &next, &pos) == 1) {
				memmove(line, line + pos, strlen(line + pos) + 1);
				break;
			}
			next = -1;
		}
		if (next >= 0 && elapsed() >= next) {
			if (!act(line))
				fprintf(stderr, "script: cannot do '%s'\n", line);
			next = -1;
		}
		if (elapsed() >= seconds)
			break;
		ms1_master_poll(&master);
		struct timespec ts = { 0, 500000 };
		nanosleep(&ts, 0);
	}
	return 0;
}

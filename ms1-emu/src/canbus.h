#ifndef CANBUS_H
#define CANBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct canbus {
	int fd;          /* -1: not attached */
	bool udp;
	int port;
	uint32_t tag;    /* identifies our own UDP frames when they loop back */
} canbus_t;

/* spec: a SocketCAN interface name ("can0", "vcan0") or "udp:PORT" (local multicast bus). */
int canbus_open(canbus_t *bus, const char *spec, char *err, size_t errsize);
void canbus_close(canbus_t *bus);
void canbus_send(canbus_t *bus, uint32_t id, bool ext, uint8_t dlc, const uint8_t *data);
/* One received frame, non-blocking: 1 if one was read, 0 if none waiting. */
int canbus_recv(canbus_t *bus, uint32_t *id, bool *ext, uint8_t *dlc, uint8_t data[8]);
uint32_t canbus_idr_to_id(const uint8_t idr[4], bool *ext);
void canbus_id_to_idr(uint32_t id, bool ext, uint8_t idr[4]);

#endif

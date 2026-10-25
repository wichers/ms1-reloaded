/* CAN bus transports: Linux SocketCAN (a real adapter or vcan), or a local UDP multicast group
 * that several emulator processes join as a shared virtual bus (no privileges needed).
 * UDP datagram: 4-byte CAN ID (bit 31 = extended), DLC, 8 data bytes, 4-byte sender tag. */
#define _DEFAULT_SOURCE
#include "canbus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UDP_GROUP "239.255.12.1"

int canbus_open(canbus_t *bus, const char *spec, char *err, size_t errsize)
{
	memset(bus, 0, sizeof *bus);
	bus->fd = -1;
	if (!strncmp(spec, "udp:", 4)) {
		int port = atoi(spec + 4);
		int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
		struct ip_mreq mr;
		unsigned char loop = 1, ttl = 0;
		if (fd < 0 || port <= 0 || port > 65535) {
			snprintf(err, errsize, "%s: bad UDP bus", spec);
			return -1;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
			snprintf(err, errsize, "%s: bind: %s", spec, strerror(errno));
			close(fd);
			return -1;
		}
		inet_pton(AF_INET, UDP_GROUP, &mr.imr_multiaddr);
		mr.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
		struct in_addr lo = { htonl(INADDR_LOOPBACK) };
		if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0 ||
		    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &lo, sizeof lo) < 0) {
			snprintf(err, errsize, "%s: multicast: %s", spec, strerror(errno));
			close(fd);
			return -1;
		}
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
		fcntl(fd, F_SETFL, O_NONBLOCK);
		bus->fd = fd;
		bus->udp = true;
		bus->port = port;
		bus->tag = (uint32_t)getpid() ^ (uint32_t)time(0) << 16;
		return 0;
	}
	int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	struct ifreq ifr;
	struct sockaddr_can addr = { .can_family = AF_CAN };
	if (fd < 0) {
		snprintf(err, errsize, "SocketCAN: %s", strerror(errno));
		return -1;
	}
	snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", spec);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		snprintf(err, errsize, "%s: %s", spec, strerror(errno));
		close(fd);
		return -1;
	}
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		snprintf(err, errsize, "%s: bind: %s", spec, strerror(errno));
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	bus->fd = fd;
	return 0;
}

void canbus_close(canbus_t *bus)
{
	if (bus->fd >= 0)
		close(bus->fd);
	bus->fd = -1;
}

/* MSCAN identifier registers <-> CAN ID (S12MSCANV2 IDR layout). */
uint32_t canbus_idr_to_id(const uint8_t idr[4], bool *ext)
{
	*ext = idr[1] & 0x08;
	if (*ext)
		return (uint32_t)idr[0] << 21 | (uint32_t)(idr[1] & 0xE0) << 13 | (uint32_t)(idr[1] & 0x07) << 15 |
			(uint32_t)idr[2] << 7 | (uint32_t)idr[3] >> 1;
	return (uint32_t)idr[0] << 3 | idr[1] >> 5;
}

void canbus_id_to_idr(uint32_t id, bool ext, uint8_t idr[4])
{
	if (ext) {
		idr[0] = (uint8_t)(id >> 21);
		idr[1] = (uint8_t)((id >> 13 & 0xE0) | 0x18 | (id >> 15 & 0x07));   /* SRR, IDE */
		idr[2] = (uint8_t)(id >> 7);
		idr[3] = (uint8_t)(id << 1);
	} else {
		idr[0] = (uint8_t)(id >> 3);
		idr[1] = (uint8_t)(id << 5);
		idr[2] = idr[3] = 0;
	}
}

void canbus_send(canbus_t *bus, uint32_t id, bool ext, uint8_t dlc, const uint8_t *data)
{
	if (bus->fd < 0)
		return;
	if (dlc > 8)
		dlc = 8;
	if (bus->udp) {
		uint8_t pkt[17];
		uint32_t w = id | (ext ? 0x80000000u : 0);
		pkt[0] = (uint8_t)(w >> 24); pkt[1] = (uint8_t)(w >> 16); pkt[2] = (uint8_t)(w >> 8); pkt[3] = (uint8_t)w;
		pkt[4] = dlc;
		memset(pkt + 5, 0, 8);
		memcpy(pkt + 5, data, dlc);
		memcpy(pkt + 13, &bus->tag, 4);
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)bus->port) };
		inet_pton(AF_INET, UDP_GROUP, &a.sin_addr);
		sendto(bus->fd, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a);
		return;
	}
	struct can_frame f = { .can_id = ext ? (id | CAN_EFF_FLAG) : id, .can_dlc = dlc };
	memcpy(f.data, data, dlc);
	if (write(bus->fd, &f, sizeof f) < 0 && errno != EAGAIN)
		perror("CAN write");
}

int canbus_recv(canbus_t *bus, uint32_t *id, bool *ext, uint8_t *dlc, uint8_t data[8])
{
	if (bus->fd < 0)
		return 0;
	for (;;) {
		if (bus->udp) {
			uint8_t pkt[17];
			ssize_t n = recv(bus->fd, pkt, sizeof pkt, 0);
			if (n != (ssize_t)sizeof pkt)
				return 0;
			uint32_t tag;
			memcpy(&tag, pkt + 13, 4);
			if (tag == bus->tag)
				continue;                     /* our own frame looped back */
			uint32_t w = (uint32_t)pkt[0] << 24 | (uint32_t)pkt[1] << 16 | (uint32_t)pkt[2] << 8 | pkt[3];
			*ext = w & 0x80000000u;
			*id = w & 0x1FFFFFFF;
			*dlc = pkt[4] > 8 ? 8 : pkt[4];
			memcpy(data, pkt + 5, 8);
			return 1;
		}
		struct can_frame f;
		ssize_t n = read(bus->fd, &f, sizeof f);
		if (n != (ssize_t)sizeof f)
			return 0;
		if (f.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG))
			continue;
		*ext = f.can_id & CAN_EFF_FLAG;
		*id = f.can_id & (*ext ? CAN_EFF_MASK : CAN_SFF_MASK);
		*dlc = f.can_dlc > 8 ? 8 : f.can_dlc;
		memset(data, 0, 8);
		memcpy(data, f.data, *dlc);
		return 1;
	}
}

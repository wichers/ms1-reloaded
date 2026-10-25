/**
 * @file ms1_platform.h
 * @brief Platform abstraction layer for M-CAN protocol.
 *
 * This header defines the interface that must be implemented by each
 * platform (microcontroller) to provide CAN bus access and timing
 * services to the M-CAN protocol stack.
 *
 * The M-CAN protocol uses CAN 2.0B (29-bit extended IDs) at 250 kBaud.
 */

#ifndef MS1_PLATFORM_H
#define MS1_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CAN frame as seen by the M-CAN protocol layer. */
typedef struct {
    uint32_t id;        /**< 29-bit extended CAN ID (bits 28..0 used) */
    uint8_t  dlc;       /**< Data length code (0..8) */
    uint8_t  data[8];   /**< Payload bytes */
} ms1_can_frame_t;

/**
 * @brief Platform interface structure.
 *
 * The user must fill in all function pointers before calling
 * ms1_master_init() or ms1_slave_init().
 */
typedef struct {
    /**
     * Send a CAN frame on the bus.
     * @param frame  Pointer to the frame to send.
     * @return true on success, false if the transmit buffer is full.
     */
    bool (*can_send)(const ms1_can_frame_t *frame);

    /**
     * Receive a CAN frame from the bus (non-blocking).
     * @param frame  Pointer to buffer where the received frame is stored.
     * @return true if a frame was available, false if the RX queue is empty.
     */
    bool (*can_receive)(ms1_can_frame_t *frame);

    /**
     * Get the current time in milliseconds (free-running, wrapping is OK).
     * Used for timeouts and cyclic polling intervals.
     */
    uint32_t (*millis)(void);

    /**
     * Optional: called by the protocol stack when a debug/log message
     * is available. May be NULL.
     * @param msg  Null-terminated log string.
     */
    void (*log)(const char *msg);
} ms1_platform_t;

#ifdef __cplusplus
}
#endif

#endif /* MS1_PLATFORM_H */

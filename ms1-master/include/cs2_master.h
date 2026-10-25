/**
 * @file cs2_master.h
 * @brief CS2 CAN protocol master — API, types, and callbacks.
 *
 * This header defines the master (CS2-GUI central station) interface
 * for controlling MS2 throttles, Gleisboxes, and other CS2-compatible
 * devices on the CAN bus.
 *
 * The master responds to pings, manages device discovery, serves
 * locomotive data, processes lok/accessory/system commands, and
 * streams configuration data.
 */

#ifndef CS2_MASTER_H
#define CS2_MASTER_H

#include "cs2_types.h"
#include "ms1_platform.h"
#include "ms1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Master state
 * ====================================================================== */

typedef enum {
    CS2_STATE_IDLE,         /**< Not yet started */
    CS2_STATE_DISCOVERING,  /**< Initial ping broadcast sent */
    CS2_STATE_RUNNING,      /**< Normal operation (Go) */
    CS2_STATE_STOPPED,      /**< Track power off (Stop) */
} cs2_master_state_t;

/* ========================================================================
 * Config data stream state
 * ====================================================================== */

typedef enum {
    CS2_CFG_IDLE,       /**< No config stream active */
    CS2_CFG_SENDING,    /**< Sending config data to a peer */
    CS2_CFG_RECEIVING,  /**< Receiving config data from a peer */
} cs2_config_state_t;

/* ========================================================================
 * Device (bus participant) discovered via ping
 * ====================================================================== */

typedef struct {
    bool     present;        /**< Slot in use */
    uint32_t uid;            /**< Device UID (data bytes 0-3 of ping response) */
    uint16_t sw_version;     /**< Software version (data bytes 4-5) */
    uint16_t device_type;    /**< Device type (data bytes 6-7) */
    uint16_t hash;           /**< Device's hash (from CAN ID) */
    uint32_t last_seen_ms;   /**< Timestamp of last activity */
} cs2_device_t;

/* ========================================================================
 * Locomotive
 * ====================================================================== */

typedef struct {
    char            name[CS2_LOK_NAME_LEN + 1];  /**< Null-terminated name */
    uint32_t        locid;       /**< 32-bit Loc-ID (protocol base + address) */
    cs2_protocol_t  protocol;    /**< Track protocol (MM, MFX, DCC, SX1) */
    uint16_t        address;     /**< Protocol-specific address */
    uint16_t        speed;       /**< Current speed (0-1000) */
    cs2_direction_t direction;   /**< Current direction */
    uint32_t        functions;   /**< Bitmask of F0-F31 states */
    uint8_t         func_count;  /**< Number of functions this lok supports */
    bool            active;      /**< Slot in use */
} cs2_lok_t;

/* ========================================================================
 * Accessory
 * ====================================================================== */

typedef struct {
    uint32_t locid;        /**< Accessory Loc-ID */
    uint8_t  position;     /**< Current position (0-255) */
    uint8_t  power;        /**< Power/current (0-31) */
    uint16_t switch_time;  /**< Switch time in 10ms steps */
    bool     active;       /**< Slot in use */
} cs2_accessory_t;

/* ========================================================================
 * Config data stream context
 * ====================================================================== */

typedef struct {
    cs2_config_state_t state;
    uint16_t peer_hash;        /**< Hash of peer we're streaming to/from */
    uint32_t total_length;     /**< Total data length */
    uint16_t crc;              /**< CRC-16 of data */
    uint32_t offset;           /**< Current send/receive offset */
    uint8_t  buffer[CS2_CONFIG_BUF_SIZE];
    uint32_t buffer_used;      /**< Bytes stored in buffer */
    uint32_t last_activity_ms; /**< Timeout tracking */
    char     filename[9];      /**< 8-char filename + null */
} cs2_config_stream_t;

/* ========================================================================
 * Master configuration (passed to init)
 * ====================================================================== */

typedef struct {
    uint32_t uid;           /**< This device's UID */
    uint16_t sw_version;    /**< Software version (e.g. 0x0301 = 3.01) */
    uint16_t device_type;   /**< Device type (typically CS2_DEV_CS2_GUI) */
} cs2_master_config_t;

/* ========================================================================
 * Callbacks — application is notified of protocol events
 * ====================================================================== */

typedef struct {
    void *ctx;  /**< Opaque context pointer passed to all callbacks */

    /** Device discovered via ping response. */
    void (*on_device_found)(void *ctx, uint8_t dev_idx, const cs2_device_t *dev);

    /** Device timed out (no ping/activity). */
    void (*on_device_lost)(void *ctx, uint8_t dev_idx, const cs2_device_t *dev);

    /** Lok speed changed (from bus command). */
    void (*on_lok_speed)(void *ctx, uint32_t locid, uint16_t speed);

    /** Lok direction changed. */
    void (*on_lok_direction)(void *ctx, uint32_t locid, cs2_direction_t dir);

    /** Lok function toggled. */
    void (*on_lok_function)(void *ctx, uint32_t locid, uint8_t func_nr, uint8_t value);

    /** System stop received (track power off). */
    void (*on_system_stop)(void *ctx);

    /** System go received (track power on). */
    void (*on_system_go)(void *ctx);

    /** System halt received (all loks speed 0). */
    void (*on_system_halt)(void *ctx);

    /** Overload/short circuit on a channel. */
    void (*on_overload)(void *ctx, uint8_t channel);

    /** Emergency stop for a specific lok. */
    void (*on_emergency_stop)(void *ctx, uint32_t locid);

    /** Accessory switch command received. */
    void (*on_accessory_switch)(void *ctx, uint32_t locid, uint8_t position,
                                uint8_t power);

    /** S88 feedback event. */
    void (*on_s88_event)(void *ctx, uint16_t device_id, uint16_t contact,
                         uint8_t old_state, uint8_t new_state);

    /** Config data requested by a peer (filename). */
    void (*on_config_data_request)(void *ctx, const char *filename);

    /** Config data fully received from a peer. */
    void (*on_config_data_received)(void *ctx, const char *filename,
                                    const uint8_t *data, uint32_t length);

    /** Firmware update offer received. Return true to accept. */
    bool (*on_update_offer)(void *ctx, uint32_t uid, const uint8_t *data);
} cs2_master_callbacks_t;

/* ========================================================================
 * Master context
 * ====================================================================== */

typedef struct {
    const ms1_platform_t      *platform;
    cs2_master_config_t        config;
    cs2_master_callbacks_t     callbacks;

    uint16_t                   hash;       /**< Our computed hash */
    cs2_master_state_t         state;

    cs2_device_t               devices[CS2_MAX_DEVICES];
    cs2_lok_t                  loks[CS2_MAX_LOKS];
    cs2_accessory_t            accessories[CS2_MAX_ACCESSORIES];

    bool                       stopped;    /**< Track power off */
    bool                       halt;       /**< All loks halted */

    uint32_t                   last_ping_ms;
    cs2_config_stream_t        config_stream;
    bool                       hash_collision;
} cs2_master_t;

/* ========================================================================
 * Lifecycle
 * ====================================================================== */

/**
 * Initialize the CS2 master.
 * Computes hash from UID, clears all state, stores platform/config/callbacks.
 */
void cs2_master_init(cs2_master_t *m, const ms1_platform_t *platform,
                     const cs2_master_config_t *config,
                     const cs2_master_callbacks_t *callbacks);

/**
 * Poll the CS2 master — call from your main loop.
 * Processes incoming CAN frames, sends periodic pings, checks timeouts.
 */
void cs2_master_poll(cs2_master_t *m);

/* ========================================================================
 * Lok database management
 * ====================================================================== */

/** Add or update a lok in a slot (0..CS2_MAX_LOKS-1). */
bool cs2_master_set_lok(cs2_master_t *m, uint8_t slot, const cs2_lok_t *lok);

/** Remove a lok from a slot. */
bool cs2_master_remove_lok(cs2_master_t *m, uint8_t slot);

/* ========================================================================
 * Lok control — send commands on the bus
 * ====================================================================== */

/** Set lok speed (0-1000). */
void cs2_master_lok_speed(cs2_master_t *m, uint32_t locid, uint16_t speed);

/** Set lok direction. */
void cs2_master_lok_direction(cs2_master_t *m, uint32_t locid, cs2_direction_t dir);

/** Set lok function value. */
void cs2_master_lok_function(cs2_master_t *m, uint32_t locid,
                              uint8_t func_nr, uint8_t value);

/** Emergency stop a specific lok. */
void cs2_master_lok_emergency_stop(cs2_master_t *m, uint32_t locid);

/* ========================================================================
 * System commands
 * ====================================================================== */

/** System stop — track power off. */
void cs2_master_system_stop(cs2_master_t *m);

/** System go — track power on. */
void cs2_master_system_go(cs2_master_t *m);

/** System halt — all loks speed 0 (track stays powered). */
void cs2_master_system_halt(cs2_master_t *m);

/* ========================================================================
 * Accessory control
 * ====================================================================== */

/** Switch an accessory (default timeout). */
void cs2_master_acc_switch(cs2_master_t *m, uint32_t locid,
                            uint8_t position, uint8_t power);

/** Switch an accessory with custom timeout (10ms steps). */
void cs2_master_acc_switch_timed(cs2_master_t *m, uint32_t locid,
                                  uint8_t position, uint8_t power,
                                  uint16_t time_10ms);

/* ========================================================================
 * S88 feedback
 * ====================================================================== */

/** Subscribe to S88 events from a device/contact. */
void cs2_master_s88_subscribe(cs2_master_t *m, uint16_t device_id,
                               uint16_t contact);

/** Query current S88 state. */
void cs2_master_s88_query(cs2_master_t *m, uint16_t device_id,
                           uint16_t contact);

/* ========================================================================
 * Config data streaming
 * ====================================================================== */

/** Request config data from the bus (sends CMD_CFG_DATA_REQ). */
void cs2_master_config_request(cs2_master_t *m, const char *filename);

/**
 * Provide config data to send to a requesting peer.
 * Call this from the on_config_data_request callback.
 * @return true if accepted, false if a stream is already in progress.
 */
bool cs2_master_config_provide(cs2_master_t *m, const char *filename,
                                const uint8_t *data, uint32_t length);

/* ========================================================================
 * CV programming (read/write config)
 * ====================================================================== */

/** Read CV(s) from a lok. */
void cs2_master_read_config(cs2_master_t *m, uint32_t locid,
                             uint16_t cv_number, uint8_t count);

/** Write a CV value to a lok. */
void cs2_master_write_config(cs2_master_t *m, uint32_t locid,
                              uint16_t cv_number, uint8_t value);

/* ========================================================================
 * Discovery
 * ====================================================================== */

/** Send a ping broadcast (DLC=0 query). */
void cs2_master_ping(cs2_master_t *m);

/** Get a discovered device by index. Returns true if slot is occupied. */
bool cs2_master_get_device(const cs2_master_t *m, uint8_t idx,
                            cs2_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CS2_MASTER_H */

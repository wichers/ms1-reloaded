/**
 * @file ms1_master.h
 * @brief M-CAN Master API for controlling MS1 slave devices.
 *
 * This module implements the master side of the M-CAN protocol. It handles:
 *   - Bus occupation signaling (once, at start-up)
 *   - Slave registration (UID handshake, node assignment)
 *   - Setup phase (handles, status list, lokstack, lok data exchange)
 *   - Operational phase (monitoring, lok control, stop/go, take/release)
 *   - Firmware flashing
 *
 * Usage:
 *   1. Fill in an ms1_platform_t with your CAN driver functions.
 *   2. Configure ms1_master_config_t with master UID, versions, etc.
 *   3. Set up ms1_master_callbacks_t for events you want to handle.
 *   4. Call ms1_master_init().
 *   5. Add locomotives with ms1_master_set_lok().
 *   6. Call ms1_master_poll() repeatedly from your main loop.
 *   7. When a slave connects, assign loks with ms1_master_assign_lok().
 */

#ifndef MS1_MASTER_H
#define MS1_MASTER_H

#include "ms1_platform.h"
#include "ms1_types.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Slave states
 * ====================================================================== */

/* A slave in registration waits in MS1_SLAVE_REG_STAGE_1..3 for its stage
 * 1..3 frame; after stage 3 the master assigns the node (stage 7) and the
 * slave is in MS1_SLAVE_SETUP until it has fetched the lokstack. A slave
 * silent for MS1_HEARTBEAT_TIMEOUT_MS goes back to MS1_SLAVE_IDLE.
 * MS1_SLAVE_REG_STAGE_0, _4.._7 and MS1_SLAVE_DISCONNECTING are not entered. */
typedef enum {
    MS1_SLAVE_IDLE = 0,
    MS1_SLAVE_REG_STAGE_0,
    MS1_SLAVE_REG_STAGE_1,
    MS1_SLAVE_REG_STAGE_2,
    MS1_SLAVE_REG_STAGE_3,
    MS1_SLAVE_REG_STAGE_4,
    MS1_SLAVE_REG_STAGE_5,
    MS1_SLAVE_REG_STAGE_6,
    MS1_SLAVE_REG_STAGE_7,
    MS1_SLAVE_SETUP,
    MS1_SLAVE_OPERATIONAL,
    MS1_SLAVE_DISCONNECTING,
} ms1_slave_state_t;

/* ========================================================================
 * Locomotive function definition
 * ====================================================================== */

/* The slave asks the lok's objects 1, 2, 3, ... in turn and stops at the
 * first number no function has: number them without gaps. */
typedef struct {
    uint8_t              number;      /**< CAN object number (1 = speed, then 2, 3, ...) */
    ms1_func_activation_t activation; /**< Speed / maintained / momentary */
    ms1_icon_t          icon;        /**< LCD icon on MS1 (0x01-0x10; 0 = the MS1 drops the function) */
    uint8_t              value;       /**< Current value (0 or 1, or speed byte) */
} ms1_lok_func_t;

/* ========================================================================
 * Locomotive definition
 * ====================================================================== */

typedef struct {
    char                name[MS1_LOK_NAME_LEN + 1]; /**< Null-terminated name (the MS1 keeps 15 chars) */
    ms1_lok_type_t     type;         /**< Steam / diesel / electric / none (the MS1's lok symbol) */
    ms1_rail_format_t  format;       /**< mfx / DCC / MM */
    ms1_lok_func_t     funcs[MS1_MAX_LOK_FUNCS]; /**< Function definitions */
    uint8_t             func_count;   /**< Number of defined functions (including speed) */
    bool                active;       /**< true if this slot is populated */
} ms1_lok_t;

/* ========================================================================
 * Read-only slave information (returned by ms1_master_get_slave_info)
 * ====================================================================== */

typedef struct {
    ms1_slave_state_t state;
    uint8_t            uid[4];                     /**< Slave UID (big-endian) */
    uint8_t            master_node;                /**< Assigned master node number */
    uint8_t            slave_node;                 /**< Assigned slave node number */
    int8_t             lokstack[MS1_LOKSTACK_SIZE]; /**< Lok slot indices (-1 = empty) */
    uint8_t            lokstack_count;             /**< Number of assigned loks */
    int8_t             selected_lok;               /**< Lok slot the slave controls (-1 = none) */
} ms1_slave_info_t;

/* ========================================================================
 * Master configuration
 * ====================================================================== */

/* The registration answers of stages 1-3 carry the versions, vendor and
 * master type; an MS1 slave does not evaluate them. */
typedef struct {
    uint8_t  uid[4];          /**< Master UID (4 bytes, big-endian); must not be 0 (an MS1 slave
                                   restarts on a monitor answer with UID 0) */
    uint16_t hw_version;      /**< Hardware version (e.g. 0x4010) */
    uint16_t sw_version;      /**< Software version (e.g. 0x0103 = v1.3) */
    uint16_t build_version;   /**< Build version */
    uint8_t  vendor_id;       /**< NMRA vendor ID (0x97 = ESU) */
    uint8_t  master_type;     /**< Master type byte (0x22=ECoS, 0x84=MS1, 0x86=CS1) */
    uint8_t  node_base;       /**< Master node of slave 0: slave i gets master node node_base + 2i
                                   and slave node one higher (42 for ECoS/CS1, 0 as an MS1 master);
                                   even, and node_base + 2 * MS1_MAX_SLAVES - 1 at most 125 */
} ms1_master_config_t;

/* ========================================================================
 * Callbacks (all optional, set to NULL if not needed)
 * ====================================================================== */

typedef struct {
    void *ctx; /**< User context pointer, passed to all callbacks */

    /** A slave has completed registration and setup (it has fetched its lokstack). */
    void (*on_slave_connected)(void *ctx, uint8_t slave_id);

    /** A connected slave is gone: silent for MS1_HEARTBEAT_TIMEOUT_MS, or registering again. */
    void (*on_slave_disconnected)(void *ctx, uint8_t slave_id);

    /** A slave changed locomotive speed/direction (only the slave controlling the lok).
     *  speed_dir: 0x00..0x7F = forward, 0x80..0xFF = reverse */
    void (*on_lok_speed)(void *ctx, uint8_t slave_id, uint8_t lok_slot, uint8_t speed_dir);

    /** A slave toggled a function (only the slave controlling the lok).
     *  func_nr: function number (1-based), value: 0=off, 1=on */
    void (*on_lok_function)(void *ctx, uint8_t slave_id, uint8_t lok_slot,
                            uint8_t func_nr, uint8_t value);

    /** A slave pressed the Stop button (Stop, or emergency stop). */
    void (*on_stop)(void *ctx, uint8_t slave_id);

    /** A slave pressed Go (released Stop). */
    void (*on_go)(void *ctx, uint8_t slave_id);

    /** A slave took a lok (it controls it now) or released it.
     *  lok_slot: index into the master lok database, or -1 if released. */
    void (*on_lok_selected)(void *ctx, uint8_t slave_id, int8_t lok_slot);

    /** A slave wants a firmware update. Return true to allow.
     *  Only called if firmware has been loaded via ms1_master_load_firmware(). */
    bool (*on_flash_request)(void *ctx, uint8_t slave_id, const uint8_t uid[4]);
} ms1_master_callbacks_t;

/* ========================================================================
 * Flash state (internal)
 * ====================================================================== */

typedef struct {
    bool         active;
    uint8_t      slave_id;
    uint8_t      flash_handle;
    uint8_t      query_stage;    /**< 0=vendor, 1=sw/hw, 2=build, 3=authorized, 4=transferring */
    const uint8_t *fw_data;
    uint32_t     fw_size;
    uint32_t     fw_offset;
    uint32_t     block_offset;
    uint32_t     block_size;
} ms1_flash_state_t;

/* ========================================================================
 * Per-slave internal context
 * ====================================================================== */

typedef struct {
    ms1_slave_state_t state;
    uint8_t   uid[4];
    uint8_t   mid;              /**< Registration number (MID) the master gave this slave */
    uint8_t   master_node;
    uint8_t   slave_node;
    uint16_t  sd_handle;        /**< The slave's own object (MS1_HANDLE_DEVICE + slave node) */
    uint16_t  sy_handle;        /**< System-Status handle (MS1_HANDLE_STATUS) */
    uint16_t  start_obj_handle; /**< Start object handle from registration (MS1_HANDLE_START) */

    /* Lokstack: maps lokstack index -> master lok database slot (-1 = empty);
     * a lok's handle is MS1_HANDLE_LOK + its slot */
    int8_t    lokstack[MS1_LOKSTACK_SIZE];
    uint8_t   lokstack_count;

    /* Lok this slave controls (-1 = none) */
    int8_t    selected_lok;

    /* Monitoring: the last frame from the slave, the last poll / registration step */
    uint32_t  last_heartbeat_ms;
    uint32_t  last_poll_ms;

    /* Flash flag from registration */
    bool      flash_requested;
} ms1_slave_ctx_t;

/* ========================================================================
 * Master context (the main handle)
 * ====================================================================== */

typedef struct {
    const ms1_platform_t   *platform;
    ms1_master_config_t     config;
    ms1_master_callbacks_t  callbacks;

    /* Slave contexts */
    ms1_slave_ctx_t         slaves[MS1_MAX_SLAVES];
    uint8_t                  next_mid;      /**< MID for the next registration (0..31) */

    /* Locomotive database */
    ms1_lok_t               loks[MS1_MAX_LOKS];
    uint16_t                 lok_change[MS1_MAX_LOKS];     /**< Change counter sent with each lok */
    int8_t                   lok_controller[MS1_MAX_LOKS]; /**< Slave controlling each lok (-1 = none) */

    /* System status */
    bool                     stopped;       /**< true = track power off (Stop) */
    bool                     overcurrent;   /**< true = short circuit detected */

    /* Bus-occupied frames still to send */
    uint8_t                  bus_occupied_burst_remaining;

    /* Flash */
    ms1_flash_state_t       flash;
    uint8_t                  next_flash_handle;
} ms1_master_t;

/* ========================================================================
 * Public API
 * ====================================================================== */

/**
 * Initialize the master context.
 * Must be called before any other ms1_master_* function.
 */
void ms1_master_init(ms1_master_t *master,
                      const ms1_platform_t *platform,
                      const ms1_master_config_t *config,
                      const ms1_master_callbacks_t *callbacks);

/**
 * Main processing loop. Call this repeatedly from your main loop.
 * Processes incoming CAN frames, manages timeouts, sends heartbeats.
 */
void ms1_master_poll(ms1_master_t *master);

/**
 * Add or update a locomotive in the master database.
 * An update is announced to the slaves that have the lok, which fetch it again.
 * @param slot  Lok slot index (0 .. MS1_MAX_LOKS-1).
 * @param lok   Locomotive definition to copy in. Pass NULL to clear the slot.
 * @return true on success, false if slot is out of range.
 */
bool ms1_master_set_lok(ms1_master_t *master, uint8_t slot, const ms1_lok_t *lok);

/**
 * Remove a locomotive from the master database (and from every lokstack).
 * @param slot  Lok slot index.
 * @return true on success.
 */
bool ms1_master_remove_lok(ms1_master_t *master, uint8_t slot);

/**
 * Push a locomotive to a slave's lokstack (state SETUP or OPERATIONAL).
 * The slave is notified and fetches the lok's data; to drive it, the slave
 * takes it (on_lok_selected).
 * @param slave_id  Slave index (0 .. MS1_MAX_SLAVES-1).
 * @param lok_slot  Lok slot in master database.
 * @return true on success.
 */
bool ms1_master_assign_lok(ms1_master_t *master, uint8_t slave_id, uint8_t lok_slot);

/**
 * Remove a locomotive from a slave's lokstack.
 * @param slave_id  Slave index.
 * @param lok_slot  Lok slot in master database.
 * @return true on success.
 */
bool ms1_master_unassign_lok(ms1_master_t *master, uint8_t slave_id, uint8_t lok_slot);

/**
 * Set system Stop/Go state. Broadcasts to all connected slaves.
 * @param stopped  true = Stop (no track power), false = Go.
 */
void ms1_master_set_stop(ms1_master_t *master, bool stopped);

/**
 * Set overcurrent/short-circuit state. Broadcasts to all connected slaves.
 */
void ms1_master_set_overcurrent(ms1_master_t *master, bool fault);

/**
 * Master drives a locomotive (sent to every slave that has it in its lokstack).
 * @param lok_slot   Lok slot in master database.
 * @param speed_dir  0x00..0x7F = forward speeds, 0x80..0xFF = reverse speeds.
 */
void ms1_master_drive_lok(ms1_master_t *master, uint8_t lok_slot, uint8_t speed_dir);

/**
 * Master sets a function on a locomotive.
 * @param lok_slot  Lok slot.
 * @param func_nr   Function number (1-based).
 * @param value     0=off, 1=on.
 */
void ms1_master_drive_function(ms1_master_t *master, uint8_t lok_slot,
                                uint8_t func_nr, uint8_t value);

/**
 * Rename a connected slave.
 * @param slave_id  Slave index.
 * @param name      New name (null-terminated; the MS1 keeps 15 chars).
 */
void ms1_master_rename_slave(ms1_master_t *master, uint8_t slave_id, const char *name);

/**
 * Get read-only information about a slave.
 * @param slave_id  Slave index.
 * @param info      Output struct filled with slave info.
 * @return true if slave_id is valid.
 */
bool ms1_master_get_slave_info(const ms1_master_t *master, uint8_t slave_id,
                                ms1_slave_info_t *info);

/**
 * Load firmware data for flash operations.
 * The data pointer must remain valid as long as flashing may occur.
 * @param data  Pointer to BCI firmware file content (payload section).
 * @param size  Size in bytes.
 */
void ms1_master_load_firmware(ms1_master_t *master, const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MS1_MASTER_H */

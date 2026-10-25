/**
 * @file ms1_test_reference.c
 * @brief Replays the reference conversation of the original MS1 firmware against the library.
 *
 * test/ms1_reference.can holds the frames of an emulated original MS1 master with an
 * emulated original MS1 slave (how they were captured: the file's header). The slave's frames
 * (RX) are fed to the library at their times; the library, configured as the MS1 master was
 * (node 0, its UID, loco 3 with 9 functions), must answer with the master's frames (TX), in the
 * same order and no later. The operator's actions on the MS1 master become API calls. Then the
 * library's callbacks and its monitoring of the unplugged slave are checked against the
 * capture.
 *
 * Frames the library intentionally does not send as the MS1 master did:
 *   - the master's own polls (timer-driven; their timing is checked separately) and its mfx
 *     registration status (status object 4), which the library has no reason to change;
 *   - the controller frames of the MS1 master's operator taking and releasing loco 3 (the
 *     library's lok stays free), and so controller 0 in the announcement and the answer to
 *     the slave's controller query;
 *   - the versions in the registration answers of stages 1-3 (config fields; the slave does
 *     not evaluate them) and the lok's track address and protocol flags (not modelled: 0).
 *
 *   build/ms1_test_reference test/ms1_reference.can
 */

#include "ms1_master.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Mock platform
 * ====================================================================== */

#define QUEUE_MAX 64

static ms1_can_frame_t rx_queue[QUEUE_MAX];
static int rx_count;
static ms1_can_frame_t tx_queue[QUEUE_MAX];
static uint32_t tx_time[QUEUE_MAX];
static int tx_head, tx_tail;
static uint32_t now_ms;

/* The library's polls of the slave, kept apart from the compared frames */
static uint32_t poll_time[256];
static int poll_count;

static bool is_poll(const ms1_can_frame_t *f)
{
    static const uint8_t data[8] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x52, 0xE2};
    return f->id == 0x0C000380u && f->dlc == 8 && memcmp(f->data, data, 8) == 0;
}

static bool mock_send(const ms1_can_frame_t *f)
{
    if (is_poll(f)) {
        if (poll_count < 256) poll_time[poll_count++] = now_ms;
        return true;
    }
    if (tx_tail - tx_head >= QUEUE_MAX) {
        printf("FAIL: transmit queue overflow\n");
        exit(1);
    }
    tx_time[tx_tail % QUEUE_MAX] = now_ms;
    tx_queue[tx_tail++ % QUEUE_MAX] = *f;
    return true;
}

static bool mock_receive(ms1_can_frame_t *f)
{
    if (rx_count == 0) return false;
    *f = rx_queue[0];
    memmove(rx_queue, rx_queue + 1, (size_t)(--rx_count) * sizeof rx_queue[0]);
    return true;
}

static uint32_t mock_millis(void) { return now_ms; }

static const ms1_platform_t platform = {mock_send, mock_receive, mock_millis, NULL};

/* ========================================================================
 * Callbacks, recorded as text
 * ====================================================================== */

#define EVENT_MAX 128
static char events[EVENT_MAX][48];
static uint32_t event_time[EVENT_MAX];
static int event_count;

static void event(const char *fmt, int a, int b, int c, int d)
{
    if (event_count == EVENT_MAX) return;
    event_time[event_count] = now_ms;
    snprintf(events[event_count++], sizeof events[0], fmt, a, b, c, d);
}

static void cb_connected(void *ctx, uint8_t s) { (void)ctx; event("connected %d", s, 0, 0, 0); }
static void cb_disconnected(void *ctx, uint8_t s) { (void)ctx; event("disconnected %d", s, 0, 0, 0); }
static void cb_speed(void *ctx, uint8_t s, uint8_t l, uint8_t v) { (void)ctx; event("speed %d %d %02X", s, l, v, 0); }
static void cb_function(void *ctx, uint8_t s, uint8_t l, uint8_t f, uint8_t v)
{
    (void)ctx;
    event("function %d %d %d %d", s, l, f, v);
}
static void cb_stop(void *ctx, uint8_t s) { (void)ctx; event("stop %d", s, 0, 0, 0); }
static void cb_go(void *ctx, uint8_t s) { (void)ctx; event("go %d", s, 0, 0, 0); }
static void cb_selected(void *ctx, uint8_t s, int8_t l) { (void)ctx; event("selected %d %d", s, l, 0, 0); }

/* ========================================================================
 * The capture
 * ====================================================================== */

typedef struct {
    uint32_t us;       /* time, microseconds */
    bool from_master;  /* TX */
    ms1_can_frame_t f;
} line_t;

static line_t lines[1024];
static int line_count;

static void load(const char *path)
{
    FILE *fp = fopen(path, "r");
    char buf[256];
    if (!fp) {
        perror(path);
        exit(1);
    }
    while (fgets(buf, sizeof buf, fp)) {
        unsigned sec, frac, id, dlc, b[8];
        char dir[4];
        if (buf[0] == '#' || sscanf(buf, "%u.%u %3s %x [%u]", &sec, &frac, dir, &id, &dlc) != 5) {
            continue;
        }
        line_t *l = &lines[line_count++];
        l->us = sec * 1000000u + frac;
        l->from_master = strcmp(dir, "TX") == 0;
        l->f.id = id;
        l->f.dlc = (uint8_t)dlc;
        memset(l->f.data, 0, 8);
        const char *p = strchr(buf, ']') + 1;
        int n = sscanf(p, "%x %x %x %x %x %x %x %x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
        for (int i = 0; i < n && i < 8; i++) l->f.data[i] = (uint8_t)b[i];
    }
    fclose(fp);
}

/* ========================================================================
 * The library's differences, by capture time (us)
 * ====================================================================== */

typedef struct {
    uint32_t us;
    int dlc;            /* -1: the master's frame is not expected from the library */
    uint8_t data[8];
    const char *why;
} diff_t;

static const diff_t diffs[] = {
    {9346428, 8, {0x00, 0x00, 0x00, 0x97, 0x01, 0x00, 0xC0, 0x10}, "stage-1 answer: vendor, MS1 family (MS1: 02 00 C0 11)"},
    {9350432, 8, {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11}, "stage-2 answer: build, HW (MS1: 01 01 02 2F ...)"},
    {9354428, 8, {0x00, 0x00, 0x40, 0x11, 0x01, 0x08, 0x00, 0x84}, "stage-3 answer: HW, SW, type (MS1: ... 00 14)"},
    {9382634, 3, {0x04, 0x00, 0x00}, "status 4 (mfx registration): not running"},
    {16007215, -1, {0}, "the MS1 master's operator takes loco 3"},
    {16007742, 8, {0x80, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02}, "announcement: controller 0 (MS1: 0x0100, itself)"},
    {16014487, 8, {0x40, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00}, "protocol MM, flags 0 (MS1: 3)"},
    {16018474, 8, {0x40, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, "address 0 (MS1: 3)"},
    {16126426, 7, {0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00}, "controller 0 (MS1: 0x0100, itself)"},
    {17030411, -1, {0}, "the MS1 master's operator releases loco 3"},
};

static const diff_t *find_diff(uint32_t us)
{
    for (size_t i = 0; i < sizeof diffs / sizeof diffs[0]; i++) {
        if (diffs[i].us == us) return &diffs[i];
    }
    return NULL;
}

/* The master's spontaneous frames: its polls and its own mfx-registration status */
static bool spontaneous(const line_t *l)
{
    static const uint8_t mfx_status[2] = {0x04, 0x00};
    if (is_poll(&l->f)) return true;
    return l->f.id == 0x08000900u && l->f.dlc == 3 && memcmp(l->f.data, mfx_status, 2) == 0 &&
           !find_diff(l->us);
}

/* ========================================================================
 * Replay
 * ====================================================================== */

static ms1_master_t master;
static int failures;
static int compared, substituted, skipped;

static void check_frame(const line_t *l)
{
    const diff_t *d = find_diff(l->us);
    ms1_can_frame_t want = l->f;
    if ((d && d->dlc < 0) || (!d && spontaneous(l))) {
        skipped++;
        return;
    }
    compared++;
    if (d) {
        substituted++;
        want.dlc = (uint8_t)d->dlc;
        memcpy(want.data, d->data, 8);
    }
    if (tx_head == tx_tail) {
        printf("FAIL %u.%06u: library sent nothing, MS1 master sent %08X\n",
               l->us / 1000000, l->us % 1000000, (unsigned)want.id);
        failures++;
        return;
    }
    uint32_t t = tx_time[tx_head % QUEUE_MAX];
    ms1_can_frame_t got = tx_queue[tx_head++ % QUEUE_MAX];
    if (got.id != want.id || got.dlc != want.dlc || memcmp(got.data, want.data, want.dlc) != 0 ||
        t > l->us / 1000) {
        printf("FAIL %u.%06u: want %08X [%u]", l->us / 1000000, l->us % 1000000, (unsigned)want.id, want.dlc);
        for (int i = 0; i < want.dlc; i++) printf(" %02X", want.data[i]);
        printf(", library sent at %u ms %08X [%u]", t, (unsigned)got.id, got.dlc);
        for (int i = 0; i < got.dlc; i++) printf(" %02X", got.data[i]);
        printf("\n");
        failures++;
    }
}

/* Operator actions on the MS1 master, at the time of its first frame for them. The MS1
 * master starts in STOP (set before the run) and switches to GO once it is up. */
static void act(uint32_t ms)
{
    static int done;
    if (done == 0 && ms >= 11302) {
        ms1_master_set_stop(&master, false);
        done++;
    }
    if (done == 1 && ms >= 16007) {
        ms1_master_assign_lok(&master, 0, 0);
        done++;
    }
    if (done == 2 && ms >= 28006) {
        ms1_master_set_stop(&master, true);
        done++;
    }
    if (done == 3 && ms >= 29030) {
        ms1_master_set_stop(&master, false);
        done++;
    }
}

static void expect_event(int *k, const char *text, uint32_t at, uint32_t tolerance)
{
    if (*k >= event_count || strcmp(events[*k], text) != 0 ||
        event_time[*k] + tolerance < at || event_time[*k] > at + tolerance) {
        printf("FAIL: callback %d: want \"%s\" at %u ms, got \"%s\" at %u ms\n", *k, text, at,
               *k < event_count ? events[*k] : "(none)", *k < event_count ? event_time[*k] : 0);
        failures++;
    }
    (*k)++;
}

int main(int argc, char **argv)
{
    load(argc > 1 ? argv[1] : "test/ms1_reference.can");
    printf("Reference replay (%d frames of the original MS1 master and slave)\n", line_count);

    ms1_master_config_t config = {
        .uid = {0x00, 0x07, 0x52, 0xE2},
        .hw_version = 0x4011, .sw_version = 0x0108, .build_version = 0x0101,
        .vendor_id = MS1_VENDOR_ESU, .master_type = MS1_MASTER_TYPE_MS1, .node_base = 0,
    };
    ms1_master_callbacks_t cb = {0};
    cb.on_slave_connected = cb_connected;
    cb.on_slave_disconnected = cb_disconnected;
    cb.on_lok_speed = cb_speed;
    cb.on_lok_function = cb_function;
    cb.on_stop = cb_stop;
    cb.on_go = cb_go;
    cb.on_lok_selected = cb_selected;
    ms1_master_init(&master, &platform, &config, &cb);

    /* Loco 3 as the MS1 master created it: steam symbol, MM, speed and 9 functions with its
     * generic icon; set twice so that its change counter is 2, as the MS1's slot */
    ms1_lok_t lok;
    memset(&lok, 0, sizeof lok);
    strcpy(lok.name, "3");
    lok.type = MS1_LOK_TYPE_STEAM;
    lok.format = MS1_RAIL_MM;
    lok.func_count = 10;
    lok.funcs[0] = (ms1_lok_func_t){.number = 1, .activation = MS1_FUNC_SPEED};
    for (uint8_t i = 1; i < 10; i++) {
        lok.funcs[i] = (ms1_lok_func_t){.number = (uint8_t)(i + 1), .activation = MS1_FUNC_MAINTAINED,
                                        .icon = MS1_ICON_10};
    }
    ms1_master_set_lok(&master, 0, &lok);
    ms1_master_set_lok(&master, 0, &lok);
    ms1_master_set_stop(&master, true);

    /* The frames in order, the library polled every millisecond in between */
    uint32_t last_rx = 0;
    now_ms = lines[0].us / 1000;
    for (int i = 0; i < line_count; i++) {
        const line_t *l = &lines[i];
        while (now_ms < l->us / 1000) {
            now_ms++;
            act(now_ms);
            ms1_master_poll(&master);
        }
        act(now_ms);
        if (!l->from_master) {
            rx_queue[rx_count++] = l->f;
            last_rx = now_ms;
        }
        ms1_master_poll(&master);
        if (l->from_master) check_frame(l);
    }
    /* On to the end of the run (38 s) */
    for (uint32_t t = now_ms + 1; t <= 38000; t++) {
        now_ms = t;
        ms1_master_poll(&master);
    }
    while (tx_head != tx_tail) {
        ms1_can_frame_t f = tx_queue[tx_head % QUEUE_MAX];
        printf("FAIL: extra frame from the library at %u ms: %08X\n", tx_time[tx_head % QUEUE_MAX], (unsigned)f.id);
        tx_head++;
        failures++;
    }

    /* Callbacks, from the capture: connected when the slave has fetched its (empty)
     * lokstack, the slave takes loco 3, drives it and switches STOP/GO as its frames say,
     * and is given up MS1_HEARTBEAT_TIMEOUT_MS after its last frame */
    int k = 0;
    expect_event(&k, "connected 0", 9397, 0);
    expect_event(&k, "selected 0 0", 18021, 0);
    for (int i = 0; i < line_count; i++) {
        const line_t *l = &lines[i];
        char text[48];
        if (l->from_master || l->f.dlc != 3) continue;
        if (l->f.id == 0x08080101u && l->f.data[0] == 1)
            snprintf(text, sizeof text, "speed 0 0 %02X", l->f.data[2]);
        else if (l->f.id == 0x08080101u)
            snprintf(text, sizeof text, "function 0 0 %d %d", l->f.data[0], l->f.data[2] & 1);
        else if (l->f.id == 0x08000901u && l->f.data[0] == 1)
            snprintf(text, sizeof text, "%s 0", l->f.data[2] ? "stop" : "go");
        else
            continue;
        expect_event(&k, text, l->us / 1000, 0);
    }
    expect_event(&k, "disconnected 0", last_rx + MS1_HEARTBEAT_TIMEOUT_MS, 0);
    if (k != event_count) {
        printf("FAIL: %d callbacks, want %d\n", event_count, k);
        failures++;
    }

    /* Polls after the unplug: the MS1 master's times (one 4 ms tick of tolerance), none after
     * the slave is given up */
    int mine = 0, theirs = 0;
    for (int i = 0; i < poll_count; i++) {
        if (poll_time[i] > last_rx) mine++;
    }
    for (int i = 0; i < line_count; i++) {
        const line_t *l = &lines[i];
        if (!l->from_master || !is_poll(&l->f) || l->us / 1000 <= last_rx) continue;
        uint32_t t = l->us / 1000;
        bool near = false;
        for (int j = 0; j < poll_count; j++) {
            if (poll_time[j] + 4 >= t && poll_time[j] <= t + 4) near = true;
        }
        if (!near) {
            printf("FAIL: the MS1 master polls at %u ms, the library not within 4 ms\n", t);
            failures++;
        }
        theirs++;
    }
    if (mine != theirs) {
        printf("FAIL: %d polls after the unplug, the MS1 master %d\n", mine, theirs);
        failures++;
    }

    printf("  %d master frames compared (%d with the differences listed), %d not expected\n",
           compared, substituted, skipped);
    printf("  %d callbacks, %d polls after the unplug (MS1 master: %d)\n", event_count, mine, theirs);
    printf("Results: %d/%d tests passed\n", failures ? 0 : 1, 1);
    return failures ? 1 : 0;
}

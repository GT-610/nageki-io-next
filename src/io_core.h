#ifndef MU3_IO_CORE_H
#define MU3_IO_CORE_H

/* Offline-only input core. No HID access, native exports, or device writes.
 * Raw offsets are hypotheses from the archived DLL, not captured device facts.
 *
 * Validity rule: DEVICE CONNECTION is authoritative, not frame recency. The
 * frozen, cabinet-tested DLL kept serving the most recent report until the
 * device actually disappeared, and never expired a report on a timer. Expiring
 * on a timer is unsafe on exactly the input that matters: a lever held at a
 * stop or a button held down must stay in effect for as long as it is held, and
 * a report can only be known to be superseded once a newer one has arrived.
 * Nothing here consults frame age. Age is not even exposed, so no caller can be
 * tempted to.
 *
 * A capture has since settled the device's behaviour: it streams continuously at
 * about 200 reports/s (hid_probe.exe jitter 30000, 5986 reports in 30 s with the
 * lever untouched), so in practice a fresh report always arrives within
 * milliseconds and this rule is a fallback rather than the normal path. That
 * same measurement means "the device is open but has sent nothing for a while"
 * is detectable on this hardware, which a genuinely change-triggered device
 * would make indistinguishable from stillness. A stalled firmware that keeps its
 * handle open currently freezes input at the last report; see mu3_core_snapshot. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define MU3_REPORT_SIZE 65u
#define MU3_PAYLOAD_SIZE 64u
#define MU3_CARD_SIZE 10u

typedef enum mu3_health {
    MU3_NO_DEVICE,        /* never opened */
    MU3_WAITING_FOR_FRAME,/* opened, no report received yet */
    MU3_FRESH,            /* opened and at least one valid report is held */
    MU3_DISCONNECTED      /* device went away; input is neutral */
} mu3_health;

typedef struct mu3_sample {
    uint8_t left;
    uint8_t right;
    uint16_t raw_lever; /* Uncalibrated LE field; not yet a game lever. */
    uint8_t scan;
    uint8_t card[MU3_CARD_SIZE];
    uint8_t operator_buttons; /* Raw byte; coin edge meaning unverified. */
    uint64_t received_ms;
    uint64_t sequence;
} mu3_sample;

typedef struct mu3_core {
    SRWLOCK lock;
    mu3_sample latest;
    mu3_health health;
    int report_id; /* -1 skips ID check until a real descriptor is captured. */
    bool have_frame;
} mu3_core;

/* report_id is the expected HID report ID, or -1 to accept any ID until a real
 * descriptor is captured. Only one reader may publish reports; getters can run
 * on arbitrary threads. */
bool mu3_core_init(mu3_core *core, int report_id);
/* Device opened or re-opened: connected, waiting for the first report. Clears
 * the previous sample so a reconnect can never replay stale input. */
void mu3_core_open(mu3_core *core);
bool mu3_core_publish(mu3_core *core, const uint8_t *report, size_t size,
                      uint64_t received_ms);
/* Device removed: input becomes neutral until the next open/publish. */
void mu3_core_disconnect(mu3_core *core);
/* Returns false and zeroes output only when the device is not connected or no
 * report has ever arrived. A held report is returned no matter how old it is: a
 * newer report is the only thing that may supersede it.
 *
 * Consequence worth knowing: if the firmware stalls while the device stays
 * enumerated, the handle remains open, no read ever fails and no report ever
 * arrives, so this keeps returning the last report for ever and input freezes
 * rather than releasing. The controller has been measured at about 200
 * reports/s, so a silence threshold would detect that; it is not implemented,
 * and adding one would reinstate the age-based invalidation this rule exists to
 * avoid for genuinely held input. */
bool mu3_core_snapshot(mu3_core *core, mu3_sample *out, mu3_health *health);

#endif

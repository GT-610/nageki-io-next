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
    uint8_t operator_buttons; /* Payload offset 23, capture-confirmed. 1=Test,
                              * 2=Service, 4=Coin; the Test button reports 3
                              * (Test+Service together), so Service is never
                              * seen alone on this controller. */
    uint64_t received_ms;
    uint64_t sequence;
} mu3_sample;

typedef struct mu3_core {
    SRWLOCK lock;
    mu3_sample latest;
    mu3_health health;
    int report_id; /* -1 accepts any ID. A capture showed the wire ID is 0x00, but
                    * the descriptor length is what decides header-vs-payload
                    * (mu3_hid_unpack), so no ID test is enforced here. */
    bool have_frame;
} mu3_core;

/* report_id is the expected HID report ID, or -1 to accept any ID. Pass -1:
 * the real wire ID is 0x00, and the header-vs-payload decision belongs to
 * mu3_hid_unpack, which keys off the descriptor length rather than this value.
 * Only one reader may publish reports; getters can run on arbitrary threads. */
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

/* Cross-process sample serving.
 *
 * The game process reads the owner's sample out of shared memory. Two failures
 * look similar but must not be answered the same way:
 *
 *   - the copy raced the owner's write (torn, unusable) while the owner is
 *     alive and ticking. The right answer is the previous consistent sample: it
 *     is a frame the device really sent, and for a held button it is still the
 *     truth. Answering neutral here would release every held key for one poll,
 *     which is exactly what this design exists to avoid.
 *   - the owner is gone. The right answer is neutral, and the cache must be
 *     dropped so a dead owner's held button can never be replayed.
 *
 * Getting the second case wrong is the dangerous one, so it is stated in the
 * type rather than left to a caller's discretion. */
typedef enum mu3_read_outcome {
    MU3_READ_OK,    /* a consistent sample was copied into the caller's buffer */
    MU3_READ_RETRY, /* owner alive and ticking; the copy raced its write */
    MU3_READ_GONE   /* no owner, owner not alive, or heartbeat stopped */
} mu3_read_outcome;

typedef enum mu3_sample_source {
    MU3_SAMPLE_FRESH,  /* *out is the sample just read */
    MU3_SAMPLE_CACHED, /* *out is the previous consistent sample */
    MU3_SAMPLE_NONE    /* nothing to serve; *out is neutral */
} mu3_sample_source;

typedef struct mu3_sample_cache {
    mu3_sample last;
    bool have_last;
} mu3_sample_cache;

/* Fold one read outcome into the cache and decide what to serve. `sample` is
 * read only for MU3_READ_OK; `out` is always written, so the caller has a
 * neutral sample whenever the result is MU3_SAMPLE_NONE. */
mu3_sample_source mu3_sample_cache_apply(mu3_sample_cache *cache,
                                         mu3_read_outcome outcome,
                                         const mu3_sample *sample,
                                         mu3_sample *out);

#endif

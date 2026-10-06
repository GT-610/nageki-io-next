#include "io_core.h"
#include "button_map.h"

#include <string.h>

void mu3_core_init(mu3_core *core)
{
    if (core == NULL) {
        return;
    }
    memset(core, 0, sizeof(*core));
    InitializeSRWLock(&core->lock);
    core->health = MU3_NO_DEVICE;
}

void mu3_core_open(mu3_core *core)
{
    AcquireSRWLockExclusive(&core->lock);
    core->health = MU3_WAITING_FOR_FRAME;
    core->have_frame = false;
    memset(&core->latest, 0, sizeof(core->latest));
    ReleaseSRWLockExclusive(&core->lock);
}

bool mu3_core_publish(mu3_core *core, const uint8_t *report, size_t size,
                      uint64_t received_ms)
{
    mu3_sample next = {0};
    const uint8_t *payload;
    if (core == NULL || report == NULL || size != MU3_REPORT_SIZE) {
        return false;
    }
    payload = report + 1;
    /* Button bytes are taken as active-high "nonzero means pressed". The frozen
     * DLL OR-ed the raw bytes into a bitmask and validated nothing at all, so a
     * device reporting 0x00/0xFF (not 0/1) still works there. Rejecting values
     * above 1 here would discard every frame and produce "DLL loads, no input at
     * all", so the value is normalised instead of rejected. Exact polarity and
     * byte meanings still need a real capture.
     *
     * The decode itself is shared with hid_probe (src/button_map.c) so the DLL
     * and the diagnostic cannot disagree about which byte is which key. It is
     * per-key independent: one key's byte cannot affect another's bit, so a
     * combination-dependent loss cannot originate here. */
    mu3_buttons_decode(payload, &next.left, &next.right);
    next.raw_lever = (uint16_t)(payload[10] | ((uint16_t)payload[11] << 8));
    /* Any other scan value means "no card", matching AimiIO's fallthrough; it
     * must not cost the buttons and lever in the same report. */
    next.scan = payload[12] <= 2 ? payload[12] : 0;
    if (next.scan != 0) {
        memcpy(next.card, payload + 13, MU3_CARD_SIZE);
    }
    /* Operator byte, capture-confirmed at offset 23. This controller only ever
     * sent 0x03 (the Test-menu button, declaring Test+Service) and 0x04 (Coin);
     * a standalone 0x02 was never observed. Passed through unchanged: assigning
     * meaning to the bits, and counting coin edges, is segatools' job
     * (games/mu3hook/io4.c), not this core's. */
    next.operator_buttons = payload[23];
    next.received_ms = received_ms;

    AcquireSRWLockExclusive(&core->lock);
    if (core->health == MU3_NO_DEVICE || core->health == MU3_DISCONNECTED ||
        (core->have_frame && received_ms < core->latest.received_ms)) {
        ReleaseSRWLockExclusive(&core->lock);
        return false;
    }
    core->latest = next;
    core->have_frame = true;
    core->health = MU3_FRESH;
    ReleaseSRWLockExclusive(&core->lock);
    return true;
}

void mu3_core_disconnect(mu3_core *core)
{
    AcquireSRWLockExclusive(&core->lock);
    core->health = MU3_DISCONNECTED;
    core->have_frame = false;
    memset(&core->latest, 0, sizeof(core->latest));
    ReleaseSRWLockExclusive(&core->lock);
}

bool mu3_core_snapshot(mu3_core *core, mu3_sample *out, mu3_health *health)
{
    bool valid;
    if (core == NULL || out == NULL || health == NULL) {
        return false;
    }
    AcquireSRWLockExclusive(&core->lock);
    /* Connected and holding at least one report is all that is required. Age is
     * deliberately not consulted: a change-triggered controller that goes quiet
     * must keep reporting its last state, exactly as the frozen DLL did. */
    valid = core->have_frame && core->health == MU3_FRESH;
    if (valid) {
        *out = core->latest;
    } else {
        memset(out, 0, sizeof(*out));
    }
    *health = core->health;
    ReleaseSRWLockExclusive(&core->lock);
    return valid;
}

mu3_sample_source mu3_sample_cache_apply(mu3_sample_cache *cache,
                                         mu3_read_outcome outcome,
                                         const mu3_sample *sample,
                                         mu3_sample *out)
{
    if (cache == NULL || out == NULL) return MU3_SAMPLE_NONE;
    switch (outcome) {
    case MU3_READ_OK:
        if (sample != NULL) {
            cache->last = *sample;
            cache->have_last = true;
            *out = *sample;
            return MU3_SAMPLE_FRESH;
        }
        /* No sample with an OK outcome is a caller error, not a transient. */
        cache->have_last = false;
        memset(out, 0, sizeof(*out));
        return MU3_SAMPLE_NONE;
    case MU3_READ_RETRY:
        /* Torn copy, live owner. Serve the previous consistent sample rather
         * than releasing every held key for one poll. */
        if (cache->have_last) {
            *out = cache->last;
            return MU3_SAMPLE_CACHED;
        }
        memset(out, 0, sizeof(*out));
        return MU3_SAMPLE_NONE;
    case MU3_READ_GONE:
    default:
        /* The owner is gone: forget it, so its last buttons cannot be replayed. */
        cache->have_last = false;
        memset(out, 0, sizeof(*out));
        return MU3_SAMPLE_NONE;
    }
}

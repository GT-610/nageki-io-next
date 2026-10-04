#include "io_core.h"

#include <string.h>

bool mu3_core_init(mu3_core *core, int report_id)
{
    if (core == NULL || report_id < -1 || report_id > 255) {
        return false;
    }
    memset(core, 0, sizeof(*core));
    InitializeSRWLock(&core->lock);
    core->health = MU3_NO_DEVICE;
    core->report_id = report_id;
    return true;
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
    size_t i;
    const uint8_t *payload;
    if (core == NULL || report == NULL || size != MU3_REPORT_SIZE ||
        (core->report_id >= 0 && report[0] != core->report_id)) {
        return false;
    }
    payload = report + 1;
    /* Button bytes are taken as active-high "nonzero means pressed". The frozen
     * DLL OR-ed the raw bytes into a bitmask and validated nothing at all, so a
     * device reporting 0x00/0xFF (not 0/1) still works there. Rejecting values
     * above 1 here would discard every frame and produce "DLL loads, no input at
     * all", so the value is normalised instead of rejected. Exact polarity and
     * byte meanings still need a real capture. */
    for (i = 0; i < 5; ++i) {
        if (payload[i]) next.left |= (uint8_t)(1u << i);
        if (payload[i + 5]) next.right |= (uint8_t)(1u << i);
    }
    next.raw_lever = (uint16_t)(payload[10] | ((uint16_t)payload[11] << 8));
    /* Any other scan value means "no card", matching AimiIO's fallthrough; it
     * must not cost the buttons and lever in the same report. */
    next.scan = payload[12] <= 2 ? payload[12] : 0;
    if (next.scan != 0) {
        memcpy(next.card, payload + 13, MU3_CARD_SIZE);
    }
    next.operator_buttons = payload[23];
    next.received_ms = received_ms;

    AcquireSRWLockExclusive(&core->lock);
    if (core->health == MU3_NO_DEVICE || core->health == MU3_DISCONNECTED ||
        (core->have_frame && received_ms < core->latest.received_ms)) {
        ReleaseSRWLockExclusive(&core->lock);
        return false;
    }
    next.sequence = core->latest.sequence + 1;
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

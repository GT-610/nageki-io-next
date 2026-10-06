#include "button_map.h"

#include <stdio.h>
#include <string.h>

/* Record a byte value for a key unless it has already been seen. Silently stops
 * recording once the small table is full: a key that has taken eight distinct
 * values is already established as non-binary, and the census must not overflow
 * because of it. */
static void note_value(mu3_chord_census *census, unsigned key, uint8_t value)
{
    unsigned i;
    uint8_t count;
    if (census == NULL || key >= MU3_BUTTON_COUNT) return;
    count = census->value_count[key];
    for (i = 0; i < count && i < MU3_CHORD_VALUES_MAX; ++i) {
        if (census->values_seen[key][i] == value) return;
    }
    if (count < MU3_CHORD_VALUES_MAX) {
        census->values_seen[key][count] = value;
        census->value_count[key] = (uint8_t)(count + 1);
    }
}

void mu3_buttons_decode(const uint8_t *payload, uint8_t *left, uint8_t *right)
{
    uint8_t l = 0;
    uint8_t r = 0;
    unsigned i;
    if (payload != NULL) {
        for (i = 0; i < MU3_BUTTONS_PER_SIDE; ++i) {
            if (payload[i]) l = (uint8_t)(l | (1u << i));
            if (payload[i + MU3_BUTTONS_PER_SIDE]) {
                r = (uint8_t)(r | (1u << i));
            }
        }
    }
    if (left != NULL) *left = l;
    if (right != NULL) *right = r;
}

void mu3_key_name(unsigned key, char *out, size_t size)
{
    if (out == NULL || size == 0) return;
    if (key >= MU3_BUTTON_COUNT) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%c%u", key < MU3_BUTTONS_PER_SIDE ? 'L' : 'R',
             (unsigned)(key % MU3_BUTTONS_PER_SIDE) + 1);
}

bool mu3_buttons_from_report(const uint8_t *wire, size_t got, size_t wire_len,
                             uint8_t out[MU3_BUTTON_COUNT])
{
    /* The descriptor length decides whether a Report ID byte is present; the
     * returned length does not, because a short read would otherwise shift the
     * field. This mirrors mu3_hid_unpack in hid_device.c. */
    size_t at = wire_len >= MU3_WIRE_WITH_ID ? 1u : 0u;
    size_t i;
    if (out == NULL) return false;
    memset(out, 0, MU3_BUTTON_COUNT);
    if (wire == NULL) return false;
    /* Bounds are what matter, not whether the transfer was short: the bytes
     * that did arrive are real, and the layout is fixed by the descriptor, so a
     * partial read is still readable as long as the whole field is present.
     * Refusing it would silently discard a real key change mid-gesture. */
    if (got < at + MU3_BUTTON_COUNT) return false;
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) out[i] = wire[at + i];
    return true;
}

bool mu3_chord_update(mu3_chord_census *census, const uint8_t *buttons,
                      mu3_chord_step *step)
{
    mu3_chord_step local = {0};
    uint16_t down = 0, up = 0;
    unsigned held_before = 0, held_after = 0;
    unsigned i;
    bool raw_changed = false;
    uint8_t prev_left = 0, prev_right = 0, now_left = 0, now_right = 0;

    if (census == NULL || buttons == NULL) return false;
    if (step == NULL) step = &local;

    /* Always recompute both masks from the bytes. Folding the new report onto
     * the previous mask would hide exactly the case the census exists to catch:
     * bytes that moved while the decoded mask did not. */
    mu3_buttons_decode(buttons, &now_left, &now_right);
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) {
        if (buttons[i]) ++held_after;
    }

    if (!census->started) {
        /* First report only establishes a baseline. A key already held at
         * start-up is not a press, and must not be counted as one. */
        for (i = 0; i < MU3_BUTTON_COUNT; ++i) {
            census->previous[i] = buttons[i];
            note_value(census, i, buttons[i]);
        }
        census->started = true;
        step->mask_changed = false;
        step->down = 0;
        step->up = 0;
        step->held_before = (uint8_t)held_after;
        step->held_after = (uint8_t)held_after;
        return false;
    }

    for (i = 0; i < MU3_BUTTON_COUNT; ++i) {
        uint8_t before = census->previous[i];
        uint8_t after = buttons[i];
        if (before) ++held_before;
        if (before == after) continue;
        raw_changed = true;
        if (before == 0) down = (uint16_t)(down | (1u << i));
        if (after == 0) up = (uint16_t)(up | (1u << i));
        note_value(census, i, after);
    }

    if (!raw_changed) return false;

    mu3_buttons_decode(census->previous, &prev_left, &prev_right);
    step->mask_changed = (prev_left != now_left) || (prev_right != now_right);
    step->down = down;
    step->up = up;
    step->held_before = (uint8_t)held_before;
    step->held_after = (uint8_t)held_after;

    ++census->raw_changes;
    if (step->mask_changed) {
        ++census->changes;
    } else {
        /* The bytes moved but the decoded mask did not. Nonzero here for a
         * binary device would mean the polarity hypothesis is wrong. */
        ++census->byte_changes_without_mask_change;
    }

    for (i = 0; i < MU3_BUTTON_COUNT; ++i) {
        if (down & (1u << i)) {
            ++census->presses[i];
            if (held_before < MU3_BUTTON_COUNT) {
                ++census->presses_with_others[i][held_before];
            }
            /* Same report as some key's release: the shape of "press one while
             * releasing another". At 200 reports/s, anything genuinely
             * simultaneous lands in one 5 ms frame. */
            if (up != 0) ++census->presses_with_simultaneous_release[i];
        }
        if (up & (1u << i)) ++census->releases[i];
    }
    if (held_after > census->max_held) census->max_held = (uint32_t)held_after;

    for (i = 0; i < MU3_BUTTON_COUNT; ++i) census->previous[i] = buttons[i];
    return true;
}

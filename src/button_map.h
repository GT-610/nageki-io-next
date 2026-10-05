#ifndef MU3_BUTTON_MAP_H
#define MU3_BUTTON_MAP_H
/* Pure button decoding and chord census. No I/O, no Windows types, so the same
 * code runs in the DLL, in the standalone probe, and in the offline tests.
 *
 * Why the mapping lives in exactly one place: the DLL decides which wire byte
 * means which segatools key bit, and hid_probe is what a human uses to decide
 * whether the controller reported a key at all. If the probe re-derived the
 * mapping instead of sharing it, a disagreement between the two would look
 * exactly like a real input fault. Sharing the function removes that failure
 * mode.
 *
 * Layout hypothesis, from protocol analysis of the replaced DLL rather than a
 * capture (see docs/field-notes.zh-CN.md section 10): one byte per key, offsets
 * 0..4 the five left keys in the order 1,2,3,SIDE,MENU and offsets 5..9 the
 * five right keys in the same order. A byte counts as pressed when it is
 * nonzero, so a device reporting 0x00/0xFF behaves like one reporting 0/1.
 * Which physical key sits at which offset is still unconfirmed, but the census
 * below only ever compares an offset against itself, so its result does not
 * depend on the names being right. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MU3_BUTTON_COUNT 10
#define MU3_BUTTONS_PER_SIDE 5
/* Wire length at and above which byte 0 is a Report ID rather than payload.
 * Same value as MU3_HID_WIRE in hid_device.h; repeated here so this module
 * stays free of Windows headers. */
#define MU3_WIRE_WITH_ID 65u
/* Enough distinct byte values per key to characterise its polarity without
 * storing a 256-entry table per key. */
#define MU3_CHORD_VALUES_MAX 8

/* Decode the ten discrete button bytes into segatools' two five-bit masks
 * (left bits 0..4 from offsets 0..4, right bits 0..4 from offsets 5..9).
 *
 * Every byte sets its own bit and nothing else: no bit is shifted across bytes
 * and no byte is combined with another. That is the property that makes a
 * combination-dependent input fault impossible in this layer, and
 * tests/button_map_tests.c asserts it exhaustively over all 1024 possible
 * byte vectors. out pointers may be NULL; a NULL payload decodes to all-up. */
void mu3_buttons_decode(const uint8_t *payload, uint8_t *left, uint8_t *right);

/* Copy the ten button bytes out of a wire report into `out`, in payload order.
 *
 * Whether byte 0 of the transfer is a Report ID or the first payload byte is a
 * property of the descriptor, not of how many bytes happened to arrive: a
 * 65-byte report places the payload at wire offset 1, a 64-byte one at 0. It is
 * the same rule mu3_hid_unpack applies, extracted so hid_probe can read the
 * field exactly as the DLL would instead of re-deriving the offsets. Getting
 * this wrong in the probe would misread which key moved, which is
 * indistinguishable from a real input fault, so it is tested.
 *
 * Returns false when the transfer is too short to contain the field (a short
 * read must not be read out of bounds, and a truncated field must not be
 * reported as keys being up). `wire_len` is the descriptor length and `got` the
 * number of bytes actually transferred. */
bool mu3_buttons_from_report(const uint8_t *wire, size_t got, size_t wire_len,
                             uint8_t out[MU3_BUTTON_COUNT]);

/* One report's worth of change, as seen in the ten button bytes. */
typedef struct mu3_chord_step {
    bool mask_changed;   /* decoded press mask differs from the previous report */
    uint16_t down;       /* bit k: key k's byte went zero -> nonzero */
    uint16_t up;         /* bit k: key k's byte went nonzero -> zero */
    uint8_t held_before; /* keys nonzero before this report */
    uint8_t held_after;  /* keys nonzero in this report */
} mu3_chord_step;

/* Running census over a session of button reports.
 *
 * The number that matters for a "hold four, press a fifth" investigation is
 * presses_with_others[k][4]: how often key k went down while exactly four
 * other keys were already held. Nonzero for any k means the device does report
 * a new key under that combination. */
typedef struct mu3_chord_census {
    uint8_t previous[MU3_BUTTON_COUNT];
    bool started;
    uint32_t changes;      /* reports where the decoded mask changed */
    uint32_t raw_changes;  /* reports where any button byte changed at all */
    uint32_t byte_changes_without_mask_change;
    uint32_t presses[MU3_BUTTON_COUNT];
    uint32_t releases[MU3_BUTTON_COUNT];
    /* presses_with_others[k][n]: key k went down while n other keys were held.
     * Keys going down together in one report are all filed under the count that
     * held before it, because at 200 reports/s they are the same 5 ms frame. */
    uint32_t presses_with_others[MU3_BUTTON_COUNT][MU3_BUTTON_COUNT];
    /* presses that shared their report with some key's release, which is the
     * shape of "press one while releasing another". */
    uint32_t presses_with_simultaneous_release[MU3_BUTTON_COUNT];
    uint32_t max_held;
    /* Distinct byte values each key has taken, in the order first seen. */
    uint8_t values_seen[MU3_BUTTON_COUNT][MU3_CHORD_VALUES_MAX];
    uint8_t value_count[MU3_BUTTON_COUNT];
} mu3_chord_census;

/* Feed one report's ten button bytes, in payload order.
 *
 * Returns true when anything changed (including a byte changing value without
 * changing the decoded mask, which is reported through step->mask_changed being
 * false). The first call only establishes a baseline: it records the bytes and
 * returns false, so a state that was already held at start-up is not counted as
 * a press. */
bool mu3_chord_update(mu3_chord_census *census, const uint8_t *buttons,
                      mu3_chord_step *step);

#endif

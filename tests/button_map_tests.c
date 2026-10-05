/* Tests for the shared button decode and the chord census.
 *
 * The decode test is the important one. It asserts, over every one of the 1024
 * possible on/off vectors of the ten button bytes, that the decoded masks are
 * exactly the bitwise OR of the masks each set byte produces on its own. That
 * is per-key independence, and it is what rules out a *combination-dependent*
 * input fault originating in this DLL: no key's state can influence another
 * key's bit. A "hold four keys and a fifth does not register" symptom therefore
 * cannot be produced here.
 *
 * The census tests guard the diagnostic that a human reads to decide whether
 * the controller reported the key at all. A wrong count would be read as a
 * wrong verdict, so the counting rules are pinned here.
 */
#include "../src/button_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_decode_basics(void)
{
    uint8_t buttons[MU3_BUTTON_COUNT];
    uint8_t left = 0xEE, right = 0xEE;

    memset(buttons, 0, sizeof(buttons));
    mu3_buttons_decode(buttons, &left, &right);
    assert(left == 0 && right == 0);

    memset(buttons, 1, sizeof(buttons));
    mu3_buttons_decode(buttons, &left, &right);
    assert(left == 0x1F && right == 0x1F);

    /* Offsets 0..4 are the left keys, 5..9 the right, one bit each. */
    memset(buttons, 0, sizeof(buttons));
    buttons[0] = 1;
    buttons[4] = 1;
    buttons[5] = 1;
    buttons[9] = 1;
    mu3_buttons_decode(buttons, &left, &right);
    assert(left == 0x11 && right == 0x11);

    /* Nonzero is pressed, so a device reporting 0xFF or 0x80 behaves like one
     * reporting 1. This is the normalisation the frozen DLL's OR also gave.
     * Offsets 7 and 8 are the right side's bits 2 and 3. */
    memset(buttons, 0, sizeof(buttons));
    buttons[2] = 0xFF;
    buttons[7] = 0x80;
    buttons[8] = 0x02;
    mu3_buttons_decode(buttons, &left, &right);
    assert(left == 0x04 && right == 0x0C);

    /* NULL output pointers and a NULL payload must not crash. */
    mu3_buttons_decode(buttons, NULL, NULL);
    mu3_buttons_decode(NULL, &left, &right);
    assert(left == 0 && right == 0);
}

/* The rule that matters: a key's bit depends on that key's byte alone. */
static void test_decode_is_per_key_independent(void)
{
    uint8_t singles[MU3_BUTTON_COUNT][MU3_BUTTON_COUNT];
    uint8_t single_left[MU3_BUTTON_COUNT], single_right[MU3_BUTTON_COUNT];
    uint8_t buttons[MU3_BUTTON_COUNT];
    unsigned key, bits;

    for (key = 0; key < MU3_BUTTON_COUNT; ++key) {
        memset(singles[key], 0, sizeof(singles[key]));
        singles[key][key] = 1;
        mu3_buttons_decode(singles[key], &single_left[key], &single_right[key]);
        /* Each single key must light exactly one bit on exactly one side. */
        assert((single_left[key] ? 1 : 0) + (single_right[key] ? 1 : 0) == 1);
    }

    /* Exhaustive over all 2^10 on/off vectors. */
    for (bits = 0; bits < (1u << MU3_BUTTON_COUNT); ++bits) {
        uint8_t want_left = 0, want_right = 0, got_left, got_right;
        for (key = 0; key < MU3_BUTTON_COUNT; ++key) {
            buttons[key] = (bits & (1u << key)) ? 1 : 0;
            if (bits & (1u << key)) {
                want_left = (uint8_t)(want_left | single_left[key]);
                want_right = (uint8_t)(want_right | single_right[key]);
            }
        }
        mu3_buttons_decode(buttons, &got_left, &got_right);
        assert(got_left == want_left);
        assert(got_right == want_right);
    }
}

/* The census's reason for existing: it must report how many keys were already
 * held when a new one went down. */
static void test_census_chord_counts_held_keys(void)
{
    mu3_chord_census census;
    mu3_chord_step step;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};
    unsigned key;

    memset(&census, 0, sizeof(census));
    /* Baseline: nothing held. The first call must not count a press. */
    assert(!mu3_chord_update(&census, buttons, &step));
    assert(census.presses[0] == 0 && census.raw_changes == 0);

    /* Hold keys 0..3. */
    for (key = 0; key < 4; ++key) {
        buttons[key] = 1;
        assert(mu3_chord_update(&census, buttons, &step));
        assert(step.held_before == key);
        assert(step.held_after == key + 1);
        assert(step.down == (1u << key));
        assert(step.up == 0);
    }
    assert(census.presses_with_others[0][0] == 1);
    assert(census.presses_with_others[1][1] == 1);
    assert(census.presses_with_others[2][2] == 1);
    assert(census.presses_with_others[3][3] == 1);
    assert(census.max_held == 4);

    /* The gesture under investigation: press the fifth key on the same report
     * that releases the second. This is the count that decides the question. */
    buttons[4] = 1;
    buttons[1] = 0;
    assert(mu3_chord_update(&census, buttons, &step));
    assert(step.down == (1u << 4));
    assert(step.up == (1u << 1));
    assert(step.held_before == 4 && step.held_after == 4);
    assert(census.presses[4] == 1);
    assert(census.presses_with_others[4][4] == 1);
    assert(census.presses_with_simultaneous_release[4] == 1);
    /* The released key is not credited with a press. */
    assert(census.presses_with_simultaneous_release[1] == 0);

    /* A report with no change must not be counted at all. */
    assert(!mu3_chord_update(&census, buttons, &step));
    assert(census.raw_changes == 5);
}

/* A byte that changes value without changing the decoded mask means the device
 * is not binary. It must be counted, not silently dropped, because it is the
 * one shape that would falsify the "nonzero means pressed" hypothesis. */
static void test_census_counts_non_binary_bytes(void)
{
    mu3_chord_census census;
    mu3_chord_step step;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};

    memset(&census, 0, sizeof(census));
    mu3_chord_update(&census, buttons, &step);
    buttons[3] = 0x01;
    assert(mu3_chord_update(&census, buttons, &step));
    assert(step.mask_changed && step.down == (1u << 3));
    buttons[3] = 0x02; /* still pressed, different byte */
    assert(mu3_chord_update(&census, buttons, &step));
    assert(!step.mask_changed);
    assert(step.down == 0 && step.up == 0);
    assert(census.byte_changes_without_mask_change == 1);
    assert(census.changes == 1);
    assert(census.raw_changes == 2);
    /* Both values must have been recorded for that key. */
    assert(census.value_count[3] == 3); /* 0x00, 0x01, 0x02 */
}

/* A key already held at start-up is the baseline, not a press: otherwise every
 * session would open with a phantom chord. */
static void test_census_baseline_is_not_a_press(void)
{
    mu3_chord_census census;
    mu3_chord_step step;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};

    memset(&census, 0, sizeof(census));
    buttons[0] = buttons[5] = 1;
    assert(!mu3_chord_update(&census, buttons, &step));
    assert(step.held_after == 2);
    assert(census.presses[0] == 0 && census.presses[5] == 0);
    assert(census.raw_changes == 0);
    assert(census.value_count[0] == 1 && census.values_seen[0][0] == 1);

    /* Degenerate inputs must not crash or corrupt the census. */
    assert(!mu3_chord_update(NULL, buttons, &step));
    assert(!mu3_chord_update(&census, NULL, &step));
    assert(mu3_chord_update(&census, buttons, NULL) == false);
}

/* Where the button field sits depends on the descriptor length, and the
 * descriptor length (not the returned byte count) decides whether byte 0 is a
 * Report ID. Reading it wrong would report the wrong key, which is
 * indistinguishable from a real input fault, so every combination is pinned. */
static void test_report_extraction(void)
{
    uint8_t wire[MU3_WIRE_WITH_ID + 8];
    uint8_t out[MU3_BUTTON_COUNT];
    unsigned i;

    for (i = 0; i < sizeof(wire); ++i) wire[i] = (uint8_t)(0xA0 + i);

    /* 65-byte report: byte 0 is the ID, the field starts at wire offset 1. */
    assert(mu3_buttons_from_report(wire, MU3_WIRE_WITH_ID, MU3_WIRE_WITH_ID, out));
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) assert(out[i] == wire[1 + i]);

    /* 64-byte report: payload only, the field starts at wire offset 0. */
    assert(mu3_buttons_from_report(wire, 64, 64, out));
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) assert(out[i] == wire[i]);

    /* A short read is still readable while the whole field is present: the
     * bytes that arrived are real and the layout is fixed by the descriptor,
     * so refusing it would discard a real key change mid-gesture. */
    assert(mu3_buttons_from_report(wire, 64, 65, out));
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) assert(out[i] == wire[1 + i]);
    assert(mu3_buttons_from_report(wire, 11, 65, out));
    assert(mu3_buttons_from_report(wire, 10, 64, out));
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) assert(out[i] == wire[i]);

    /* A transfer that does not contain the whole field must be refused, not
     * read past the transferred data nor reported as keys being up. */
    assert(!mu3_buttons_from_report(wire, 10, 65, out));
    assert(!mu3_buttons_from_report(wire, 9, 65, out));
    assert(!mu3_buttons_from_report(wire, 9, 64, out));
    assert(!mu3_buttons_from_report(wire, 0, 65, out));
    assert(!mu3_buttons_from_report(wire, 0, 64, out));

    /* Degenerate arguments: the output is always defined on failure. */
    memset(out, 0xEE, sizeof(out));
    assert(!mu3_buttons_from_report(NULL, 65, 65, out));
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) assert(out[i] == 0);
    assert(!mu3_buttons_from_report(wire, 65, 65, NULL));
}

int main(void)
{
    test_decode_basics();
    test_decode_is_per_key_independent();
    test_report_extraction();
    test_census_chord_counts_held_keys();
    test_census_counts_non_binary_bytes();
    test_census_baseline_is_not_a_press();
    puts("button map tests passed");
    return 0;
}

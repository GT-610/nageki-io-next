/* Tests for the HID marshalling between the fixed 65-byte frame and the
 * device's descriptor-reported transfer length.
 *
 * The defect this guards: writing the frame straight to a device whose
 * OutputReportByteLength is 64 puts the Report ID byte where the first payload
 * byte belongs, shifting the entire payload and dropping its last byte. For an
 * LED frame that mis-drives the lights rather than failing visibly.
 *
 * These are pure functions, so no device is needed. */
#include "../src/hid_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_exact_65_is_identity(void)
{
    uint8_t frame[MU3_HID_WIRE], wire[MU3_HID_WIRE], back[MU3_HID_WIRE];
    unsigned i;
    for (i = 0; i < MU3_HID_WIRE; ++i) frame[i] = (uint8_t)(0x40 + i);
    frame[0] = 0x00; /* real report ID */

    mu3_hid_pack(frame, wire, sizeof(wire));
    assert(memcmp(wire, frame, MU3_HID_WIRE) == 0);

    mu3_hid_unpack(wire, sizeof(wire), back);
    assert(memcmp(back, frame, MU3_HID_WIRE) == 0);
}

/* A 64-byte report carries payload only. The frame's ID byte must be dropped,
 * not shifted into the payload. */
static void test_64_byte_report_drops_the_id_byte(void)
{
    uint8_t frame[MU3_HID_WIRE], wire[MU3_HID_PAYLOAD], back[MU3_HID_WIRE];
    unsigned i;
    for (i = 0; i < MU3_HID_WIRE; ++i) frame[i] = (uint8_t)i;
    frame[0] = 0x00;

    mu3_hid_pack(frame, wire, sizeof(wire));
    /* Payload lands at wire[0..63]: the frame's byte 1 must be wire byte 0. */
    assert(wire[0] == frame[1]);
    assert(memcmp(wire, frame + 1, MU3_HID_PAYLOAD) == 0);
    /* The last payload byte survives; the old code lost it. */
    assert(wire[MU3_HID_PAYLOAD - 1] == frame[MU3_HID_PAYLOAD]);

    mu3_hid_unpack(wire, sizeof(wire), back);
    assert(back[0] == 0); /* ID re-supplied as 0 */
    assert(memcmp(back + 1, wire, MU3_HID_PAYLOAD) == 0);
}

/* The LED frame is the real case: board 1 colours live at payload offsets 2..10
 * and 17..25 (wire offsets 3..11 and 18..26). Verify a colour byte arriving at
 * the right wire offset in both layouts. */
static void test_led_payload_offsets(void)
{
    uint8_t frame[MU3_HID_WIRE] = {0};
    uint8_t wire[MU3_HID_WIRE];
    frame[0] = 0x00;
    frame[1] = 0x00; /* payload type */
    frame[2] = 0xFF; /* brightness */
    frame[3] = 0xFF; /* first red channel */
    frame[26] = 0xFF; /* last channel of the second group */

    mu3_hid_pack(frame, wire, 65);
    assert(wire[0] == 0x00 && wire[1] == 0x00 && wire[2] == 0xFF);
    assert(wire[3] == 0xFF && wire[26] == 0xFF);

    /* Same logical frame in a 64-byte report shifts down by one. */
    mu3_hid_pack(frame, wire, 64);
    assert(wire[0] == 0x00 && wire[1] == 0xFF);
    assert(wire[2] == 0xFF && wire[25] == 0xFF);
}

/* A long descriptor must not overrun anything, and the tail is zeroed so no
 * stale bytes from a previous transfer leak onto the wire. */
static void test_longer_report_is_padded_and_zeroed(void)
{
    uint8_t frame[MU3_HID_WIRE], wire[128], back[MU3_HID_WIRE];
    unsigned i;
    memset(wire, 0xAA, sizeof(wire));
    for (i = 0; i < MU3_HID_WIRE; ++i) frame[i] = (uint8_t)(0x10 + i);

    mu3_hid_pack(frame, wire, sizeof(wire));
    assert(wire[0] == frame[0]);
    assert(memcmp(wire + 1, frame + 1, MU3_HID_PAYLOAD) == 0);
    for (i = MU3_HID_WIRE; i < sizeof(wire); ++i) assert(wire[i] == 0);

    /* Reading back only the first 65 bytes recovers the frame. */
    mu3_hid_unpack(wire, MU3_HID_WIRE, back);
    assert(memcmp(back, frame, MU3_HID_WIRE) == 0);
}

/* Short and degenerate lengths must stay in bounds. */
static void test_short_and_degenerate_lengths(void)
{
    uint8_t frame[MU3_HID_WIRE], wire[MU3_HID_WIRE], back[MU3_HID_WIRE];
    unsigned i;
    for (i = 0; i < MU3_HID_WIRE; ++i) frame[i] = (uint8_t)i;

    memset(wire, 0xAA, sizeof(wire));
    mu3_hid_pack(frame, wire, 1);
    assert(wire[0] == frame[1]); /* first payload byte only */

    mu3_hid_pack(frame, wire, 0); /* must not write anything or crash */
    mu3_hid_pack(NULL, wire, 8);  /* must not crash */
    mu3_hid_pack(frame, NULL, 8); /* must not crash */

    /* Reading nothing yields a fully zeroed frame with ID 0. */
    mu3_hid_unpack(wire, 0, back);
    for (i = 0; i < MU3_HID_WIRE; ++i) assert(back[i] == 0);

    /* A partial read shorter than the payload is zero-padded, not stale. */
    memset(wire, 0x5A, sizeof(wire));
    mu3_hid_unpack(wire, 3, back);
    assert(back[0] == 0);
    assert(back[1] == 0x5A && back[3] == 0x5A);
    for (i = 4; i < MU3_HID_WIRE; ++i) assert(back[i] == 0);

    mu3_hid_unpack(NULL, 10, back); /* must not crash */
    for (i = 0; i < MU3_HID_WIRE; ++i) assert(back[i] == 0);
}

/* pack then unpack must be lossless whenever the report carries a payload, and
 * lossy only in the expected way (the ID byte) when it does not. */
static void test_round_trip_over_all_lengths(void)
{
    uint8_t frame[MU3_HID_WIRE], wire[MU3_HID_MAX_REPORT], back[MU3_HID_WIRE];
    size_t len;
    unsigned i;
    for (i = 0; i < MU3_HID_WIRE; ++i) frame[i] = (uint8_t)(i * 7 + 1);
    frame[0] = 0x00;

    for (len = 1; len <= sizeof(wire); ++len) {
        memset(wire, 0xCC, sizeof(wire));
        mu3_hid_pack(frame, wire, len);
        mu3_hid_unpack(wire, len, back);
        if (len >= MU3_HID_WIRE) {
            assert(memcmp(back, frame, MU3_HID_WIRE) == 0);
        } else {
            assert(back[0] == 0);
            assert(memcmp(back + 1, frame + 1, len) == 0);
        }
    }
}

int main(void)
{
    test_exact_65_is_identity();
    test_64_byte_report_drops_the_id_byte();
    test_led_payload_offsets();
    test_longer_report_is_padded_and_zeroed();
    test_short_and_degenerate_lengths();
    test_round_trip_over_all_lengths();
    puts("HID marshalling tests passed");
    return 0;
}

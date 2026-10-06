#include "../src/led_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Covers the rgb-to-frame mapping in led_packet.c. Marshalling the finished
 * frame onto the device is hid_pack_tests.c's job, not this file's. */
int main(void)
{
    uint8_t rgb[18] = {0}, out[65];
    unsigned i;
    /* Every channel is on/off: any nonzero value becomes 255, and the two
     * nine-byte groups land at payload offsets 2..10 and 17..25. */
    rgb[0] = 1; rgb[8] = 127; rgb[9] = 255; rgb[17] = 1;
    mu3_led_packet(rgb, out);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 255);
    assert(out[3] == 255 && out[11] == 255 && out[18] == 255 && out[26] == 255);
    for (i = 3; i < 65; ++i) {
        if (i == 11 || i == 18 || i == 26) continue; /* the lit channels */
        assert(out[i] == 0);
    }
    /* An all-zero input must clear the channels without disturbing the header. */
    memset(rgb, 0, sizeof(rgb));
    mu3_led_packet(rgb, out);
    assert(out[2] == 255);
    for (i = 3; i < 65; ++i) assert(out[i] == 0);
    puts("LED packet tests passed");
    return 0;
}

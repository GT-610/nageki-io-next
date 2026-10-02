#include "../src/led_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    uint8_t rgb[18] = {0}, out[65];
    unsigned i;
    rgb[0] = 1; rgb[8] = 127; rgb[9] = 255; rgb[17] = 1;
    mu3_led_packet(rgb, out);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 255);
    assert(out[3] == 255 && out[11] == 255 && out[18] == 255 && out[26] == 255);
    for (i = 0; i < 65; ++i) {
        if (i == 2 || i == 3 || i == 11 || i == 18 || i == 26) continue;
        assert(out[i] == 0);
    }
    memset(rgb, 0, sizeof(rgb));
    mu3_led_packet(rgb, out);
    for (i = 3; i < 65; ++i) assert(out[i] == 0);
    puts("LED packet tests passed");
    return 0;
}

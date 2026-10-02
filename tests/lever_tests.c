#include "../src/lever.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int s16(uint16_t v) { return v < 0x8000u ? (int)v : (int)v - 65536; }

int main(void)
{
    /* Centre is exactly 0 as measured on the cabinet: the game shows 0000H. */
    assert(mu3_lever_from_raw(0) == 0);

    /* The three measured cabinet readings, with the frozen DLL's settings
     * (LeverSensitivity = 2, LeverOffset = 0, no clamp). The measured left
     * B0FFH / right 557FH are approximate, not multiples of 64; the exact
     * reachable points bracket them. */
    {
        int left = mu3_lever_from_raw((uint16_t)(-316));
        int right = mu3_lever_from_raw(342);
        printf("left  = %04X (measured B0FF)\n", (unsigned)(uint16_t)left);
        printf("right = %04X (measured 557F)\n", (unsigned)(uint16_t)right);
        assert((uint16_t)left == 0xB100);
        assert((uint16_t)right == 0x5580);
        assert(abs(s16(0xB0FFu) - left) <= 64);
        assert(abs(s16(0x557Fu) - right) <= 64);
        /* Sign convention: left is negative/high-MSB, right positive. This
         * matches mu3io.h (real cabinet sits near 0xB000 left, 0x5000 right). */
        assert(left < 0 && right > 0);
        /* The physical span is about 660 counts end to end, so the output
         * stays far from the int16 limits and never wraps in normal play. */
        assert(left == -20224 && right == 21888);
        assert(abs(right - left) < 60000);
    }

    /* Linear, no dead zone: halving raw roughly halves the output. */
    assert(mu3_lever_from_raw((uint16_t)(-158)) ==
           mu3_lever_from_raw((uint16_t)(-316)) / 2);

    /* raw*64 mod 65536: only the low 10 bits of raw can matter, because
     * 1024 * 64 == 65536 exactly. */
    assert(mu3_lever_from_raw(0x0100) == 16384);  /* 256 * 64 */
    assert(mu3_lever_from_raw(0x0400) == 0);      /* 1024 * 64 wraps to 0 */
    assert(mu3_lever_from_raw(0x0800) == 0);      /* 2048 * 64 also wraps */
    assert(mu3_lever_from_raw(0x07FF) == -64);    /* 2047 * 64 = 131008 */
    assert(mu3_lever_from_raw(0xFFFF) == -64);    /* unsigned wrap, as ushort */
    for (unsigned raw = 0; raw < 1024; ++raw) {
        assert(mu3_lever_from_raw((uint16_t)raw) ==
               mu3_lever_from_raw((uint16_t)(raw + 1024)));
    }

    /* Every 16-bit input must stay in int16 range after the narrowing wrap. */
    for (unsigned raw = 0; raw <= 0xFFFFu; ++raw) {
        int v = mu3_lever_from_raw((uint16_t)raw);
        assert(v >= -32768 && v <= 32767);
    }

    puts("lever conversion tests passed");
    return 0;
}

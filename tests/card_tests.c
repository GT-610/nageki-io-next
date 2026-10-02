#include "../src/card_id.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    uint8_t in[8] = {0}, out[10];
    uint8_t expected[10] = {0};
    mu3_felica_to_aime_bcd(in, out);
    assert(memcmp(out, expected, 10) == 0);
    in[7] = 0x7b; /* 123 decimal -> ... 01 23 */
    expected[8] = 0x01; expected[9] = 0x23;
    mu3_felica_to_aime_bcd(in, out);
    assert(memcmp(out, expected, 10) == 0);
    { /* UINT64_MAX = 18446744073709551615 */
        const uint8_t maximum[8] = {255,255,255,255,255,255,255,255};
        const uint8_t digits[10] = {0x18,0x44,0x67,0x44,0x07,0x37,0x09,0x55,0x16,0x15};
        mu3_felica_to_aime_bcd(maximum, out);
        assert(memcmp(out, digits, 10) == 0);
    }
    puts("card conversion tests passed");
    return 0;
}

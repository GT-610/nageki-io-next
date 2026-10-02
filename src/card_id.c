#include "card_id.h"
#include <stddef.h>

void mu3_felica_to_aime_bcd(const uint8_t input[8], uint8_t output[10])
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8; ++i) value = (value << 8) | input[i];
    for (i = 10; i > 0; --i) {
        uint8_t low = (uint8_t)(value % 10u);
        value /= 10u;
        output[i - 1] = (uint8_t)(low | (uint8_t)((value % 10u) << 4));
        value /= 10u;
    }
}

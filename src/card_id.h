#ifndef MU3_CARD_ID_H
#define MU3_CARD_ID_H
#include <stdint.h>
/* Legacy scan=2: interpret first eight ID bytes as a big-endian unsigned
 * hexadecimal integer, then encode its 20 decimal digits as ten BCD bytes. */
void mu3_felica_to_aime_bcd(const uint8_t input[8], uint8_t output[10]);
#endif

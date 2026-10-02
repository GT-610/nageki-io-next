#include "lever.h"

int16_t mu3_lever_from_raw(uint16_t raw)
{
    /* Replicates (short)(raw * 32 * sensitivity) with offset 0: compute in
     * 32 bits, then wrap modulo 65536 exactly as the narrowing cast did. */
    uint32_t scaled = ((uint32_t)raw * (uint32_t)MU3_LEVER_SCALE) +
                      (uint32_t)(MU3_LEVER_OFFSET * 100);
    uint16_t bits = (uint16_t)(scaled & 0xFFFFu);
    return bits < 0x8000u ? (int16_t)bits : (int16_t)((int32_t)bits - 65536);
}

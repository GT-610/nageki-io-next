#ifndef MU3_LEVER_H
#define MU3_LEVER_H
#include <stdint.h>

/* Lever conversion, isolated so it can be tested against measured cabinet
 * values without linking the DLL.
 *
 * The frozen DLL computed exactly:
 *     lever = (short)(LeverOffset * 100.0 + raw * 32.0 * LeverSensitivity)
 * with LeverOffset = 0 and no clamp (the cast wraps modulo 65536).
 *
 * Measured on the cabinet with the DLL at LeverSensitivity = 2 (centre,
 * full left, full right as shown by the game's lever calibration):
 *
 *     centre 0000H   left B0FFH   right 557FH
 *
 * Those correspond to raw = 0, about -316 and about +342, i.e. raw is a
 * signed count centred on zero. 32 * 2 = 64 reproduces them: raw -316 gives
 * B100H and raw +342 gives 5580H, bracketing the approximate readings, and
 * the sign convention (left negative, high MSB) matches mu3io.h's note that a
 * real cabinet sits near 0xB000 on the left and 0x5000 on the right.
 *
 * So sensitivity 2 is the movement-rate baseline, exactly as the frozen DLL
 * behaved when configured that way. */
#define MU3_LEVER_SENSITIVITY 2
#define MU3_LEVER_SCALE (32 * MU3_LEVER_SENSITIVITY)
#define MU3_LEVER_OFFSET 0

/* raw is the device's unsigned 16-bit lever field; the frozen DLL treated it
 * as ushort and relied on the narrowing cast wrapping, which is what the
 * explicit modulo below reproduces portably. */
int16_t mu3_lever_from_raw(uint16_t raw);

#endif

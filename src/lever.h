#ifndef MU3_LEVER_H
#define MU3_LEVER_H
#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

/* Lever conversion. Kept free of I/O state so it can be tested against real
 * captured values without loading the DLL.
 *
 * The original DLL computed, with offset defaulting to 0 and no clamp:
 *     lever = (short)( LeverOffset * 100 + raw * 32 * LeverSensitivity )
 * and the narrowing cast wraps modulo 65536. It never subtracted a centre, so
 * it implicitly assumed the firmware reported 0 at the stick's centre.
 *
 * It does not. A capture from the deployed controller (hid_probe.exe dump)
 * reads the lever field directly:
 *     full left 0x032A (810)   at rest 0x0402 (1026)   full right 0x04A4 (1188)
 *
 * The electrical centre is 0x0400 (1024). The probe pins it, and the odd/even
 * sensitivity behaviour agrees: the original centres only when raw_centre *
 * sensitivity is a multiple of 2048, and "even works, odd reads 0x8000" forces
 * v2(raw_centre) == 10, i.e. raw_centre = 1024 * odd. Of the candidates in the
 * 10-bit range, 1024 is 2 counts from the measured rest value while 3072 is
 * 2046 away.
 *
 * Subtracting the centre removes the odd/even bug and, because the centre no
 * longer consumes range, allows a higher sensitivity. At the deployed
 * sensitivity 2 the centred conversion is bit-identical to the original for
 * every raw value, so this is not a behavioural regression. */
#define MU3_LEVER_NEUTRAL_DEFAULT 1024
#define MU3_LEVER_SENSITIVITY_DEFAULT 2
#define MU3_LEVER_SENSITIVITY_MIN 1
/* Highest sensitivity that keeps the captured travel monotonic: the largest
 * excursion from centre is 214 counts, and 214 * 32 * 5 overflows int16. */
#define MU3_LEVER_SENSITIVITY_MAX 4

typedef struct mu3_lever_config {
    int neutral;     /* raw value at the electrical centre, 0..65535 */
    int sensitivity; /* integer, same meaning as the original setting */
} mu3_lever_config;

void mu3_lever_config_defaults(mu3_lever_config *cfg);
bool mu3_lever_config_valid(const mu3_lever_config *cfg);
/* Replicates (short)((raw - neutral) * 32 * sensitivity) with the original's
 * modulo-65536 narrowing. Invalid configs fall back to the defaults. */
int16_t mu3_lever_convert(uint16_t raw, const mu3_lever_config *cfg);
/* Optional overrides from the DLL's own INI file. Missing or out-of-range keys
 * keep the values already in cfg, so a partially edited file is safe. */
void mu3_lever_config_load(mu3_lever_config *cfg, const wchar_t *path);

#endif

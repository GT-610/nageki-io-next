/* Regression tests for the lever conversion, built from a real capture on the
 * deployed controller (hid_probe.exe dump):
 *
 *     full left 0x032A (810)   at rest 0x0402 (1026)   full right 0x04A4 (1188)
 *
 * The firmware's electrical centre is 0x0400 (1024); see src/lever.h for why
 * the odd/even sensitivity behaviour of the original formula pins it there.
 */
#include "../src/lever.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define RAW_LEFT 0x032Au
#define RAW_REST 0x0402u
#define RAW_RIGHT 0x04A4u
#define CENTRE 1024

static mu3_lever_config cfg;

/* The frozen DLL's formula with LeverOffset = 0: no centre subtraction. */
static int16_t original(uint16_t raw, int sensitivity)
{
    int64_t scaled = (int64_t)raw * 32 * sensitivity;
    uint16_t bits = (uint16_t)((uint64_t)scaled & 0xFFFFu);
    return bits < 0x8000u ? (int16_t)bits : (int16_t)((int32_t)bits - 65536);
}

static void test_defaults(void)
{
    mu3_lever_config_defaults(&cfg);
    assert(cfg.neutral == MU3_LEVER_NEUTRAL_DEFAULT);
    assert(cfg.neutral == CENTRE);
    assert(cfg.sensitivity == MU3_LEVER_SENSITIVITY_DEFAULT);
    assert(mu3_lever_config_valid(&cfg));

    /* Out-of-range configs are rejected rather than trusted. */
    {
        const mu3_lever_config bad[] = {
            {-1, 2}, {65536, 2}, {1024, 0}, {1024, 9}
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            assert(!mu3_lever_config_valid(&bad[i]));
        }
    }
}

static void test_capture_values(void)
{
    mu3_lever_config_defaults(&cfg);
    /* Full left / right as captured, at the default sensitivity 2. */
    assert(mu3_lever_convert(RAW_LEFT, &cfg) == -13696);  /* (810-1024)*64 */
    assert(mu3_lever_convert(RAW_RIGHT, &cfg) == 10496);  /* (1188-1024)*64 */

    /* The captured rest position is 2 counts off the electrical centre, which
     * is why it is not exactly 0: (1026-1024)*64 = 128. */
    assert(mu3_lever_convert(RAW_REST, &cfg) == 128);

    /* Sign convention: left is negative, right positive. */
    assert(mu3_lever_convert(RAW_LEFT, &cfg) < 0);
    assert(mu3_lever_convert(RAW_RIGHT, &cfg) > 0);
}

/* THE fix for the odd/even bug: with the centre subtracted, the electrical
 * centre reads exactly 0 for every integer sensitivity. Under the original
 * formula every odd sensitivity read 0x8000 there instead. */
static void test_centre_is_zero_at_any_sensitivity(void)
{
    int sensitivity;
    for (sensitivity = MU3_LEVER_SENSITIVITY_MIN;
         sensitivity <= MU3_LEVER_SENSITIVITY_MAX; ++sensitivity) {
        cfg.sensitivity = sensitivity;
        assert(mu3_lever_convert(CENTRE, &cfg) == 0);
    }
    cfg.sensitivity = MU3_LEVER_SENSITIVITY_DEFAULT;
}

/* No regression at the deployed baseline: for even sensitivities the centred
 * conversion is bit-identical to the original, because (raw-1024)*32*sens and
 * raw*32*sens differ by a multiple of 65536 exactly when sens is even. */
static void test_no_regression_at_even_sensitivity(void)
{
    int sensitivity;
    unsigned raw;
    for (sensitivity = 2; sensitivity <= MU3_LEVER_SENSITIVITY_MAX; sensitivity += 2) {
        cfg.sensitivity = sensitivity;
        for (raw = 0; raw <= 0xFFFFu; ++raw) {
            assert(mu3_lever_convert((uint16_t)raw, &cfg) ==
                   original((uint16_t)raw, sensitivity));
        }
    }
    cfg.sensitivity = MU3_LEVER_SENSITIVITY_DEFAULT;
}

/* Where the original was broken: at odd sensitivities the whole mapping was
 * shifted by half of int16 and the largest excursions wrapped to the far end.
 * The centred conversion keeps the sign and stays monotonic. */
static void test_odd_sensitivity_is_monotonic(void)
{
    int sensitivity;
    assert(original(CENTRE, 1) == -32768); /* what odd sensitivity used to do */
    for (sensitivity = MU3_LEVER_SENSITIVITY_MIN;
         sensitivity <= MU3_LEVER_SENSITIVITY_MAX; ++sensitivity) {
        int left, right;
        cfg.sensitivity = sensitivity;
        left = mu3_lever_convert(RAW_LEFT, &cfg);
        right = mu3_lever_convert(RAW_RIGHT, &cfg);
        assert(left < 0 && right > 0);
        assert(left < right);
    }
    cfg.sensitivity = MU3_LEVER_SENSITIVITY_DEFAULT;
}

/* Settles which quantity the cabinet's calibration screen displays. A lever
 * value is always a multiple of 32 * sensitivity, so it is even, and it is a
 * multiple of 64 whenever the scale is. The two readings observed on the
 * cabinet, B0FFH and 557FH, are both odd and both congruent to 63 modulo 64,
 * so they cannot be lever values. They are segatools' adcs[0] = 0x7FFF - lever,
 * which is what a lever value maps onto: 0x7FFF is 63 modulo 64, and
 * subtracting a multiple of 64 leaves that residue untouched. The readings
 * were taken at sensitivity 2, where the scale is 64. */
static void test_calibration_readings_are_not_lever_values(void)
{
    const uint16_t observed[2] = { 0xB0FFu, 0x557Fu };
    size_t i;
    for (i = 0; i < 2; ++i) {
        assert((observed[i] & 1u) == 1u);          /* odd: cannot be lever */
        assert((observed[i] % 64u) == 63u);        /* and carries 0x7FFF's residue */
    }

    /* At sensitivity 2, the scale is 64, so no lever value is ever 63 mod 64
     * while every adcs value derived from one is. This is exactly the case the
     * cabinet readings come from. */
    cfg.sensitivity = 2;
    assert((32 * cfg.sensitivity) % 64 == 0);
    for (unsigned raw = 0; raw <= 0xFFFFu; raw += 7) {
        int lever = mu3_lever_convert((uint16_t)raw, &cfg);
        uint16_t adcs = (uint16_t)((0x7FFF - lever) & 0xFFFF);
        assert(((uint16_t)lever % 64u) != 63u);
        assert((adcs % 64u) == 63u);
    }

    /* The general relation, valid at every sensitivity: adcs is the lever
     * value complemented against 0x7FFF, so it is odd whenever lever is even. */
    for (int sensitivity = MU3_LEVER_SENSITIVITY_MIN;
         sensitivity <= MU3_LEVER_SENSITIVITY_MAX; ++sensitivity) {
        cfg.sensitivity = sensitivity;
        for (unsigned raw = 0; raw <= 0xFFFFu; raw += 7) {
            int lever = mu3_lever_convert((uint16_t)raw, &cfg);
            uint16_t adcs = (uint16_t)((0x7FFF - lever) & 0xFFFF);
            assert((lever & 1) == 0);           /* lever is always even */
            assert((adcs & 1u) == 1u);          /* adcs is therefore odd */
            assert((uint16_t)adcs == (uint16_t)((0x7FFF - lever) & 0xFFFF));
        }
    }
    cfg.sensitivity = MU3_LEVER_SENSITIVITY_DEFAULT;
}

static void write_file(const wchar_t *path, const char *body)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    assert(file != INVALID_HANDLE_VALUE);
    assert(WriteFile(file, body, (DWORD)strlen(body), &written, NULL));
    assert(written == strlen(body));
    assert(CloseHandle(file) != 0);
}

static void test_ini_overrides(void)
{
    /* The test process runs from build/, so use a fixed local name instead of
     * %TEMP%: sandboxed runs may deny writes there. */
    const wchar_t *path = L".\\mu3_lever_test.ini";
    mu3_lever_config_defaults(&cfg);

    write_file(path, "lever_neutral=1194\n"
                     "lever_sensitivity=3\n"
                     "garbage_key=not_a_number\n"
                     "lever_sensitivity_garbage=99\n");
    mu3_lever_config_load(&cfg, path);
    assert(cfg.neutral == 1194);
    assert(cfg.sensitivity == 3);

    /* Out-of-range values are ignored, keeping whatever was there. */
    write_file(path, "lever_neutral=99999\nlever_sensitivity=42\n");
    mu3_lever_config_load(&cfg, path);
    assert(cfg.neutral == 1194);
    assert(cfg.sensitivity == 3);

    /* A file with none of our keys changes nothing. */
    write_file(path, "unrelated=1\n");
    mu3_lever_config_load(&cfg, path);
    assert(cfg.neutral == 1194 && cfg.sensitivity == 3);

    /* A missing file keeps the current config (and must not corrupt it). */
    assert(DeleteFileW(path) != 0);
    mu3_lever_config_load(&cfg, path);
    assert(cfg.neutral == 1194 && cfg.sensitivity == 3);

    mu3_lever_config_defaults(&cfg);
}

int main(void)
{
    test_defaults();
    test_capture_values();
    test_centre_is_zero_at_any_sensitivity();
    test_no_regression_at_even_sensitivity();
    test_odd_sensitivity_is_monotonic();
    
    test_calibration_readings_are_not_lever_values();
    test_ini_overrides();
    puts("lever conversion tests passed");
    return 0;
}

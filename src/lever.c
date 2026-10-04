#include "lever.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void mu3_lever_config_defaults(mu3_lever_config *cfg)
{
    if (cfg == NULL) return;
    cfg->neutral = MU3_LEVER_NEUTRAL_DEFAULT;
    cfg->sensitivity = MU3_LEVER_SENSITIVITY_DEFAULT;
}

bool mu3_lever_config_valid(const mu3_lever_config *cfg)
{
    if (cfg == NULL) return false;
    if (cfg->neutral < 0 || cfg->neutral > 0xFFFF) return false;
    if (cfg->sensitivity < MU3_LEVER_SENSITIVITY_MIN ||
        cfg->sensitivity > MU3_LEVER_SENSITIVITY_MAX) return false;
    return true;
}

int16_t mu3_lever_convert(uint16_t raw, const mu3_lever_config *cfg)
{
    mu3_lever_config fallback;
    int32_t centred;
    int64_t scaled;
    uint16_t bits;

    if (!mu3_lever_config_valid(cfg)) {
        mu3_lever_config_defaults(&fallback);
        cfg = &fallback;
    }
    /* Do the subtraction in signed space so a raw reading below the centre
     * gives the negative excursion the game expects (left is negative). */
    centred = (int32_t)raw - (int32_t)cfg->neutral;
    scaled = (int64_t)centred * 32 * (int64_t)cfg->sensitivity;
    /* Reproduce the original's (short) narrowing: keep the low 16 bits. */
    bits = (uint16_t)((uint64_t)scaled & 0xFFFFu);
    return bits < 0x8000u ? (int16_t)bits : (int16_t)((int32_t)bits - 65536);
}

void mu3_lever_config_load(mu3_lever_config *cfg, const wchar_t *path)
{
    /* Read as raw bytes and scan in narrow characters: the file is a plain
     * text file that an operator edits in Notepad, which saves ANSI or UTF-8
     * without a BOM, not UTF-16. */
    char buffer[512];
    DWORD read = 0;
    HANDLE file;
    char *at;
    static const char *keys[] = { "lever_neutral", "lever_sensitivity" };
    size_t i;

    if (cfg == NULL || path == NULL || path[0] == L'\0') return;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(file, buffer, sizeof(buffer) - 1, &read, NULL)) {
        CloseHandle(file);
        return;
    }
    CloseHandle(file);
    buffer[read] = '\0';

    /* Deliberately minimal: a tiny "key=value" scan is far less error-prone
     * here than pulling in the profile API, and it cannot fail the load. */
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const char *found = strstr(buffer, keys[i]);
        long value;
        if (!found) continue;
        found += strlen(keys[i]);
        while (*found == ' ' || *found == '\t' || *found == '=') ++found;
        value = strtol(found, &at, 10);
        if (at == found) continue; /* no digits: keep the current value */
        if (i == 0) {
            if (value >= 0 && value <= 0xFFFF) cfg->neutral = (int)value;
        } else if (value >= MU3_LEVER_SENSITIVITY_MIN &&
                   value <= MU3_LEVER_SENSITIVITY_MAX) {
            cfg->sensitivity = (int)value;
        }
    }
}

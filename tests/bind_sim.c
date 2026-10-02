/* Mirror segatools' dll_bind() for this exact reference tree so a missing or
 * renamed export fails here instead of on the cabinet. The order and the
 * per-version counts below are copied from:
 *   games/mu3hook/mu3-dll.c   (mu3_dll_syms, has_enough_symbols)
 *   common/board/aime-dll.c   (aime_dll_syms, AIME_DLL_SYM_COUNT_V100/V101)
 * dll_bind is all-or-nothing and stops at the first absent name. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static const char *mu3_syms[] = {
    "mu3_io_init", "mu3_io_poll", "mu3_io_get_opbtns", "mu3_io_get_gamebtns",
    "mu3_io_get_lever", "mu3_io_led_init", "mu3_io_led_set_colors"
};
/* First 5 are the 0x0100 set; all 17 are required at 0x0101. */
static const char *aime_syms[] = {
    "aime_io_init", "aime_io_nfc_poll", "aime_io_nfc_get_aime_id",
    "aime_io_nfc_get_felica_id", "aime_io_led_set_color",
    "aime_io_vfd_set_text", "aime_io_vfd_set_state", "aime_io_nfc_get_mifare_uid",
    "aime_io_nfc_mifare_select", "aime_io_nfc_mifare_set_key",
    "aime_io_nfc_mifare_authenticate", "aime_io_nfc_mifare_read_block",
    "aime_io_nfc_felica_transact", "aime_io_nfc_radio_on",
    "aime_io_nfc_radio_off", "aime_io_nfc_to_update_mode",
    "aime_io_nfc_send_hex_data"
};
typedef uint16_t (__cdecl *version_fn)(void);

static int bind_syms(const char *label, HMODULE dll, const char *const *syms,
                     size_t count, size_t required)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (GetProcAddress(dll, syms[i]) == NULL) {
            printf("%s: %zu/%zu INCOMPLETE, first missing: %s\n",
                   label, i, required, syms[i]);
            return 1;
        }
        if (i + 1 == required) printf("%s: %zu/%zu COMPLETE\n", label, required, required);
    }
    return 0;
}

int main(int argc, char **argv)
{
    HMODULE dll;
    version_fn mu3_ver, aime_ver;
    int failures = 0;
    if (argc != 2) { fprintf(stderr, "usage: bind_sim <path to MU3CustomIO.dll>\n"); return 2; }
    dll = LoadLibraryA(argv[1]);
    if (!dll) {
        fprintf(stderr, "LoadLibraryA failed: %#lx (0x800700c1 = wrong architecture)\n",
                GetLastError());
        return 2;
    }
    mu3_ver = (version_fn)(void *) GetProcAddress(dll, "mu3_io_get_api_version");
    aime_ver = (version_fn)(void *) GetProcAddress(dll, "aime_io_get_api_version");
    printf("mu3_io_get_api_version  = %#06x\n", mu3_ver ? mu3_ver() : 0x0100);
    printf("aime_io_get_api_version = %#06x\n", aime_ver ? aime_ver() : 0x0100);
    if (mu3_ver && mu3_ver() >= 0x0200) { puts("mu3 version rejected by segatools"); failures++; }
    if (aime_ver && aime_ver() >= 0x0200) { puts("aime version rejected by segatools"); failures++; }
    failures += bind_syms("mu3", dll, mu3_syms, 7, 7);
    failures += bind_syms("aime", dll, aime_syms, 17,
                          (aime_ver && aime_ver() < 0x0101) ? 5 : 17);
    FreeLibrary(dll);
    if (failures) { puts("bind simulation FAILED"); return 1; }
    puts("bind simulation passed: segatools would bind all required symbols");
    return 0;
}

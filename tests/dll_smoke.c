#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *symbols[] = {
    "mu3_io_get_api_version", "mu3_io_init", "mu3_io_poll",
    "mu3_io_get_opbtns", "mu3_io_get_gamebtns", "mu3_io_get_lever",
    "mu3_io_led_init", "mu3_io_led_set_colors",
    "aime_io_get_api_version", "aime_io_init", "aime_io_nfc_poll",
    "aime_io_nfc_get_aime_id", "aime_io_nfc_get_felica_id",
    "aime_io_led_set_color", "aime_io_vfd_set_text", "aime_io_vfd_set_state",
    "aime_io_nfc_get_mifare_uid", "aime_io_nfc_mifare_select",
    "aime_io_nfc_mifare_set_key", "aime_io_nfc_mifare_authenticate",
    "aime_io_nfc_mifare_read_block", "aime_io_nfc_felica_transact",
    "aime_io_nfc_radio_on", "aime_io_nfc_radio_off",
    "aime_io_nfc_to_update_mode", "aime_io_nfc_send_hex_data"
};
typedef uint16_t (__cdecl *version_fn)(void);
typedef HRESULT (__cdecl *poll_fn)(void);
typedef void (__cdecl *buttons_fn)(uint8_t *, uint8_t *);
typedef void (__cdecl *lever_fn)(int16_t *);
typedef HRESULT (__cdecl *card_fn)(uint8_t, uint8_t *, size_t);
int main(int argc, char **argv)
{
    HMODULE dll;
    size_t i;
    uint8_t left = 255, right = 255, card[10] = {0};
    int16_t lever = 123;
    HRESULT hr;
    (void)argc; (void)argv;
    dll = LoadLibraryW(L"MU3CustomIO.dll");
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        if (!GetProcAddress(dll, symbols[i])) {
            fprintf(stderr, "Missing export: %s\n", symbols[i]); return 2;
        }
    }
    if (((version_fn)GetProcAddress(dll, "mu3_io_get_api_version"))() != 0x0101 ||
        ((version_fn)GetProcAddress(dll, "aime_io_get_api_version"))() != 0x0101) return 3;
    if (((poll_fn)GetProcAddress(dll, "mu3_io_init"))() != S_OK) return 4;
    ((poll_fn)GetProcAddress(dll, "mu3_io_poll"))();
    ((buttons_fn)GetProcAddress(dll, "mu3_io_get_gamebtns"))(&left, &right);
    ((lever_fn)GetProcAddress(dll, "mu3_io_get_lever"))(&lever);
    if (left || right || lever) return 5;
    if (((card_fn)GetProcAddress(dll, "aime_io_nfc_get_aime_id"))(0, card, sizeof(card)) != S_FALSE) return 6;
    Sleep(3200);
    /* Offline must stay S_OK: segatools treats a failure from init/poll as
     * fatal (mu3hook/dllmain.c -> ExitProcess), and the frozen DLL always
     * returned S_OK here. A dead controller shows up as neutral input. */
    hr = ((poll_fn)GetProcAddress(dll, "mu3_io_poll"))();
    if (hr != S_OK) { fprintf(stderr, "Offline poll must be S_OK, got %#lx\n", hr); return 7; }
    left = 255; right = 255; lever = 123;
    ((buttons_fn)GetProcAddress(dll, "mu3_io_get_gamebtns"))(&left, &right);
    ((lever_fn)GetProcAddress(dll, "mu3_io_get_lever"))(&lever);
    if (left || right || lever) { fprintf(stderr, "Offline input must be neutral\n"); return 8; }
    if (((poll_fn)GetProcAddress(dll, "mu3_io_init"))() != S_OK) { fprintf(stderr, "Offline init must be S_OK\n"); return 9; }
    printf("DLL smoke passed: %zu exports, offline init/poll S_OK with neutral input\n",
           sizeof(symbols) / sizeof(symbols[0]));
    FreeLibrary(dll);
    return 0;
}

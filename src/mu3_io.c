#include "io_core.h"
#include "hid_device.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "led_packet.h"
#include "card_id.h"
#include "lever.h"

/* MU3 1.1 (7 symbols) + Aime 1.1 (17 symbols). C / __cdecl exports, .def
 * provides undecorated names on x86. No device operation runs in DllMain. */
static mu3_core core;
static mu3_hid_device hid;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static SRWLOCK poll_lock = SRWLOCK_INIT;
static mu3_sample polled;
static volatile LONG started = 0;
static ULONGLONG init_ms;
#ifndef MU3_DISCONNECT_REPORT_MS
#define MU3_DISCONNECT_REPORT_MS 3000u
#endif
static volatile LONG owner = 0;
static mu3_lever_config lever_cfg;
typedef struct shared_frame {
    volatile LONG sequence;
    DWORD owner_pid;
    uint64_t published_ms; /* Owner's worker liveness, refreshed even when idle. */
    mu3_sample sample;
    volatile LONG led_sequence;
    uint8_t led_report[65];
} shared_frame;
/* A live owner refreshes published_ms on every worker tick (<=250 ms). This is
 * deliberately independent of report age: a change-triggered controller sends
 * nothing while idle, and that must not look like a dead owner. */
#define MU3_OWNER_LIVENESS_MS 2000u
static LONG last_led_sequence;
static SRWLOCK led_lock = SRWLOCK_INIT;
static SRWLOCK shared_lock = SRWLOCK_INIT;
static HANDLE mapping;
static shared_frame *shared;
static bool shared_read(mu3_sample *sample)
{
    LONG before, after;
    DWORD pid;
    uint64_t published;
    uint64_t now;
    HANDLE process;
    unsigned attempt;
    if (!shared || shared->owner_pid == GetCurrentProcessId()) return false;
    now = GetTickCount64();
    for (attempt = 0; attempt < 3; ++attempt) {
        before = InterlockedCompareExchange(&shared->sequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        pid = shared->owner_pid;
        published = shared->published_ms;
        *sample = shared->sample;
        MemoryBarrier();
        after = InterlockedCompareExchange(&shared->sequence, 0, 0);
        /* Gate on owner liveness, never on report age. */
        if (before != after || (after & 1) || !pid || !published ||
            now < published || now - published > MU3_OWNER_LIVENESS_MS) continue;
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process) return false;
        {
            DWORD alive = WaitForSingleObject(process, 0);
            CloseHandle(process);
            return alive == WAIT_TIMEOUT;
        }
    }
    return false;
}
static void shared_write(const mu3_sample *sample)
{
    if (!shared) return;
    /* Odd sequence = write in flight. The HID worker thread and the tick
     * heartbeat both publish and must not interleave, so hold a lock rather
     * than relying on "single writer". */
    AcquireSRWLockExclusive(&shared_lock);
    InterlockedIncrement(&shared->sequence);
    shared->owner_pid = GetCurrentProcessId();
    shared->published_ms = GetTickCount64();
    shared->sample = *sample;
    InterlockedIncrement(&shared->sequence);
    ReleaseSRWLockExclusive(&shared_lock);
}
static void on_frame(void *ctx, const uint8_t *report, size_t size, uint64_t now);
static void on_state(void *ctx, bool connected);
static void publish_health(void *ctx);

static BOOL CALLBACK initialize_once(PINIT_ONCE unused, PVOID param, PVOID *context)
{
    wchar_t path[MAX_PATH];
    const wchar_t *filename;
    (void) unused; (void) param; (void) context;
    mu3_core_init(&core, -1); /* Report ID unverified until a descriptor is captured. */
    mu3_lever_config_defaults(&lever_cfg);
    init_ms = GetTickCount64();
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 sizeof(shared_frame), L"Local\\MU3CustomIO-v1");
    if (mapping) shared = (shared_frame *)MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                                        0, 0, sizeof(shared_frame));
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return TRUE;
    filename = wcsrchr(path, L'\\');
    filename = filename ? filename + 1 : path;
    /* Optional overrides live next to the DLL as MU3CustomIO.ini, not next to
     * the exe: both mu3.exe and amdaemon.exe load this one DLL, so the DLL's
     * own directory is the one place both processes agree on. A missing or
     * partial file simply keeps the defaults, so this can never fail a load. */
    {
        HMODULE self = NULL;
        wchar_t dll_path[MAX_PATH];
        wchar_t *dot;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(const void *)&initialize_once, &self) &&
            GetModuleFileNameW(self, dll_path, MAX_PATH) != 0) {
            dot = wcsrchr(dll_path, L'.');
            if (dot && (size_t)(dot - dll_path) + 5 <= MAX_PATH) {
                if (wcscpy_s(dot, 5, L".ini") == 0) {
                    mu3_lever_config_load(&lever_cfg, dll_path);
                }
            }
        }
    }
    /* segatools also loads this DLL in the game process. One process must
     * own the controller; the game process must never open a second handle. */
    if (_wcsicmp(filename, L"amdaemon.exe") == 0) {
        InterlockedExchange(&owner, 1);
        if (mu3_hid_start(&hid, &core, on_frame, on_state, publish_health)) {
            InterlockedExchange(&started, 1);
        }
    }
    return TRUE;
}

static void ensure_init(void) { InitOnceExecuteOnce(&once, initialize_once, NULL, NULL); }
static void publish_health(void *ctx)
{
    if (shared && started) {
        uint8_t leds[65];
        LONG before = InterlockedCompareExchange(&shared->led_sequence, 0, 0);
        if (!(before & 1) && before != last_led_sequence) {
            MemoryBarrier();
            memcpy(leds, shared->led_report, sizeof(leds));
            MemoryBarrier();
            if (InterlockedCompareExchange(&shared->led_sequence, 0, 0) == before) {
                last_led_sequence = before;
                mu3_hid_queue(&hid, leds);
            }
        }
    }
    mu3_sample sample;
    mu3_health health;
    /* Always publish: this is the owner's liveness heartbeat. A snapshot that
     * is not fresh zeroes the sample, so a dead owner and a merely quiet
     * controller stay distinguishable. Publishing only when fresh would stop
     * the heartbeat as soon as the controller went idle. */
    mu3_core_snapshot((mu3_core *)ctx, &sample, &health);
    shared_write(&sample);
}
static void on_frame(void *ctx, const uint8_t *report, size_t size, uint64_t now)
{
    mu3_sample sample;
    mu3_health health;
    mu3_core *state = (mu3_core *)ctx;
    if (mu3_core_publish(state, report, size, now) &&
        mu3_core_snapshot(state, &sample, &health)) {
        shared_write(&sample);
    }
}
static void on_state(void *ctx, bool connected)
{
    if (connected) mu3_core_open((mu3_core *)ctx);
    else { mu3_core_disconnect((mu3_core *)ctx); publish_health(ctx); }
}

uint16_t mu3_io_get_api_version(void) { return 0x0101; }
/* Init must succeed even with no controller attached. segatools calls this
 * from mu3_io4_hook_init (games/mu3hook/io4.c:35); dllmain.c:112-116 treats a
 * failure as fatal and calls ExitProcess, so failing here aborts the whole
 * game/hook start-up instead of just reporting a dead controller.
 * The frozen baseline also always succeeded here. */
HRESULT mu3_io_init(void)
{
    ensure_init();
    return S_OK;
}
HRESULT mu3_io_poll(void)
{
    mu3_sample next = {0};
    mu3_health health;
    bool fresh;
    ensure_init();
    fresh = owner ? mu3_core_snapshot(&core, &next, &health)
                  : shared_read(&next);
    if (!fresh) memset(&next, 0, sizeof(next)); /* Neutral, never stale input. */
    AcquireSRWLockExclusive(&poll_lock);
    polled = next;
    ReleaseSRWLockExclusive(&poll_lock);
    /* Poll must also stay S_OK. A failure here propagates out of
     * io4_async_poll (common/board/io4.c:349) into the virtual IO4 read and
     * aborts start-up the same way. Stale/absent input is reported as neutral
     * state instead, matching the frozen DLL's always-S_OK behaviour.
     * MU3_IO_REPORT_DISCONNECT (build with /DMU3_IO_REPORT_DISCONNECT) opts
     * into returning HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) after
     * MU3_DISCONNECT_REPORT_MS, for experiments that need a visible fault.
     * Whether the game shows an IO4 error for it is still unverified. */
#ifdef MU3_IO_REPORT_DISCONNECT
    return !fresh && GetTickCount64() - init_ms > MU3_DISCONNECT_REPORT_MS
        ? HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) : S_OK;
#else
    (void)init_ms;
    return S_OK;
#endif
}
void mu3_io_get_opbtns(uint8_t *buttons)
{
    if (!buttons) return;
    AcquireSRWLockShared(&poll_lock);
    *buttons = polled.operator_buttons & 7;
    ReleaseSRWLockShared(&poll_lock);
}
void mu3_io_get_gamebtns(uint8_t *left, uint8_t *right)
{
    AcquireSRWLockShared(&poll_lock);
    if (left) *left = polled.left;
    if (right) *right = polled.right;
    ReleaseSRWLockShared(&poll_lock);
}
void mu3_io_get_lever(int16_t *lever)
{
    if (!lever) return;
    AcquireSRWLockShared(&poll_lock);
    *lever = mu3_lever_convert(polled.raw_lever, &lever_cfg);
    ReleaseSRWLockShared(&poll_lock);
}
HRESULT mu3_io_led_init(void) { ensure_init(); return S_OK; }
void mu3_io_led_set_colors(uint8_t board, uint8_t *rgb)
{
    uint8_t report[65] = {0};
    ensure_init();
    if (board != 1 || rgb == NULL || (owner && !started)) return;
    mu3_led_packet(rgb, report);
    if (owner) mu3_hid_queue(&hid, report);
    else if (shared && shared->owner_pid != GetCurrentProcessId()) {
        /* Multiple callback threads in this process serialize their writes. */
        AcquireSRWLockExclusive(&led_lock);
        InterlockedIncrement(&shared->led_sequence);
        memcpy(shared->led_report, report, sizeof(report));
        InterlockedIncrement(&shared->led_sequence);
        ReleaseSRWLockExclusive(&led_lock);
    }
}
uint16_t aime_io_get_api_version(void) { return 0x0101; }
HRESULT aime_io_init(void) { ensure_init(); return S_OK; }
HRESULT aime_io_nfc_poll(uint8_t unit) { (void)unit; ensure_init(); return S_OK; }
HRESULT aime_io_nfc_get_aime_id(uint8_t unit, uint8_t *id, size_t size)
{
    mu3_sample sample;
    mu3_health health;
    (void)unit;
    ensure_init();
    if (!id || size < MU3_CARD_SIZE) return E_INVALIDARG;
    if (!(owner ? mu3_core_snapshot(&core, &sample, &health)
                 : shared_read(&sample))) return S_FALSE;
    if (sample.scan == 1) {
        memcpy(id, sample.card, MU3_CARD_SIZE);
        return S_OK;
    }
    if (sample.scan == 2) {
        mu3_felica_to_aime_bcd(sample.card, id);
        return S_OK;
    }
    return S_FALSE;
}
HRESULT aime_io_nfc_get_felica_id(uint8_t unit, uint64_t *idm)
{
    (void)unit;
    if (idm) *idm = 0;
    /* Scan=2 encoding is unverified. Do not fabricate IDm/card identity. */
    return S_FALSE;
}
void aime_io_led_set_color(uint8_t unit, uint8_t r, uint8_t g, uint8_t b)
{
    (void)unit; (void)r; (void)g; (void)b; /* No verified reader LED command. */
}
/* Unsupported reader features are advertised as absent, never fabricated. */
HRESULT aime_io_nfc_get_mifare_uid(uint8_t unit, uint8_t *uid, size_t size)
{ (void)unit; (void)uid; (void)size; return S_FALSE; }
HRESULT aime_io_nfc_mifare_select(uint8_t unit, const uint8_t *uid, size_t size)
{ (void)unit; (void)uid; (void)size; return S_FALSE; }
HRESULT aime_io_nfc_mifare_set_key(uint8_t unit, uint8_t type,
                                   const uint8_t *key, size_t size)
{ (void)unit; (void)type; (void)key; (void)size; return S_FALSE; }
HRESULT aime_io_nfc_mifare_authenticate(uint8_t unit, uint8_t type,
                                        const uint8_t *payload, size_t size)
{ (void)unit; (void)type; (void)payload; (void)size; return S_FALSE; }
HRESULT aime_io_nfc_mifare_read_block(uint8_t unit, const uint8_t *uid,
                                      size_t uid_size, uint8_t block_no,
                                      uint8_t *block, size_t block_size)
{ (void)unit; (void)uid; (void)uid_size; (void)block_no; (void)block; (void)block_size; return S_FALSE; }
HRESULT aime_io_nfc_felica_transact(uint8_t unit, const uint8_t *req,
                                    size_t req_size, uint8_t *res,
                                    size_t res_size, size_t *written)
{ (void)unit; (void)req; (void)req_size; (void)res; (void)res_size;
  if (written) *written = 0; return S_FALSE; }
HRESULT aime_io_nfc_radio_on(uint8_t unit) { (void)unit; return S_FALSE; }
HRESULT aime_io_nfc_radio_off(uint8_t unit) { (void)unit; return S_FALSE; }
HRESULT aime_io_nfc_to_update_mode(uint8_t unit) { (void)unit; return S_FALSE; }
HRESULT aime_io_nfc_send_hex_data(uint8_t unit, const uint8_t *payload,
                                   size_t size, uint8_t *status)
{ (void)unit; (void)payload; (void)size; (void)status; return S_FALSE; }
struct aime_io_vfd_state;
void aime_io_vfd_set_text(const uint8_t *text, size_t length,
                           const struct aime_io_vfd_state *state)
{ (void)text; (void)length; (void)state; }
void aime_io_vfd_set_state(const struct aime_io_vfd_state *state)
{ (void)state; }

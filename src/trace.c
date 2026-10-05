#include "trace.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* One channel per process: the owner logs the wire, the game process logs what
 * it served. Neither process ever opens the other's file. */
static HANDLE trace_file = INVALID_HANDLE_VALUE;
static mu3_chord_census trace_census;
/* Which channel this process opened. Every write is checked against it, so a
 * call from the wrong channel is dropped rather than mixed into the file. */
static mu3_trace_channel trace_channel;

/* GetTickCount64 resolves to the scheduler tick (about 15.6 ms), which is far
 * too coarse to measure a switch bouncing: the gap being looked for is single
 * milliseconds. QPC is sub-microsecond, and its value is comparable between
 * processes on one machine, so the two channels' timestamps can be lined up. */
static uint64_t qpc_frequency;

static uint64_t now_us(void)
{
    LARGE_INTEGER counter;
    if (qpc_frequency == 0) {
        LARGE_INTEGER freq;
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0;
        qpc_frequency = (uint64_t) freq.QuadPart;
    }
    if (!QueryPerformanceCounter(&counter)) return 0;
    /* Split the division so the multiplication cannot overflow on a long
     * uptime: microseconds = whole seconds * 1e6 + remainder scaled. */
    return ((uint64_t) counter.QuadPart / qpc_frequency) * 1000000u +
           (((uint64_t) counter.QuadPart % qpc_frequency) * 1000000u) /
               qpc_frequency;
}

uint64_t mu3_trace_now_us(void) { return now_us(); }

/* Bounded append. Truncation is reported rather than silently producing a line
 * the analyzer would misread. */
typedef struct out_buf {
    char *buf;
    size_t size;
    size_t used;
    bool overflow;
} out_buf;

static void out_init(out_buf *o, char *buf, size_t size)
{
    o->buf = buf;
    o->size = size;
    o->used = 0;
    o->overflow = false;
    if (size > 0) buf[0] = '\0';
}

static void out_text(out_buf *o, const char *text)
{
    size_t len = strlen(text);
    if (o->buf == NULL || o->used + len + 1 > o->size) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->used, text, len);
    o->used += len;
    o->buf[o->used] = '\0';
}

static void out_u64(out_buf *o, uint64_t value)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long) value);
    out_text(o, tmp);
}

static void out_uint(out_buf *o, unsigned value)
{
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u", value);
    out_text(o, tmp);
}

static void out_keys(out_buf *o, uint16_t mask)
{
    unsigned k;
    bool any = false;
    if (mask == 0) {
        out_text(o, "-");
        return;
    }
    for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
        char name[8];
        if (!(mask & (1u << k))) continue;
        mu3_key_name(k, name, sizeof(name));
        if (any) out_text(o, ",");
        out_text(o, name);
        any = true;
    }
}

size_t mu3_trace_format(char *buf, size_t size, uint64_t us,
                        const mu3_chord_step *step,
                        const uint8_t buttons[MU3_BUTTON_COUNT])
{
    out_buf o;
    unsigned k;
    if (buf == NULL || size == 0 || step == NULL || buttons == NULL) return 0;
    out_init(&o, buf, size);
    out_text(&o, "t=");
    out_u64(&o, us);
    out_text(&o, "us held=");
    out_uint(&o, step->held_before);
    out_text(&o, "->");
    out_uint(&o, step->held_after);
    out_text(&o, " down=");
    out_keys(&o, step->down);
    out_text(&o, " up=");
    out_keys(&o, step->up);
    out_text(&o, " bytes=");
    for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
        char tmp[8];
        if (k) out_text(&o, " ");
        snprintf(tmp, sizeof(tmp), "%02X", buttons[k]);
        out_text(&o, tmp);
    }
    /* Bytes moved but the decoded mask did not: would falsify "nonzero means
     * pressed", and is otherwise invisible in the down/up columns. */
    if (!step->mask_changed) out_text(&o, " !mask");
    out_text(&o, "\n");
    if (o.overflow) {
        if (size > 0) buf[0] = '\0';
        return 0;
    }
    return o.used;
}

bool mu3_trace_active(void)
{
    return trace_file != INVALID_HANDLE_VALUE;
}

static void write_line(const char *line, size_t length)
{
    DWORD written = 0;
    if (trace_file == INVALID_HANDLE_VALUE || line == NULL || length == 0) return;
    /* Unbuffered: the process is usually killed rather than unloaded, so a
     * buffered tail would be lost exactly when it matters. Lines are only
     * written on a button change, so this is a handful of syscalls per second. */
    (void) WriteFile(trace_file, line, (DWORD) length, &written, NULL);
}

/* Derive the channel's log path from the INI path by replacing its extension,
 * so both files land beside the DLL where the operator already looks. */
static bool log_path(wchar_t *out, size_t size, const wchar_t *ini,
                     const wchar_t *suffix)
{
    const wchar_t *dot;
    size_t base;
    if (out == NULL || ini == NULL || suffix == NULL || size == 0) return false;
    dot = wcsrchr(ini, L'.');
    base = dot ? (size_t)(dot - ini) : wcslen(ini);
    if (base + wcslen(suffix) + 1 > size) return false;
    wmemcpy(out, ini, base);
    return wcscpy_s(out + base, size - base, suffix) == 0;
}

void mu3_trace_open(const wchar_t *ini_path, mu3_trace_channel channel)
{
    wchar_t path[MAX_PATH];
    char header[256];
    const wchar_t *suffix =
        channel == MU3_TRACE_WIRE ? L"-wire.log" : L"-served.log";
    const char *label = channel == MU3_TRACE_WIRE ? "wire" : "served";
    int length;

    if (ini_path == NULL || ini_path[0] == L'\0') return;
    /* Off unless explicitly asked for. Read the flag from the file rather than
     * from an environment variable so it travels with the DLL, and so enabling
     * it cannot be done by accident from a launcher. */
    if (GetPrivateProfileIntW(L"trace", L"enabled", 0, ini_path) != 1) return;
    if (!log_path(path, MAX_PATH, ini_path, suffix)) return;
    /* CREATE_ALWAYS: one file per run, so a session is never mixed with the
     * previous one and the timestamps stay unambiguous. */
    trace_file = CreateFileW(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (trace_file == INVALID_HANDLE_VALUE) return;
    trace_channel = channel;
    /* A new file is a new session: the previous census must not carry into it,
     * or the first change of this session would be compared against a state
     * that belongs to the previous file and the baseline line would be lost. */
    memset(&trace_census, 0, sizeof(trace_census));

    qpc_frequency = 0;
    length = snprintf(header, sizeof(header),
                      "# channel=%s pid=%lu qpc_us=%llu\n"
                      "# columns: t=<us> held=<n>-><n> down=<keys> up=<keys> "
                      "bytes=<10 hex>\n"
                      "# a key that goes down, up and down again within a few "
                      "ms while otherwise held is a bouncing contact\n",
                      label, (unsigned long) GetCurrentProcessId(),
                      (unsigned long long) now_us());
    if (length > 0 && (size_t) length < sizeof(header)) {
        write_line(header, (size_t) length);
    }
}

void mu3_trace_close(void)
{
    if (trace_file != INVALID_HANDLE_VALUE) {
        CloseHandle(trace_file);
        trace_file = INVALID_HANDLE_VALUE;
    }
}

void mu3_trace_wire_bytes(uint64_t us, const uint8_t buttons[MU3_BUTTON_COUNT])
{
    mu3_chord_step step;
    char line[256];
    size_t length;
    bool baseline;
    /* A served-channel process writing here would replace the device's real
     * bytes with reconstructed 00/01 and corrupt the one file the hardware
     * verdict is read from. Refuse rather than write something plausible. */
    if (!mu3_trace_active() || trace_channel != MU3_TRACE_WIRE) return;
    if (buttons == NULL) return;
    /* The first report is a baseline rather than a change: a key already held
     * when tracing began is not a press. It is still written, because every
     * line carries the full byte vector and the analyzer needs one to compare
     * against. Without it the session's first real transition would have no
     * predecessor and would be silently dropped. */
    baseline = !trace_census.started;
    if (!mu3_chord_update(&trace_census, buttons, &step) && !baseline) return;
    /* Only a change after the baseline produces a line: the device streams at
     * about 200 reports/s, and most of those say nothing new. */
    length = mu3_trace_format(line, sizeof(line), us, &step, buttons);
    write_line(line, length);
}

void mu3_trace_served_masks(uint64_t us, uint8_t left, uint8_t right)
{
    uint8_t buttons[MU3_BUTTON_COUNT];
    mu3_chord_step step;
    char line[256];
    size_t length;
    bool baseline;
    unsigned i;
    if (!mu3_trace_active() || trace_channel != MU3_TRACE_SERVED) return;
    /* The served channel only carries the decoded masks, so the byte columns are
     * reconstructed as 00/01. Both channels then share one format and one
     * analyzer; the wire file remains the only place the real byte values are
     * visible, and that is the file that settles a polarity question. */
    memset(buttons, 0, sizeof(buttons));
    for (i = 0; i < MU3_BUTTONS_PER_SIDE; ++i) {
        if (left & (1u << i)) buttons[i] = 1;
        if (right & (1u << i)) buttons[i + MU3_BUTTONS_PER_SIDE] = 1;
    }
    /* Same baseline rule as the wire channel: the first line has to be written
     * even though it carries no transition, or the analyzer would have no
     * predecessor for the session's first change on this channel. */
    baseline = !trace_census.started;
    if (!mu3_chord_update(&trace_census, buttons, &step) && !baseline) return;
    length = mu3_trace_format(line, sizeof(line), us, &step, buttons);
    write_line(line, length);
}

/* Tests for the DLL's opt-in button trace.
 *
 * Two things are checked, and both matter for the verdict the trace is read to
 * produce:
 *
 *   1. The line format is what tests/trace_analyze.c parses. A drifting format
 *      would make the analyzer report a clean key, which is worse than
 *      reporting nothing, so the exact shape is pinned here.
 *   2. A bouncing contact survives the trace intact. The whole point is to see
 *      down, up, down within a few milliseconds; if the trace coalesced or
 *      dropped those, the instrument would hide the very fault it exists to
 *      find.
 *
 * The trace is a pure formatter plus a census, so no device and no file are
 * needed for most of it; the file gate at the end does touch a temp file. */
#define _CRT_SECURE_NO_WARNINGS
#include "../src/button_map.h"
#include "../src/trace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Feed a sequence of button states through the same calls the DLL makes and
 * hand each line back. A line is produced on every change, and also for the
 * first report, which is the baseline the analyzer compares against. */
static size_t feed_line(mu3_chord_census *census, uint64_t us,
                        const uint8_t buttons[MU3_BUTTON_COUNT],
                        char *out, size_t size)
{
    mu3_chord_step step;
    bool baseline = !census->started;
    if (!mu3_chord_update(census, buttons, &step) && !baseline) return 0;
    return mu3_trace_format(out, size, us, &step, buttons);
}

static void test_format_shape(void)
{
    mu3_chord_census census;
    mu3_chord_step step;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};
    char line[256];
    size_t length;

    memset(&census, 0, sizeof(census));
    /* Baseline, then press L1 and R3 together. */
    mu3_chord_update(&census, buttons, &step);
    buttons[0] = 1;
    buttons[7] = 1;
    assert(mu3_chord_update(&census, buttons, &step));
    length = mu3_trace_format(line, sizeof(line), 123456, &step, buttons);
    assert(length > 0);
    /* The analyzer splits on these exact tokens and parses ten hex columns.
     * The byte list must start immediately after "bytes=" - no leading space -
     * because that is the format the analyzer's own tests generate; accepting
     * both here would have hidden a mismatch between writer and reader. */
    assert(strstr(line, "t=123456us") == line);
    assert(strstr(line, "held=0->2") != NULL);
    assert(strstr(line, "down=L1,R3") != NULL);
    assert(strstr(line, "up=-") != NULL);
    assert(strstr(line, "bytes=01 00 00 00 00 00 00 01 00 00") != NULL);
    assert(strstr(line, "bytes= 0") == NULL);
    assert(line[length - 1] == '\n');

    /* A release-only change prints up= and down=-. */
    buttons[0] = 0;
    buttons[7] = 0;
    assert(mu3_chord_update(&census, buttons, &step));
    length = mu3_trace_format(line, sizeof(line), 200000, &step, buttons);
    assert(length > 0);
    assert(strstr(line, "down=-") != NULL);
    assert(strstr(line, "up=L1,R3") != NULL);

    /* A byte that moves without changing the decoded mask is flagged, because
     * it is the one shape that falsifies "nonzero means pressed". */
    memset(buttons, 0, sizeof(buttons));
    buttons[0] = 0x01;
    assert(mu3_chord_update(&census, buttons, &step));
    buttons[0] = 0x02; /* still pressed */
    assert(mu3_chord_update(&census, buttons, &step));
    length = mu3_trace_format(line, sizeof(line), 300000, &step, buttons);
    assert(length > 0);
    assert(strstr(line, "!mask") != NULL);

    /* Degenerate arguments must not produce a partial line. */
    assert(mu3_trace_format(NULL, 16, 0, &step, buttons) == 0);
    assert(mu3_trace_format(line, 0, 0, &step, buttons) == 0);
    assert(mu3_trace_format(line, sizeof(line), 0, NULL, buttons) == 0);
    assert(mu3_trace_format(line, sizeof(line), 0, &step, NULL) == 0);
    /* Too small for the line: reported as 0 with nothing half-written. */
    line[0] = 'x';
    assert(mu3_trace_format(line, 8, 123456, &step, buttons) == 0);
    assert(line[0] == '\0');
}

/* The fault the trace exists to find, reproduced synthetically. A contact that
 * opens for 3 ms mid-hold must appear on its own line, and must be
 * distinguishable from a human release-and-repress. */
static void test_bounce_is_visible(void)
{
    mu3_chord_census census;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};
    char line[256];
    size_t length;
    unsigned lines = 0;

    memset(&census, 0, sizeof(census));

    /* Baseline: nothing held. Still written, so the analyzer has something to
     * compare the first change against. */
    length = feed_line(&census, 0, buttons, line, sizeof(line));
    assert(length > 0 && strstr(line, "down=-") != NULL &&
           strstr(line, "up=-") != NULL);
    ++lines;

    /* Press L1 and hold it. */
    buttons[0] = 1;
    length = feed_line(&census, 10000, buttons, line, sizeof(line));
    assert(length > 0 && strstr(line, "down=L1") != NULL);
    ++lines;

    /* The contact opens for 3 ms: a bouncing switch. */
    buttons[0] = 0;
    length = feed_line(&census, 13000, buttons, line, sizeof(line));
    assert(length > 0 && strstr(line, "up=L1") != NULL);
    ++lines;

    /* And closes again. Three lines for one intended press. */
    buttons[0] = 1;
    length = feed_line(&census, 16000, buttons, line, sizeof(line));
    assert(length > 0 && strstr(line, "down=L1") != NULL);
    ++lines;

    /* Nothing further changes, so nothing more is written. */
    assert(feed_line(&census, 20000, buttons, line, sizeof(line)) == 0);
    assert(feed_line(&census, 90000, buttons, line, sizeof(line)) == 0);

    /* Release for real. */
    buttons[0] = 0;
    length = feed_line(&census, 120000, buttons, line, sizeof(line));
    assert(length > 0 && strstr(line, "up=L1") != NULL);
    ++lines;

    /* One intended press produced five change lines: the baseline, then down,
     * up, down (the bounce) and the real up. The census must have counted two
     * presses and two releases, which is exactly what makes the bounce
     * measurable. */
    assert(lines == 5);
    assert(census.presses[0] == 2);
    assert(census.releases[0] == 2);
    assert(census.max_held == 1);
    /* The byte stayed binary throughout, so key 0 has seen only 00 and 01. */
    assert(census.value_count[0] == 2);
    assert(census.values_seen[0][0] == 0);
    assert(census.values_seen[0][1] == 1);
    assert(census.byte_changes_without_mask_change == 0);
}

/* A key that is already held when the trace starts must not be reported as a
 * press: the log opens mid-session, and a phantom press at t=0 would be read as
 * evidence. */
static void test_baseline_is_silent(void)
{
    mu3_chord_census census;
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};
    char line[256];

    memset(&census, 0, sizeof(census));
    buttons[0] = buttons[5] = 1;
    /* The baseline line is emitted, but it must carry no transition. */
    assert(feed_line(&census, 1000, buttons, line, sizeof(line)) > 0);
    assert(strstr(line, "down=-") != NULL && strstr(line, "up=-") != NULL);
    assert(census.presses[0] == 0 && census.presses[5] == 0);
    assert(census.raw_changes == 0);
    /* A second report with nothing changed writes nothing at all. */
    assert(feed_line(&census, 2000, buttons, line, sizeof(line)) == 0);
}

/* Key naming is shared with the DLL and the probe, and the analyzer parses it,
 * so a change here would silently break the verdict. */
static void test_key_names(void)
{
    char name[8];
    unsigned i;
    static const char *want[MU3_BUTTON_COUNT] = {
        "L1", "L2", "L3", "L4", "L5", "R1", "R2", "R3", "R4", "R5"
    };
    for (i = 0; i < MU3_BUTTON_COUNT; ++i) {
        mu3_key_name(i, name, sizeof(name));
        assert(strcmp(name, want[i]) == 0);
    }
    /* Out of range must not write past a caller's tiny buffer. */
    mu3_key_name(99, name, sizeof(name));
    assert(name[0] == '\0');
    mu3_key_name(0, NULL, 8);
    mu3_key_name(0, name, 0);
}

/* The file side: the gate, the path, and the channel guard.
 *
 * This is the part whose failure is worst, because it is silent. A trace that
 * refuses to enable produces no log and no error message, and the session spent
 * collecting it is simply lost. So the gate is tested in both directions, along
 * with the guard that stops one channel writing into the other's file. */
static size_t slurp(const char *path, char *out, size_t size)
{
    FILE *file;
    size_t got;
    out[0] = '\0';
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    got = fread(out, 1, size - 1, file);
    out[got] = '\0';
    fclose(file);
    return got;
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fputs(text, file);
    fclose(file);
}

/* Derive the log path the module will use: same directory as the INI, same
 * basename, with the channel's suffix. Mirrors trace.c's log_path so the test
 * reads the file the DLL actually writes. */
static void log_path_for(const char *ini, const char *suffix, char *out, size_t size)
{
    const char *dot = strrchr(ini, '.');
    size_t base = dot ? (size_t)(dot - ini) : strlen(ini);
    assert(base + strlen(suffix) + 1 <= size);
    memcpy(out, ini, base);
    strcpy(out + base, suffix);
}

static void test_file_gate_and_channel_guard(void)
{
    /* Written beside the test binary rather than in %TEMP%: the build directory
     * is the one place a test run is guaranteed to be allowed to write, and a
     * fixture that cannot be created would fail as "tracing is broken" when the
     * real cause is the environment.
     *
     * The path is made absolute because GetPrivateProfileIntW resolves a
     * relative name against the Windows directory, not the current directory.
     * The DLL always builds an absolute path from GetModuleFileNameW, so this
     * only has to hold for the test. */
    const char *ini_rel = "trace-test.ini";
    char ini[MAX_PATH];
    char wire_log[MAX_PATH];
    char served_log[MAX_PATH];
    char text[4096];
    wchar_t ini_w[MAX_PATH];
    uint8_t buttons[MU3_BUTTON_COUNT] = {0};
    size_t got;

    assert(GetFullPathNameA(ini_rel, MAX_PATH, ini, NULL) > 0);
    log_path_for(ini, "-wire.log", wire_log, sizeof(wire_log));
    DeleteFileA(wire_log);
    assert(MultiByteToWideChar(CP_ACP, 0, ini, -1, ini_w, MAX_PATH) > 0);

    /* Disabled: no file may appear, and the API must stay inactive. */
    write_text(ini, "[trace]\nenabled=0\n");
    mu3_trace_open(ini_w, MU3_TRACE_WIRE);
    assert(!mu3_trace_active());
    buttons[0] = 1;
    mu3_trace_wire_bytes(1000, buttons);
    assert(slurp(wire_log, text, sizeof(text)) == 0);

    /* Missing section, missing file, and an empty path must all be inert. */
    write_text(ini, "lever_neutral=1024\n");
    mu3_trace_open(ini_w, MU3_TRACE_WIRE);
    assert(!mu3_trace_active());
    mu3_trace_open(NULL, MU3_TRACE_WIRE);
    assert(!mu3_trace_active());
    mu3_trace_open(L"", MU3_TRACE_WIRE);
    assert(!mu3_trace_active());

    /* Enabled as the wire channel. */
    write_text(ini, "[trace]\nenabled=1\n");
    mu3_trace_open(ini_w, MU3_TRACE_WIRE);
    assert(mu3_trace_active());
    /* The header must name the channel, so a mixed-up pair of files is
     * detectable after the fact. */
    got = slurp(wire_log, text, sizeof(text));
    assert(got > 0 && strstr(text, "channel=wire") != NULL);
    assert(strstr(text, "columns:") != NULL);

    /* A served-channel call must be refused while the wire channel is open:
     * writing reconstructed 00/01 into the wire log would corrupt the only file
     * the hardware verdict is read from. It must also not disturb the wire
     * channel's census, so the first real wire report still reads as the
     * baseline rather than being compared against the served call's state. */
    mu3_trace_served_masks(2000, 0x01, 0x00);
    got = slurp(wire_log, text, sizeof(text));
    assert(strstr(text, "t=2000us") == NULL);

    /* First wire report: the baseline, written with no transition. */
    buttons[0] = 1;
    mu3_trace_wire_bytes(3000, buttons);
    got = slurp(wire_log, text, sizeof(text));
    assert(got > 0);
    assert(strstr(text, "t=3000us") != NULL);
    assert(strstr(text, "down=-") != NULL);

    /* Second report releases it: now a real transition appears. */
    buttons[0] = 0;
    mu3_trace_wire_bytes(4000, buttons);
    got = slurp(wire_log, text, sizeof(text));
    assert(strstr(text, "t=4000us") != NULL);
    assert(strstr(text, "up=L1") != NULL);

    mu3_trace_close();
    assert(!mu3_trace_active());

    /* Enabled as the served channel. It writes a different file, so the mirror
     * check has to read that one: the wire call must be refused there, and the
     * served call must land. */
    write_text(ini, "[trace]\nenabled=1\n");
    mu3_trace_open(ini_w, MU3_TRACE_SERVED);
    assert(mu3_trace_active());
    log_path_for(ini, "-served.log", served_log, sizeof(served_log));
    DeleteFileA(served_log);
    /* Re-open so the served file is created after the delete. */
    mu3_trace_open(ini_w, MU3_TRACE_SERVED);
    assert(mu3_trace_active());
    got = slurp(served_log, text, sizeof(text));
    assert(got > 0 && strstr(text, "channel=served") != NULL);

    /* The wire call must be refused on this channel. */
    buttons[0] = 1;
    mu3_trace_wire_bytes(4000, buttons);
    got = slurp(served_log, text, sizeof(text));
    assert(strstr(text, "t=4000us") == NULL);

    /* The served call must land, with the reconstructed byte columns. */
    mu3_trace_served_masks(5000, 0x01, 0x00);
    got = slurp(served_log, text, sizeof(text));
    assert(strstr(text, "t=5000us") != NULL);
    assert(strstr(text, "bytes=01 00") != NULL);
    mu3_trace_close();

    DeleteFileA(served_log);
    DeleteFileA(wire_log);
    DeleteFileA(ini);
}

/* The clock must be monotonic and sub-millisecond, since the whole measurement
 * is a few-millisecond gap that GetTickCount64 cannot resolve. */
static void test_clock_resolution(void)
{
    uint64_t a = mu3_trace_now_us();
    uint64_t b;
    unsigned spins = 0;
    assert(mu3_trace_now_us() != 0);
    /* Busy-wait for a sub-tick interval: if the clock were tick-granular the
     * value would not move within a few hundred microseconds. */
    do {
        b = mu3_trace_now_us();
        ++spins;
    } while (a == b && spins < 10000000u);
    assert(b >= a);
}

int main(void)
{
    test_format_shape();
    test_bounce_is_visible();
    test_baseline_is_silent();
    test_key_names();
    test_clock_resolution();
    test_file_gate_and_channel_guard();
    puts("trace tests passed");
    return 0;
}

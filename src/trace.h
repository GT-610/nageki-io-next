#ifndef MU3_TRACE_H
#define MU3_TRACE_H
/* Opt-in, passive button trace written from inside the DLL.
 *
 * Why this exists: the standalone probe (hid_probe.exe chord) reads the device
 * directly and is the cleanest device-side instrument, but it needs the cabinet
 * to itself. When the machine is in use by a player, the DLL is the only thing
 * that can observe the wire at all: it already holds the device, so it can
 * record what the controller reported, during real play, without disturbing it.
 *
 * The question it is built to answer: is one particular key faulty? A worn or
 * dirty switch does not fail cleanly, it chatters: a contact that opens for a
 * few milliseconds in the middle of a hold. The controller reports that
 * faithfully, this DLL passes it through faithfully, and the game sees a
 * release. On the wire that shows up as down, up, down in quick succession
 * where a healthy switch produces one down and one up. Nothing in the software
 * can invent that, and nothing in the software can hide it, so it separates a
 * hardware fault from everything downstream.
 *
 * The trace is off unless MU3CustomIO.ini beside the DLL says `trace=1`. With
 * it off, nothing here opens a file, allocates, or writes anything, and the
 * cost is one predictable branch per report.
 *
 * Two channels, in two files, because the answer is a comparison:
 *   wire   (owner process, amdaemon.exe): the button bytes straight off the
 *          device, before anything interprets them.
 *   served (game process, mu3.exe):      the sample mu3_io_poll hands to the
 *          game, after the shared-memory hop.
 * A key that chatters on the wire is a hardware fault. A key that is clean on
 * the wire but missing from served is a software fault. Both are logged with
 * the same timestamp source (GetTickCount64), so the two files can be aligned. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#include "button_map.h"

typedef enum mu3_trace_channel {
    MU3_TRACE_WIRE = 0, /* owner: raw button bytes off the device */
    MU3_TRACE_SERVED = 1 /* non-owner: the sample served to the game */
} mu3_trace_channel;

/* Reads the `trace` flag from the INI and, when set, opens the channel's log
 * file next to the DLL. Any failure (no file, no flag, unwritable directory)
 * leaves tracing off. This must never fail the DLL load: a diagnostic that can
 * stop the game from starting is worse than no diagnostic. */
void mu3_trace_open(const wchar_t *ini_path, mu3_trace_channel channel);
bool mu3_trace_active(void);
/* Microseconds from QueryPerformanceCounter, comparable between processes on
 * one machine. Exposed so the caller stamps a report once, at the moment it
 * read it, instead of re-reading the clock at several layers. */
uint64_t mu3_trace_now_us(void);

/* Log one report's real button bytes in payload order. Only the WIRE channel
 * accepts these, and only a change produces a line, so an idle stream writes
 * nothing. The first call only records a baseline.
 *
 * The channel check is inside the module rather than left to the call site on
 * purpose. Both channels share one census, so writing the reconstructed 00/01
 * bytes of the served channel into the wire log would corrupt the very file the
 * hardware verdict is read from - and it would look like plausible data rather
 * than like an error. A wrong-but-accepted write here would silently invert the
 * conclusion, so it is refused instead. */
void mu3_trace_wire_bytes(uint64_t us, const uint8_t buttons[MU3_BUTTON_COUNT]);
/* Log the decoded masks this process is about to serve the game. Only the
 * SERVED channel accepts these; the byte columns are reconstructed as 00/01 so
 * both files share one format a single analyzer can read. */
void mu3_trace_served_masks(uint64_t us, uint8_t left, uint8_t right);
/* Best effort; the process is normally killed rather than unloaded, and every
 * line is written unbuffered so nothing is lost when that happens. */
void mu3_trace_close(void);

/* Render one transition line, newline included. Pure, so the format the
 * analyzer parses is covered by the offline tests. Returns the length written,
 * or 0 when it does not fit. */
size_t mu3_trace_format(char *buf, size_t size, uint64_t us,
                        const mu3_chord_step *step,
                        const uint8_t buttons[MU3_BUTTON_COUNT]);

#endif

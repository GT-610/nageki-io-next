# nageki-io-next

English | [简体中文](README.zh-CN.md)

A native x64 IO DLL for the Nageki series running under segatools, written from scratch in C. It reads a Nageki-compatible USB HID controller and implements the segatools MU3 IO and Aime IO interfaces, including button lights and card reading.

## Interfaces

26 exports across two APIs:

- MU3 IO 0x0101: `mu3_io_init`, `mu3_io_poll`, `mu3_io_get_opbtns`, `mu3_io_get_gamebtns`, `mu3_io_get_lever`, `mu3_io_led_init`, `mu3_io_led_set_colors`
- Aime IO 0x0101: the five 0x0100 entry points plus the twelve added at 0x0101

## 64-bit build required

segatools loads this DLL into `mu3.exe` and `amdaemon.exe` through `LoadLibraryW`. Both are 64-bit processes, so a 32-bit build fails at load:

```
Ongeki IO: Failed to load IO DLL: 800700c1: MU3CustomIO.dll
```

`0x800700C1` is `ERROR_BAD_EXE_FORMAT` (193), an architecture mismatch. Exports and API version play no part in it. Because `mu3hook/dllmain.c` calls `ExitProcess` when the IO DLL fails to initialize, the process exits before the hook runs and the symptom looks like the hook never started.

`scripts/build.ps1` reads the PE header and fails the build when the DLL is not x64.

## Build

Requires the x64 compiler from Visual Studio 2022 Build Tools and the Windows SDK.

```
pwsh ./scripts/build.ps1
pwsh ./scripts/test.ps1
pwsh ./scripts/package.ps1
```

`build.ps1` builds the DLL, checks the PE header, loads the DLL and calls its exports, then runs a bind simulation. The simulation copies the symbol order and per-version symbol counts from segatools' `mu3_dll_syms` and `aime_dll_syms`. segatools binds all names or none and stops at the first missing one, so the simulation reports `mu3 7/7` and `aime 17/17`.

`test.ps1` runs synthetic tests for the button map, the trace, the input core, the LED packet encoder, card conversion, lever conversion and HID marshalling. `build/` and `dist/` are gitignored.

## Fault handling

`mu3_io_init` and `mu3_io_poll` always return `S_OK`. segatools calls `mu3_io_init` from `mu3_io4_hook_init` (`games/mu3hook/io4.c:35`), where a failure is fatal to process startup in the same way a failed DLL load is. A poll failure propagates out of `io4_async_poll` (`common/board/io4.c:349`). An absent controller therefore reports neutral input instead of an error.

Building with `/DMU3_IO_REPORT_DISCONNECT` makes poll return `HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)` once `MU3_DISCONNECT_REPORT_MS` has passed without a report. This exists for experiments that need a visible fault code. Whether the game surfaces an IO4 error for it is not established.

## Input validity

Device connection decides whether input is valid. Once the device connects and one valid report arrives, that report stays in effect until a newer one supersedes it or the device goes away, however much time passes. A newer report is the only thing that may replace one, because a lever at a stop or a button held down has to stay in effect while it is held.

Measurements from the deployed controller (`hid_probe.exe jitter 30000`) show it streams continuously at about 200 reports/s, 5986 reports in 30 s with the lever untouched. So in practice a fresh report always arrives within milliseconds and the rule above is a fallback rather than the normal path. It also means a silent device is detectable on this hardware; a genuinely change-triggered device would make silence indistinguishable from stillness.

One consequence is worth stating. If the firmware stalls while the device stays enumerated, the handle remains open, no read fails and no report arrives, so input freezes at the last report rather than releasing — a stuck button would stay stuck. A silence threshold would detect that, and the 200 reports/s measurement is what would make one safe to choose; it is not implemented, because doing so would reinstate the age-based invalidation the rule above exists to avoid for genuinely held input.

Across processes the same rule holds. Shared memory under `Local\MU3CustomIO-v1` carries a `published_ms` heartbeat that the HID worker refreshes on every tick, and only that heartbeat signals whether the owning process is alive. Report age is never consulted. An absent owner is detected primarily by the process handle: the reader opens the owner PID with `OpenProcess` and finds it unopenable or already signalled, which is immediate. The five-second heartbeat window is a backstop for a reused PID or a stale mapping. A silent worker still ticks every 50 ms and a streaming one ticks far faster, so that window is hundreds of missed ticks wide, which is deliberate: the condition worth detecting is `amdaemon.exe` actually vanishing, and widening the window costs nothing real while narrowing it under a scheduler stall or a suspended process would drop input that is still live. Both disconnect and reconnect clear the stored sample, so a stale button or card cannot be replayed.

A shared-memory read separates two failures that look alike but must not be answered the same way. If the copy raced the owner's write, the copy is torn and unusable — mixing two frames could invent or drop a key — but the owner is still alive and ticking, so the reader serves the previous consistent sample. That sample is a frame the device really sent, and for a held button it is still the truth; answering neutral instead would release every held button for one poll. If the owner is gone, the answer is neutral and the cached sample is dropped, so a departed owner's held button can never be replayed. `tests/core_tests.c` covers this policy, including that a departed owner clears the cache.

## Report layout

Reports are 65 bytes: one report ID byte followed by a 64-byte payload.

| Payload offset | Field |
|---|---|
| 0-9 | button bytes, five left then five right |
| 10-11 | lever, little-endian |
| 12 | card scan state |
| 13-22 | card ID |
| 23 | operator buttons |

A button byte counts as pressed when it is nonzero, so a device reporting `0x00`/`0xFF` behaves like one reporting 0/1. A scan value of anything except 1 or 2 means no card, and the rest of that report is still used.

A capture with `hid_probe.exe dump` confirms the report ID byte, which is `0x00`, the lever field at bytes 11-12 little-endian (payload offsets 10-11), and the operator byte at payload offset 23. The button, scan and card offsets still come from protocol analysis of an existing implementation and have not been checked against a capture; `hid_probe.exe` is how to confirm them.

That same capture read the two rear buttons off payload offset 23. They are the only two nonzero values this byte ever took:

| Byte | Meaning |
|---|---|
| `0x03` | the button that opens the Test menu: declares **Test and Service together** |
| `0x04` | the other button: **Coin** only |

Bit assignments are segatools's: `TEST=0x01`, `SERVICE=0x02`, `COIN=0x04` ([mu3io.h](legacy-repository/references/segatools/games/mu3io/mu3io.h)). A standalone `0x02` was never observed, so this controller cannot produce a Service-only event, and whether `0x03` is a firmware constant or a second electrical input is not determined. `0x04` cannot open the Test menu because it does not carry bit 0, which is what identifies it as the second button without needing an in-game test.

Two consequences. **Coin cannot be verified in game**: `mu3hook` increments the credit count on every poll where the bit is set rather than on a rising edge ([io4.c](legacy-repository/references/segatools/games/mu3hook/io4.c)), so holding the button counts many credits, and `mercuryhook` in the same tree does have the edge check. And **Service is not separately reachable** from this controller: both bits always appear together.

Rewriting this byte inside the IO DLL is the only way to separate the two keys, since the firmware cannot be reflashed. The DLL does have that power — `mu3_io_get_opbtns` returns the byte as it sees fit — but the meaning of each bit is fixed by segatools, the current build passes the byte through unchanged (`polled.operator_buttons & 7`), and anything that fabricates a Test-then-Service sequence would make this DLL stop reporting the controller's real state. It is deliberately **not implemented**: neither rear button is needed beyond reaching the Test menu, which does not justify giving up faithful pass-through. Performance is not the reason — that function reads one already-captured byte per call, so a bit manipulation there would be negligible.

## Lever conversion

`src/lever.c` holds the conversion. The original formula was:

```
lever = (short)( LeverOffset * 100 + raw * 32 * LeverSensitivity )
```

with defaults `LeverSensitivity = 1` and `LeverOffset = 0`, no clamp, and the `(short)` narrowing wrapping modulo 65536. It subtracts no centre, which assumes the firmware reports zero with the stick centred.

A capture from the deployed controller (`hid_probe.exe dump`) reads the lever field directly:

| Position | raw |
|---|---|
| full left | `0x032A` (810) |
| rest | `0x0402` (1026) |
| full right | `0x04A4` (1188) |

The two end stops are set by the mechanism and repeat. The resting value is not: the stick cannot be parked exactly at the electrical centre, so it reads wherever it happens to be left, and a later `jitter` run on the same controller read 1035 instead of 1026. Only `neutral` is a hardware constant; the rest reading is one observation of stick position.

The electrical centre is `0x0400` (1024), so the build subtracts it:

```
lever = (short)( (raw - neutral) * 32 * sensitivity ),   neutral = 1024
```

### Why the centre is subtracted

At centre the output becomes `(C - C) * 32 * sens = 0` at every sensitivity. The original produced `C * 32 * sens (mod 65536)`, which reaches zero only when `C * sens` is a multiple of 2048:

| sensitivity | original centre output |
|---|---|
| 1, 3, 5, ... | `0x8000` (half scale) |
| 2, 4, 6, ... | `0x0000` |

Since 2048 is 2^11, an even sensitivity centres when `v2(C) >= 10` and an odd one when `v2(C) >= 11`. Both conditions together force `v2(C) = 10`, so `C = 1024 * odd`. The probe picks 1024: the only other candidate in the 10-bit range, 3072, sits 2046 counts from the measured rest value.

At the deployed sensitivity 2 the centred conversion is bit-identical to the original for every raw value. The two differ by `1024 * 32 * sens`, a multiple of 65536 exactly when the sensitivity is even, and `tests/lever_tests.c` asserts this across all 65536 inputs. What the change buys is the odd sensitivities, which become usable.

### Range and sensitivity limit

Travel is about 378 counts, 810 to 1188, so the largest excursion from centre is 214 counts. Keeping `excursion * 32 * sens` inside the positive half of a signed 16-bit value caps the sensitivity at 4, since `214 * 32 * 5 = 34240` wraps negative. `MU3_LEVER_SENSITIVITY_MAX` is 4.

Only the low 10 bits of the centred value affect the result, because `1024 * 64 = 65536` at sensitivity 2. This follows from the original formula rather than being a defect of this implementation.

### Tuning

`MU3CustomIO.ini` beside the DLL overrides the defaults:

```
lever_neutral=1024
lever_sensitivity=2
```

`lever_neutral` takes 0..65535 and `lever_sensitivity` takes 1..4. A missing file, or a partly invalid one, keeps the defaults; a missing key keeps the value already in effect. Setting `lever_neutral=0` reproduces the original uncentred output. `MU3CustomIO.ini.txt` in the package is a commented template that has to be renamed to take effect. The file lives beside the DLL rather than beside the executables because both `mu3.exe` and `amdaemon.exe` load this one DLL.

### The calibration screen readings

The cabinet's lever calibration screen reads `B0FFH` left and `557FH` right at sensitivity 2. Both are odd, and both are congruent to 63 modulo 64. A lever value is always a multiple of `32 * sensitivity` and therefore even, so neither reading can be one. At sensitivity 2 the scale is 64, so `adcs[0] = 0x7FFF - lever` (`common/board/io4.c:125`) is congruent to 63 modulo 64, which is exactly what the readings show. The screen therefore displays the calibration signal, not the value `mu3_io_get_lever` returns. Inverting it puts raw near 828 and 1194, a span of 366 against the 378 the probe measured directly, a 3% difference.

That congruence holds when the scale is a multiple of 64, so at sensitivities 2 and 4 and not at 1 or 3, where `adcs` can also be 31 modulo 64. The readings were taken at sensitivity 2.

A centred lever reading `0000H` is the lever output, which is 0 at centre for even sensitivities under both the original and the centred formula. The two readings are different quantities, which is what made them look contradictory.

## HID matching

The DLL matches on VID `2341` and PID `8036`. It does not constrain report lengths, and an interface reporting 65 in and 65 out is preferred but not required. A read that times out neither refreshes nor invalidates the held input.

Reports are marshalled between this DLL's fixed 65-byte frame and whatever length the descriptor reports. A device reporting 64 carries payload only, so the frame's Report ID byte is dropped on write and supplied as 0 on read; writing all 65 bytes to such a device would place the ID byte where payload belongs, shift everything by one and drop the last payload byte, which mis-drives the lights rather than failing visibly. Devices reporting more than 65 are zero-padded. `tests/hid_pack_tests.c` covers these mappings.

A read waits at most 50 ms before the loop starts over; a write still waits 1000 ms. The deployed controller streams at about 200 reports/s, so a read returns in roughly 5 ms and that timeout is only reached when the device has gone quiet, where its only jobs are bounding how long a queued LED frame waits and keeping the heartbeat fresh. It never decides whether input is valid.

Input validity is decided by reads alone. A write that fails or times out drops that one queued colour frame and nothing else; the read that follows is what detects a device that is genuinely gone. Input and output are independent paths, so one failed colour write must not be able to take the buttons down with it.

The controller currently reports about 200 times per second while the game polls far less often, so reports are continuously queued and every one but the last is already superseded by the time it is read. The loop therefore keeps reading without waiting and delivers only the newest report, bounded at eight. Feeding the queue through in order would walk the game through lever and button states that are already stale. The game reads the current level through `mu3_io_get_gamebtns` and `mu3_io_get_lever`, so a discarded report carries no information it could have observed.

## Card reader

`scan == 1` returns the ten card bytes directly. `scan == 2` treats the first eight bytes as a big-endian hex value and returns its twenty decimal digits encoded as ten BCD bytes. The MIFARE, FeliCa transaction, reader LED and VFD entry points are stubs returning `S_FALSE`, and they advertise no capability the hardware lacks.

## Button decoding

The ten discrete button bytes at payload offsets 0-9 (five left, then five right) are decoded by `src/button_map.c`. A byte counts as pressed when it is nonzero, so a device reporting `0x00`/`0xFF` behaves like one reporting 0/1.

Each byte sets its own bit and nothing else: no bit is shifted across bytes and no byte is combined with another. That is per-key independence, and it means no key's state can influence another key's bit, so a fault that depends on which keys are held cannot originate in this DLL. `tests/button_map_tests.c` asserts it exhaustively over all 1024 possible button-byte vectors, rather than leaving it as a claim about the source.

The same module is linked into `hid_probe.exe`, so the probe and the DLL share one definition of which wire byte is which key and of where the button field sits in a report. Two independent re-derivations could disagree, and a disagreement there would misread which key moved — indistinguishable from a real input fault.

## Button trace

A second, opt-in instrument exists for the case the probe cannot cover: the cabinet is in use by a player, so nothing can take the device exclusively. The DLL already holds the device, so it can record what the controller reported during real play without disturbing it.

It is off unless `MU3CustomIO.ini` beside the DLL says:

```
[trace]
enabled=1
```

With tracing off, nothing opens a file and the cost is one branch per report. With it on, each process writes one log beside the DLL, in the format `trace_analyze.exe` reads:

| File | Written by | Contains |
|---|---|---|
| `MU3CustomIO-wire.log` | `amdaemon.exe` (the HID owner) | button bytes straight off the device |
| `MU3CustomIO-served.log` | `mu3.exe` | the sample `mu3_io_poll` hands the game |

Both are timestamped from `QueryPerformanceCounter`, which is comparable across processes on one machine and fine enough to measure a switch bounce; `GetTickCount64` is not, at about 15.6 ms. Only button *changes* are written, so an idle stream produces nothing. Each file starts with a baseline line so the first real transition has something to compare against.

The distinction the two files draw: a key that misbehaves on the wire is a controller fault, and a key that is clean on the wire but missing from the served log is a software fault. The wire file is the one that settles a hardware question, because a contact that opens mid-hold shows up there as down, up, down — something no layer above the device could invent.

```
trace_analyze.exe MU3CustomIO-wire.log
```

The analyzer reports, per key, presses, releases and *clicks*: a release and re-press of one key within 15 ms. A player cannot do that deliberately in a rhythm game, so a click is a bouncing contact — the switch opened briefly while the key was being held. It also reports the shortest complete press-release seen per key, which is the single clearest sign of a worn switch: a few hundred microseconds there is a contact making and breaking, not a finger.

Two normalisations matter, and both are printed:

- **Clicks per press**, not the raw click count. A key played twice as often shows twice the clicks for the same defect rate, so a raw count mostly measures how much the key is used. The rate is what compares one switch against another.
- **The control.** One key clicking on a percent of its presses while the others stay near zero, at comparable press counts, is a difference between two switches under the same player. Every key clicking at a similar rate points at the player or the environment instead.

The analyzer derives each transition from the byte columns and uses those for the verdict, ignoring the `down=`/`up=` text; it reports how often the two disagreed. A log with a wrong text column still produces the right answer instead of a confidently wrong one.

## Probe

`build/hid_probe.exe` is a standalone tool. The game never loads it.

```
hid_probe.exe             list HID devices with VID, PID and caps
hid_probe.exe dump 30000  open the controller and print only changed reports for 30 s
hid_probe.exe jitter 30000
                          collect reports for 30 s with the lever untouched
hid_probe.exe chord 60000
                          census of the ten button bytes, per key, by how many
                          other keys were held when it went down
```

Run it on the machine the controller is plugged into. Press and release one button at a time; the byte that changes belongs to that button. Push the lever to both stops to read the real range. A capture from the deployed controller has already fixed the report ID, the lever field and its centre; the button, scan and card offsets in this DLL still come from inference, so the probe is what turns them into measurements.

`jitter` measures how steady the reading is when nobody is touching the lever, which is what decides whether a noise gate is worth having. It prints the arrival rate, the minimum, maximum and spread, and a histogram of values around 1024. On the deployed controller the result was 5986 reports in 30 s (about 200 reports/s) all reading the same value, a spread of 0, so a noise gate is not warranted: it would only suppress genuine slow movement. The histogram window is 32 counts either side of 1024, and samples outside it are counted separately because that is the lever being moved rather than noise.

`jitter` also prints the resting value and its offset from 1024. That offset is **not** a drift measurement and **not** a calibration: the stick is mechanical and cannot be parked exactly at the electrical centre, so it rests wherever it happens to rest. The deployed controller read 1035 at rest (+11, about 704 units of lever output at sensitivity 2), and an earlier capture of the same controller read 1026 (+2). Those are two different stick positions, not a change in the hardware, and neither is grounds for adjusting `lever_neutral`. What the spread and histogram do establish is that the reading is stable and exactly reproducible while the stick is untouched.

`chord` answers a different question: when several keys are held and one more is pressed, does the controller report the new key at all? It reads the ten button bytes straight off the wire, so — run with the game closed — its result depends on neither this DLL nor segatools. It prints every change as it happens and ends with a per-key census, including how many times each key went down while a given number of other keys were already held. A key counted under "4 held" was reported by the controller under that combination, so the press was not lost on the wire and any loss is downstream. A key never counted there, while the user is certain they pressed it and the same combination keeps failing at slow speed, points at the controller's own key scanning — a matrix ghosting or jamming signature, which no DLL change can fix. Failures that move between combinations, or that vanish when the pace is slowed, look like human timing instead. The census also records the distinct byte values seen per key, because a binary switch should only ever show `00` and one nonzero value; a third value would falsify the polarity model. Keys are reported by payload offset (`L1`..`L5`, `R1`..`R5`) rather than by physical name, because the byte-to-key mapping is still unconfirmed; the census only ever compares an offset against itself, so its result does not depend on the names being right.

## Deployment

1. Back up the IO DLL, the segatools configuration and the game saves currently in use, and record their paths and hashes.
2. Put the new DLL in a separate directory and point only the segatools IO and Aime configuration at it. Check the buttons and their polarity, the lever at centre and at both stops, card reading and the LEDs against the DLL being replaced.
3. Confirm that disconnecting the controller turns input neutral without reusing the last buttons or card, and that reconnecting restores input. Whether segatools or the game reports an IO4 fault for this DLL, and with which code, is not established.
4. On any load failure, wrong button direction, card error, LED misplacement or game fault, exit the game, restore the original configuration and DLL, and restart both processes. Do not replace the DLL while the game is running.

`amdaemon.exe` is assumed to be the only process holding the HID device. If the DLL is configured for both processes and only one of them may open the device, check which one before relying on the shared-memory path.

## License

[BSD 2-Clause "Simplified" License](LICENSE).


# Nageki IO Next

[简体中文](README.zh-CN.md)

**This project is not an official Nageki project, and is not affiliated with Nageki.**

A native x64 IO for the Nageki series of controllers, written from scratch in C.

## Supported controllers

- Nageki Micro
- Nageki standard (untested)
- Other controllers conforming to the Nageki input specification (untested)

## Interfaces

26 exports across two APIs:

- MU3 IO 0x0101: `mu3_io_init`, `mu3_io_poll`, `mu3_io_get_opbtns`, `mu3_io_get_gamebtns`, `mu3_io_get_lever`, `mu3_io_led_init`, `mu3_io_led_set_colors`
- Aime IO 0x0101: the five 0x0100 entry points, plus the twelve added at 0x0101

## Build

Requires the x64 compiler from Visual Studio 2022 Build Tools and the Windows SDK.

```
pwsh ./scripts/build.ps1
pwsh ./scripts/test.ps1
pwsh ./scripts/package.ps1
```

`build.ps1` builds the DLL, checks the PE header, loads the DLL and calls its exports, then runs a bind simulation. The simulation copies the symbol order and per-version symbol counts from segatools' `mu3_dll_syms` and `aime_dll_syms`. segatools binds all names or none and stops at the first missing one, so under normal conditions the simulation reports `mu3 7/7` and `aime 17/17`.

`test.ps1` runs synthetic tests for the button map, the input core, the LED packet encoder, card conversion, lever conversion and HID marshalling.

segatools loads this DLL into `mu3.exe` and `amdaemon.exe` through `LoadLibraryW`. Both are 64-bit processes, so a 32-bit build fails at load:

```
Ongeki IO: Failed to load IO DLL: 800700c1: MU3CustomIO.dll
```

`scripts/build.ps1` reads the PE header and fails the build outright when the DLL is not x64.

## Deployment

1. Back up the IO DLL and the segatools configuration currently in use.
2. Download the DLL from Releases and extract it into the game directory, then edit `segatools.ini` so the `IO` and `Aime` `path` entries point at it.
3. Once the game has started, enter the test menu and choose `レバー設定`. First choose `初期設定に戻す` to reset the lever's movable range, then move the lever slowly all the way to the left and to the right and check that the `可動域調整` value matches `レバー位置` at both extremes. Finally choose `終了` to save the lever settings.

If `amdaemon.exe` crashes, check the log output and the configuration. If the configuration is correct, please open an issue.

## Contributing

We welcome any meaningful contribution or PR. We do not care whether a contribution was written by an AI; the only requirement is that **contributions must not be signed on behalf of an AI**.

## License

[BSD 2-Clause "Simplified" License](LICENSE)

---
---
# Nageki IO Research

The following was joint research with DeepSeek V4.1 Flash, kept for reference.

## Fault handling

`mu3_io_init` and `mu3_io_poll` always return `S_OK`. segatools calls `mu3_io_init` from `mu3_io4_hook_init` (`games/mu3hook/io4.c:35`), where a failure is just as fatal to process startup as a failed DLL load; a poll failure surfaces out of `io4_async_poll` (`common/board/io4.c:349`). An offline controller therefore reports neutral input rather than an error.

Building with `/DMU3_IO_REPORT_DISCONNECT` makes poll return `HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)` once `MU3_DISCONNECT_REPORT_MS` has passed with no report. That switch exists for experiments that need a visible fault code. Whether the game shows an IO4 error because of it is not yet established.

## Input validity

Whether input is valid is decided by device connection state. Once the device has connected and one valid report has arrived, that report stays in effect until a newer one supersedes it or the device goes away, however much time passes in between. **Only a newer report can replace an older one**, because a lever resting at a stop or a button held down has to stay valid for as long as it is held.

Measurements from the deployed controller (`hid_probe.exe jitter 30000`) show that it **streams continuously**, at about 200 reports/s, and that 30 seconds with the lever untouched yields 5986 reports. In practice a new report therefore always arrives within a few milliseconds, and the rule above is a fallback rather than the normal case. It also means that on this hardware "the device has gone silent" is detectable; a genuinely change-triggered device could not tell silence apart from stillness.

One consequence is worth stating. If the firmware wedges while the device stays enumerated, the handle stays open, reads never fail and no report ever arrives, so input **freezes on the last frame instead of going neutral** — the key that was held down stays held. A silence threshold would detect this, and the measured 200 reports/s is exactly what would make such a threshold safe to choose; it is **not implemented** today, because doing so would reintroduce the age-based invalidation that the rule above exists specifically to avoid, and what it would protect is precisely the input that is genuinely held.

The same holds across processes. The shared memory under `Local\MU3CustomIO-v1` carries a `published_ms` heartbeat that the HID worker refreshes on every tick, and only that heartbeat indicates whether the owning process is still alive; report age never takes part in the decision. A vanished owner is judged primarily by the process handle itself: the reader opens the owner's PID with `OpenProcess` and declares it dead immediately once it cannot be opened or is already signalled, independently of the heartbeat window. The five-second window is only a backstop for a reused PID or a leftover mapping; a silent worker still ticks every 50 ms and a streaming one ticks far faster, so that window spans hundreds of ticks, which is deliberate — the condition actually worth detecting is `amdaemon.exe` vanishing, and widening it costs nothing in practice, while narrowing it would drop still-valid input on a scheduling stall or a suspended process. Both disconnect and reconnect clear the stored sample, so a stale button or card is never replayed.

A shared-memory read distinguishes two failures that look alike but must never be handled the same way. If the copy collided with the owner's write, that copy is torn and unusable — mixing two frames could invent or swallow a key — but the owner is still alive and ticking, so the reader keeps serving the **previous consistent sample**: that is a frame the device really did send, and for a held key it is still the truth; answering neutral would release every held key for one poll. If the owner is genuinely gone, the answer is neutral and the cache is dropped, so **a departed owner's held keys are never replayed**. `tests/core_tests.c` covers this policy, including "a departed owner must clear the cache".

## Report layout

Reports are 65 bytes: one Report ID byte plus a 64-byte payload.

| Payload offset | Field |
|---|---|
| 0-9 | button bytes, five left then five right |
| 10-11 | lever, little-endian |
| 12 | card scan state |
| 13-22 | card ID |
| 23 | operator buttons |

A button byte counts as pressed when it is nonzero, so a device reporting `0x00`/`0xFF` behaves just like one reporting 0/1. A scan state other than 1 or 2 is treated as no card, and the rest of that same report is still used.

## Button decoding

The ten discrete button bytes at payload offsets 0-9 (five left, then five right) are decoded by `src/button_map.c`. Each byte sets only its own bit: no bit is shifted across bytes and none is combined with another. That is **per-key independence** — no key's state can affect another key's bit, so a dropped key that depends on which keys are held cannot originate in this DLL. `tests/button_map_tests.c` asserts this **exhaustively over all 1024 button-byte combinations**, rather than leaving it as a claim about the source.

The same module is linked into `hid_probe.exe`, so the probe and the DLL share a single definition of which wire byte is which key and where the button field sits in a report. Two independent re-derivations could disagree, and such a disagreement would misidentify which key moved — something indistinguishable from a real input fault.

A capture taken with `hid_probe.exe dump` confirms the Report ID byte is `0x00`, that the lever field is at bytes 11-12 little-endian (payload offsets 10-11), and that **the operator buttons are at payload offset 23**. The button, scan and card offsets still come from protocol analysis of the previous implementation and have not been checked against a capture; `hid_probe.exe` is how to confirm them.

That same capture read the values of the two rear buttons. This byte only ever took two nonzero values:

| Value | Meaning |
|---|---|
| `0x03` | the button that opens the Test menu: declares **Test and Service together** |
| `0x04` | the other button: **Coin only** |

The bit assignments come from segatools: `TEST=0x01`, `SERVICE=0x02`, `COIN=0x04` (segatools `mu3io.h`). A standalone `0x02` never appeared, so this controller cannot produce a Service event on its own; whether `0x03` is a fixed firmware value or a second electrical input is not yet distinguished. `0x04` does not carry bit 0, so it cannot open the Test menu — which is exactly what identifies it as the other button without needing an in-game test.

There are two consequences. **Coin cannot be verified in game**: `mu3hook` is "increment once per poll while this bit is set" rather than counting rising edges (segatools `io4.c`), so holding the button adds many coins, whereas `mercuryhook` in the same source tree does have edge detection. **Service cannot be triggered on its own**: the two bits always appear together.

Since the firmware cannot be reflashed, the only place left to separate the two keys is rewriting this byte inside the IO DLL. The DLL does have that capability — what `mu3_io_get_opbtns` returns is its own decision — but the meaning of each bit is fixed by segatools, and the current implementation passes it through unchanged (`polled.operator_buttons & 7`); anything that fabricated a Test-then-Service sequence would make this DLL stop reporting the controller's real state. It is therefore **deliberately not implemented**: neither rear button is a necessity, being able to reach the Test menu is enough, and that is not worth giving up faithful pass-through. This is not a performance consideration — that function reads a single already-captured byte per call, so a layer of bit manipulation there would be negligible.

## Lever conversion

The conversion lives in `src/lever.c`. The original formula was:

```
lever = (short)( LeverOffset * 100 + raw * 32 * LeverSensitivity )
```

with defaults `LeverSensitivity = 1` and `LeverOffset = 0`, no clamp, and the `(short)` narrowing wrapping modulo 65536. It subtracts no zero point, which amounts to assuming the firmware reports zero when the stick is centred.

Using `hid_probe.exe dump`, the lever field was read directly on the deployed controller:

| Position | raw |
|---|---|
| full left | `0x032A` (810) |
| at rest | `0x0402` (1026) |
| full right | `0x04A4` (1188) |

The two end stops are set by the mechanism and are reproducible. **The resting value is not**: the stick cannot be parked exactly at the electrical centre, so it reads wherever it happens to be left, and a later `jitter` on the same controller read 1035 rather than 1026. `neutral` is the hardware constant; a resting reading is just an observation of one parking position.

The electrical centre is `0x0400` (1024), so this build subtracts it:

```
lever = (short)( (raw - neutral) * 32 * sensitivity ),   neutral = 1024
```

### Why the centre is subtracted

The centre output becomes `(C - C) * 32 * sens = 0`, which holds at any sensitivity. The original produced `C * 32 * sens (mod 65536)`, which reaches zero only when `C * sens` is a multiple of 2048:

| sensitivity | original centre output |
|---|---|
| 1, 3, 5, ... | `0x8000` (half scale) |
| 2, 4, 6, ... | `0x0000` |

Since 2048 is 2^11, an even sensitivity centres when `v2(C) >= 10` and an odd one needs `v2(C) >= 11`. Both conditions together force `v2(C) = 10`, i.e. `C = 1024 * odd`. The probe data picks 1024: the other candidate in the 10-bit range, 3072, sits 2046 counts away from the measured resting value.

At the deployed baseline sensitivity of 2, the centred conversion is bit-for-bit identical to the original for **every** raw value — the two differ by `1024 * 32 * sens`, which is a multiple of 65536 exactly when the sensitivity is even, and `tests/lever_tests.c` asserts this over all 65536 inputs. What this change buys is that the odd settings become usable.

### Range and sensitivity limit

Travel is about 378 counts, 810 to 1188, so the largest excursion from the centre is 214 counts. To keep `excursion * 32 * sens` inside the positive half of a signed 16-bit value, the sensitivity ceiling is 4: `214 * 32 * 5 = 34240` wraps negative. `MU3_LEVER_SENSITIVITY_MAX` is 4.

Only the low 10 bits of the value after subtracting the centre affect the result, because `1024 * 64 = 65536` at sensitivity 2. This follows from the original formula itself, and is not a defect of this implementation.

### Tuning

`MU3CustomIO.ini` beside the DLL can override the defaults:

```
lever_neutral=1024
lever_sensitivity=2
```

`lever_neutral` takes 0..65535 and `lever_sensitivity` takes 1..4. If the file does not exist, or its contents are partly invalid, the defaults are kept; if a single key is missing, the value already in effect is kept. Setting `lever_neutral=0` restores the original uncentred output. The `MU3CustomIO.ini.txt` in the package is a commented template that has to be renamed to take effect. The file sits beside the DLL rather than beside the executables, because `mu3.exe` and `amdaemon.exe` load the same one DLL.

### The calibration screen readings

At sensitivity 2 the cabinet's lever calibration screen reads `B0FFH` on the left and `557FH` on the right. Both are odd, and both are congruent to 63 modulo 64. A lever value is always a multiple of `32 * sensitivity` and therefore always even, so it cannot be either of these readings. At sensitivity 2 the scale is 64, so `adcs[0] = 0x7FFF - lever` (`games/mu3hook/io4.c:125`) is congruent to 63 modulo 64, which matches the readings exactly. The screen is therefore displaying the calibration signal, not the value `mu3_io_get_lever` returns. Inverting it gives raw around 828 and 1194, a span of 366; the span the probe measured directly is 378, a 3% difference.

That congruence holds when the scale is a multiple of 64, that is at sensitivities 2 and 4; at 1 and 3 `adcs` can also be 31, and these two readings were taken at sensitivity 2.

The `0000H` read at centre is the lever output itself — under an even sensitivity both the original and the centred formula give 0 at the centre. These two readings are different quantities, which is exactly why they previously looked contradictory.

## HID matching

The DLL matches on VID `2341` and PID `8036`. It does not constrain report lengths, and an interface reporting 65 in and 65 out is preferred but not required. A read timeout neither refreshes nor invalidates the input already held.

Reports are marshalled between this DLL's fixed 65-byte frame and the length the descriptor reports. A device reporting 64 carries payload only, so the frame's Report ID byte is dropped on write and supplied as 0 on read; sending the full 65 bytes to such a device would put the ID byte where payload belongs, shift everything forward by one and drop the last payload byte — which mis-drives the lights rather than failing visibly. Devices reporting more than 65 are zero-padded. These mappings are covered by `tests/hid_pack_tests.c`.

A read waits at most 50 ms before returning to the top of the loop; a write still waits 1000 ms. The deployed controller streams continuously at about 200 reports/s, so a read actually returns in about 5 ms and that timeout is only ever run out when the device has gone quiet, at which point its only jobs are to bound how long a queued light frame waits and to keep the heartbeat fresh. It never takes part in deciding whether input is timely.

Input validity is decided by **reads** alone. A write that fails or times out only drops that one queued colour frame and affects nothing else; whether the device has genuinely gone is discovered by the reads that follow. Input and output are two independent paths, so one failed colour write must not be able to take the buttons down with it.

The controller currently reports about 200 times per second while the game polls far more slowly than that, so reports pile up continuously and every one except the last has already been superseded by a newer one by the time it is read. After reading one report the loop therefore keeps reading with zero wait and keeps **only the newest one** to hand to the game; feeding them through in order would walk the game through a string of already-stale lever and button states. The bound is 8 reports. The game reads the current level through `mu3_io_get_gamebtns` / `mu3_io_get_lever`, so a discarded report carries no information the game could have observed anyway.

## Card reader

`scan == 1` returns the ten card bytes directly. `scan == 2` treats the first eight bytes as a big-endian hexadecimal number and returns its twenty decimal digits encoded as ten BCD bytes. The MIFARE, FeliCa transaction, reader LED and VFD entry points are placeholder implementations returning `S_FALSE`, and they do not claim capabilities the hardware does not have.

## Button trace

There is a second, on-demand instrument built specifically for the cases the probe cannot cover: the cabinet is occupied by a player, so nothing can take the device exclusively. The DLL already holds the device, so it can record what the controller actually reported **without disturbing play**.

Unless `MU3CustomIO.ini` beside the DLL says otherwise, it is off:

```
[trace]
enabled=1
```

When off, no file is opened and each report costs only one branch test. When on, each of the two processes writes one log beside the DLL, in exactly the format `trace_analyze.exe` reads:

| File | Written by | Contains |
|---|---|---|
| `MU3CustomIO-wire.log` | `amdaemon.exe` (the HID owner) | the button bytes as the device sent them |
| `MU3CustomIO-served.log` | `mu3.exe` | the sample `mu3_io_poll` handed to the game |

Both are timestamped from `QueryPerformanceCounter` — comparable across processes on the same machine, and fine enough to measure switch bounce; `GetTickCount64` cannot, at about 15.6 ms. Only button **changes** are written, so an idle stream produces no output at all. Each file opens with a baseline line, so that the first real change has something to compare against.

What the two files distinguish is precisely this: **a key that misbehaves on the wire is a controller fault, while a key that is clean on the wire but missing from the served log is a software fault.** Judging a hardware problem means looking at the wire file, because a contact opening during a hold shows up there as press, release, press — a shape that nothing above the device could invent.

```
trace_analyze.exe MU3CustomIO-wire.log
```

The analyzer reports, per key, the press and release counts, and **click**: a release followed by a press of the same key within 15 ms. A player cannot do that deliberately in a rhythm game, so one click is contact bounce — the switch opened briefly while it was held. It also reports the shortest complete press-release seen for each key, which is the single clearest sign of a worn switch: a few hundred microseconds cannot be a finger, it is a contact making and breaking.

Both normalisations matter, and both are printed:

- **Look at the click rate per press, not the absolute click count.** A key that is played more shows more clicks for the same defect rate, so the absolute count mostly measures how much you used that key. To compare two switches, compare the rates.
- **Look at the control.** A key with a click rate on the order of one percent while the others sit near zero at comparable press counts is a difference between two switches under the same player. If every key's rate is about the same, the common factor is the player or the environment, not the hardware.

The analyzer derives each transition from the byte columns **by itself** and judges on that basis, ignoring the `down=`/`up=` text columns, and it reports how often the two disagreed. That way the conclusion stays correct even if a log's text columns are wrong, instead of producing a confident wrong answer.

## Probe

`build/hid_probe.exe` is a standalone tool; the game never loads it.

```
hid_probe.exe              list HID devices with their VID, PID and caps
hid_probe.exe dump 30000   open the controller and print only changed reports, for 30 seconds
hid_probe.exe jitter 30000 collect 30 s of reports with the lever untouched and characterise the noise
hid_probe.exe chord 60000  a per-key census of the ten button bytes, counted by how many other keys were already held when it went down
```

Run it on the machine the controller is plugged into. Press and release one key at a time; the byte that changes belongs to that key. Push the lever to both extremes to read the real range. The capture from the deployed controller has already settled the Report ID, the lever field and its centre; the button, scan and card offsets in this DLL still come from inference, and the probe is the means of turning them into measured values.

`jitter` measures how steady the reading is when nobody touches the lever, which is exactly what decides whether a noise gate is worth adding. It prints the arrival rate, the minimum, maximum and spread, and a histogram of the values around 1024. On the deployed controller the result was 5986 reports in 30 seconds (about 200 reports/s), all the same value, with a spread of 0, so **no noise gate is needed**: it would only suppress genuine slow movement. The value window is ±32 counts, and samples outside the window are counted separately, because that is the lever being moved rather than noise.

`jitter` also prints the resting value and its offset from 1024. That offset is **neither** a drift measurement **nor** a calibration: the stick is mechanical and cannot be parked exactly at the electrical centre, it rests wherever it happens to rest. The deployed controller read 1035 at rest (+11, about 704 units at sensitivity 2), and an earlier capture of the same controller read 1026 (+2). These are **two different stick parking positions, not a change in the hardware**, and neither is grounds for adjusting `lever_neutral`. What the spread and the histogram really establish is that the reading is stable and exactly reproducible while the stick is untouched.

`chord` answers a different question: when several keys are held and one more is pressed, does the controller report that new key at all? It reads those ten button bytes straight off the wire, so — provided it is run with the game **closed** — its conclusion depends on neither this DLL nor segatools. It prints every change in real time and finally gives a per-key census, which includes how many times each key went down while N other keys were already held. A key counted in the "4 others held" column means the controller did report it under that combination, so that press was not lost on the wire and the loss happens downstream. Conversely, if the user is certain they pressed it and repeating the same combination slowly still never counts, that points at the controller's own key scanning — a matrix ghosting / jamming signature that no DLL change can fix. Failures that wander between combinations, or that disappear when you slow down, look more like handling timing than the device. The census also records the distinct byte values each key has shown: a binary switch should only ever show `00` and one nonzero value, and a third value falsifies the polarity model. Keys are reported by payload offset (`L1`..`L5`, `R1`..`R5`) rather than by physical key name, because the byte-to-key correspondence is still unverified; the census only ever compares an offset against itself, so whether the names are right does not affect the conclusion.

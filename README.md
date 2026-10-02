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

`test.ps1` runs synthetic tests for the input core, the LED packet encoder, card conversion and lever conversion. `build/` and `dist/` are gitignored.

## Fault handling

`mu3_io_init` and `mu3_io_poll` always return `S_OK`. segatools calls `mu3_io_init` from `mu3_io4_hook_init` (`games/mu3hook/io4.c:35`), where a failure is fatal to process startup in the same way a failed DLL load is. A poll failure propagates out of `io4_async_poll` (`common/board/io4.c:349`). An absent controller therefore reports neutral input instead of an error.

Building with `/DMU3_IO_REPORT_DISCONNECT` makes poll return `HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)` once `MU3_DISCONNECT_REPORT_MS` has passed without a report. This exists for experiments that need a visible fault code. Whether the game surfaces an IO4 error for it is not established.

## Input validity

Device connection decides whether input is valid. Once the device connects and one valid report arrives, that report stays in effect until the device goes away, however much time passes. A change-triggered controller sends nothing while the lever and buttons are still, so expiring a report on a timer would release held inputs.

Across processes the same rule holds. Shared memory under `Local\MU3CustomIO-v1` carries a `published_ms` heartbeat that the HID worker refreshes on every tick, and only that heartbeat signals whether the owning process is alive. Report age is never consulted. Input turns neutral two seconds after the owner disappears, and both disconnect and reconnect clear the stored sample so a stale button or card cannot be replayed.

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

These offsets come from protocol analysis of an existing implementation and have not been checked against a captured report. `hid_probe.exe` is the way to confirm them.

## Lever conversion

`src/lever.c` holds the conversion:

```
lever = (short)( LeverOffset * 100 + raw * 32 * LeverSensitivity )
```

The defaults are `LeverSensitivity = 1` and `LeverOffset = 0`. There is no clamp, and the `(short)` narrowing wraps modulo 65536. The current build sets sensitivity 2, giving a scale of `raw * 64`.

Measured on a cabinet at sensitivity 2, the calibration display reads `0000H` at centre, `B0FFH` at the left stop and `557FH` at the right stop. Dividing out the scale puts raw at roughly -316 and +342 either side of zero.

| Position | Displayed | raw | `raw * 64` |
|---|---|---|---|
| left | `B0FFH` | about -316 | `B100H` (-20224) |
| centre | `0000H` | 0 | `0000H` |
| right | `557FH` | about +342 | `5580H` (+21888) |

`B100H` and `5580H` bracket the readings, which are approximate because neither displayed value divides by 64. The sign convention, negative at the left with the high bit set, matches the range `mu3io.h` documents for a real cabinet, near `0xB000` left and `0x5000` right. `tests/lever_tests.c` uses these three readings as regression input.

Only the low 10 bits of raw affect the result. Since `1024 * 64 = 65536`, raw `0x0400` wraps to zero output. This follows from the original formula, where a 10-bit ADC scaled by 32 fills 16 bits and doubling the sensitivity wraps twice. It is not a defect, but it does mean the formula cannot describe raw values much beyond ±512.

### Odd sensitivities put centre at half scale

At even sensitivity the lever centres correctly. At odd sensitivity the centre reads half scale.

The formula subtracts no zero offset, so it assumes raw is zero at centre. Combined with the wrap, the centre output is:

```
C * 32 * sens  (mod 65536)  =  32768 * sens  (mod 65536)
```

| sens | centre output |
|---|---|
| 1, 3, 5, ... | `0x8000` |
| 2, 4, 6, ... | `0x0000` |

Centre reaches zero exactly when `C * sens` is a multiple of 2048. Since 2048 is 2^11, let v2(n) be the number of factors of two in n:

- even sens needs `v2(C) >= 10`
- odd sens needs `v2(C) >= 11`, because an odd number shares no factor of two with 2^11

Both conditions together force `v2(C) = 10`:

```
C = 1024 * odd = 0x400, 0xC00, 0x1400, ...
```

The conclusion is forced. Were C zero, both parities would centre correctly; were C `0x800`, both would as well. Each contradicts the observation.

So the firmware reports a nonzero centre and the formula never subtracts it. An even sensitivity multiplies that offset into a whole number of wraps, which hides it. Both parities come from the same defect, and the even setting is the one that happens to be self-consistent.

### Range

At sensitivity 2 the stops sit about 316 counts left and 342 counts right of centre, a travel of roughly 658 counts. The 26-count difference between the two sides is ordinary, since the mechanical centre of a potentiometer need not match its electrical centre. With C = 1024, raw runs from about 708 to 1366.

### Sensitivity limit

The positive half of a signed 16-bit value ends at 32767, so half the travel multiplied by 32 and by the sensitivity has to stay below that. At the right stop, 342 * 32 = 10944, which caps the sensitivity at 2.99.

Sensitivity 2 is therefore the ceiling. At sensitivity 3 the right stop reaches 342 * 96 = 32832, which wraps negative, and pushing the lever fully right throws it to the opposite end. This is why only the even settings are usable.

### Proposed change

Subtracting the centre before scaling removes the parity problem and centres exactly:

```
lever = (raw - 1024) * 64
```

The centre then evaluates to `(C - C) * 32 * sens = 0` at any sensitivity, and the headroom rises to about sensitivity 5. This is not implemented yet.

### Open question

Whether `B0FFH` and `557FH` are the values returned by `mu3_io_get_lever` or the calibration signal `0x7FFF - lever` that segatools passes to the game. `mu3io.h` documents `0xB000` and `0x5000` for a real cabinet, which matches these readings closely and points at the calibration signal, putting raw near -196 to +170. A centre of `0000H` points at the lever value, putting raw near -316 to +342.

The scale is `* 64` either way, so the conversion is unaffected and only the expected raw range differs. Reading the lever bytes with `hid_probe.exe` settles it: a span near ±200 indicates the first reading, near ±340 the second.

## HID matching

The DLL matches on VID `2341` and PID `8036`. It does not constrain report lengths, and an interface reporting 65 in and 65 out is preferred but not required. A read that times out neither refreshes nor invalidates the held input.

## Card reader

`scan == 1` returns the ten card bytes directly. `scan == 2` treats the first eight bytes as a big-endian hex value and returns its twenty decimal digits encoded as ten BCD bytes. The MIFARE, FeliCa transaction, reader LED and VFD entry points are stubs returning `S_FALSE`, and they advertise no capability the hardware lacks.

## Probe

`build/hid_probe.exe` is a standalone tool. The game never loads it.

```
hid_probe.exe             list HID devices with VID, PID and caps
hid_probe.exe dump 30000  open the controller and print only changed reports for 30 s
```

Run it on the machine the controller is plugged into. Press and release one button at a time; the byte that changes belongs to that button. Push the lever to both stops to read the real range. The offsets, polarities and lever range in this DLL all come from inference, so the probe is what turns them into measurements.

## Deployment

1. Back up the IO DLL, the segatools configuration and the game saves currently in use, and record their paths and hashes.
2. Put the new DLL in a separate directory and point only the segatools IO and Aime configuration at it. Check the buttons and their polarity, the lever at centre and at both stops, card reading and the LEDs against the DLL being replaced.
3. Confirm that disconnecting the controller turns input neutral without reusing the last buttons or card, and that reconnecting restores input. Whether segatools or the game reports an IO4 fault for this DLL, and with which code, is not established.
4. On any load failure, wrong button direction, card error, LED misplacement or game fault, exit the game, restore the original configuration and DLL, and restart both processes. Do not replace the DLL while the game is running.

`amdaemon.exe` is assumed to be the only process holding the HID device. If the DLL is configured for both processes and only one of them may open the device, check which one before relying on the shared-memory path.

## License

[BSD 2-Clause "Simplified" License](LICENSE).


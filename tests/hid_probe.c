/* Standalone HID probe for the MU3 controller.
 *
 * Run this on the machine the controller is plugged into, BEFORE trusting the
 * IO DLL. It opens VID 2341 / PID 8036 exactly the way the frozen DLL did
 * (VID/PID only, no report-length constraint), prints the descriptor, and dumps
 * raw reports so the real button/lever offsets and polarity can be read off
 * instead of guessed.
 *
 *   hid_probe.exe            list all HID devices (VID/PID + caps)
 *   hid_probe.exe dump [ms]  open the controller and dump reports for ms
 *   hid_probe.exe jitter [ms]
 *                            collect every report for ms while the lever is
 *                            left alone, then report the arrival rate and the
 *                            distribution of the lever field
 *   hid_probe.exe chord [ms] watch the ten button bytes and report, per key,
 *                            how many other keys were already held when it went
 *                            down. This is the device-side truth for a
 *                            "hold several keys, press one more" fault: run it
 *                            with the game closed and it is independent of both
 *                            the IO DLL and segatools.
 *
 * Press and release each button one at a time while dumping; the byte that
 * changes identifies that button. Move the lever to its extremes to get the
 * real ADC range for calibration. Leave the lever alone and run `jitter` to
 * measure how much the reading moves when nobody is touching it, which is the
 * number that decides whether a noise gate is worth having.
 *
 * This is how the operator buttons were settled: the rear Test-menu button
 * showed up as payload byte 23 (wire byte 24) reading 0x03, and the other rear
 * button as 0x04, both returning to 0x00 on release. Note that `dump` prints
 * wire bytes from the report ID onward, so the payload offset is one less than
 * the position in the printed line.
 */
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/button_map.h"

#define VID 0x2341
#define PID 0x8036
/* The lever field sits at frame payload offset 10, so wire bytes 11-12 once the
 * report ID byte at 0 is included. Every mode reads it from here. */
#define LEVER_WIRE_OFFSET 11
/* MU3_WIRE_WITH_ID (the length at which byte 0 is a Report ID rather than
 * payload) comes from src/button_map.h so the probe, the DLL and the offline
 * tests all share one definition of the report layout. */

static void print_caps(HANDLE h)
{
    PHIDP_PREPARSED_DATA prep = NULL;
    HIDP_CAPS caps = {0};
    if (HidD_GetPreparsedData(h, &prep) && HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
        printf("  usagePage=%#x usage=%#x in=%u out=%u feature=%u\n",
               caps.UsagePage, caps.Usage,
               caps.InputReportByteLength, caps.OutputReportByteLength,
               caps.FeatureReportByteLength);
    }
    if (prep) HidD_FreePreparsedData(prep);
}

static int list_devices(void)
{
    GUID guid;
    HDEVINFO set;
    DWORD index;
    int matches = 0;
    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { fprintf(stderr, "SetupDiGetClassDevs failed\n"); return 1; }
    for (index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA data = {0};
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        DWORD required = 0;
        HANDLE h;
        HIDD_ATTRIBUTES attrs = {0};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &guid, index, &data)) break;
        SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &required, NULL);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || required > 65536) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, NULL, NULL)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        }
        if (h != INVALID_HANDLE_VALUE) {
            attrs.Size = sizeof(attrs);
            if (HidD_GetAttributes(h, &attrs)) {
                int is_match = (attrs.VendorID == VID && attrs.ProductID == PID);
                if (is_match) matches++;
                printf("%sVID=%04X PID=%04X\n", is_match ? ">>> " : "    ",
                       attrs.VendorID, attrs.ProductID);
                printf("  path: %ls\n", detail->DevicePath);
                print_caps(h);
            }
            CloseHandle(h);
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    printf("\n%d device(s) match VID %04X PID %04X\n", matches, VID, PID);
    if (!matches) {
        puts("The controller was not found. Check it is plugged in and that the");
        puts("VID/PID in the firmware matches what the DLL expects.");
    }
    return 0;
}

/* Open the first VID/PID match and report its descriptor's input length.
 * Shared by every mode so they all open the device the same way. */
static HANDLE open_controller(size_t *in_len)
{
    GUID guid;
    HDEVINFO set;
    DWORD index;
    HANDLE found = INVALID_HANDLE_VALUE;
    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    for (index = 0; found == INVALID_HANDLE_VALUE; ++index) {
        SP_DEVICE_INTERFACE_DATA data = {0};
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        DWORD required = 0;
        HANDLE h;
        HIDD_ATTRIBUTES attrs = {0};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &guid, index, &data)) break;
        SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &required, NULL);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || required > 65536) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, NULL, NULL)) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                PHIDP_PREPARSED_DATA prep = NULL;
                HIDP_CAPS caps = {0};
                attrs.Size = sizeof(attrs);
                if (HidD_GetAttributes(h, &attrs) && attrs.VendorID == VID && attrs.ProductID == PID) {
                    if (HidD_GetPreparsedData(h, &prep) && HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                        *in_len = caps.InputReportByteLength;
                        printf("Opened controller, InputReportByteLength=%u\n", caps.InputReportByteLength);
                    }
                    if (prep) HidD_FreePreparsedData(prep);
                    found = h;
                } else {
                    CloseHandle(h);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

/* One blocking read with a bounded wait, matching the DLL's transfer path. */
/* 1 = a report arrived in buf/got, 0 = timed out, -1 = failure. */
static int read_report(HANDLE dev, uint8_t *buf, size_t len, DWORD *got, DWORD timeout_ms)
{
    OVERLAPPED ov = {0};
    BOOL ok;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    ok = ReadFile(dev, buf, (DWORD)len, got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(dev, &ov);
            GetOverlappedResult(dev, &ov, got, TRUE);
            CloseHandle(ov.hEvent);
            return 0;
        }
        ok = GetOverlappedResult(dev, &ov, got, FALSE);
    }
    CloseHandle(ov.hEvent);
    if (!ok || *got == 0) return -1;
    return 1;
}

/* Read the 16-bit lever field out of a wire report.
 *
 * Where the field sits depends on whether the report carries a Report ID byte,
 * and that is a property of the descriptor, not of how many bytes happened to
 * arrive: a 65-byte report puts the lever at wire bytes 11-12, a 64-byte report
 * at 10-11. Guessing from the returned length would silently read the wrong
 * bytes, so the descriptor length makes the decision. Reports too short to hold
 * the field are counted separately rather than read out of bounds. */
static int lever_from(const uint8_t *buf, DWORD got, size_t in_len, int *value)
{
    size_t at;
    if (in_len > got) return 0;           /* short read: field may be truncated */
    at = (in_len >= MU3_WIRE_WITH_ID) ? LEVER_WIRE_OFFSET
                                      : LEVER_WIRE_OFFSET - 1;
    if (got < at + 2) return 0;
    *value = (int)(uint16_t)(buf[at] | ((uint16_t)buf[at + 1] << 8));
    return 1;
}

/* How still is the lever when nobody is touching it?
 *
 * The deployed controller streams continuously (about 200 reports/s with the
 * lever untouched), so this measures the full distribution of the reading while
 * the stick is at rest, not a sample of "reports that only appeared because
 * something moved". It reports how often reports arrive and how far the reading
 * wanders, which is what decides whether a noise gate is worth having: a spread
 * of 0 or 1 counts means the reading is already quiet and a gate would only
 * suppress genuine slow movement.
 *
 * It also settles a question the DLL's setting depends on: what a hand-off
 * reading actually is. The stick is mechanical and cannot be parked exactly at
 * the electrical centre, so a resting reading carries a real offset from it and
 * must not be read as the centre or as drift. The observation that matters is
 * that the offset is stable and exactly reproducible while the stick is
 * untouched, which is what the spread and the histogram show.
 *
 * The histogram is centred on the nominal 1024 and covers +/-32 counts.
 * Anything outside that window is counted separately, because that is the lever
 * being moved rather than noise. */
#define JITTER_WINDOW 32
static int measure_jitter(int ms)
{
    size_t in_len = 65;
    HANDLE dev;
    ULONGLONG start, end;
    unsigned reports = 0, lever_reports = 0, unreadable = 0, changes = 0, outside = 0;
    int min = 0x10000, max = -1, previous = -1, last = -1;
    static unsigned counts[2 * JITTER_WINDOW + 1];
    int i;
    dev = open_controller(&in_len);
    if (dev == INVALID_HANDLE_VALUE) { fprintf(stderr, "Controller not found\n"); return 1; }
    if (in_len > 512 || in_len == 0) in_len = 65;

    puts("Leave the lever completely alone for the whole period.");
    puts("Do not rest a hand on it: the measurement is of the noise floor.");
    fflush(stdout);
    start = GetTickCount64();
    end = start + (ULONGLONG)ms;
    while (GetTickCount64() < end) {
        uint8_t buf[512];
        DWORD got = 0;
        int value;
        int result = read_report(dev, buf, in_len, &got, 500);
        if (result < 0) break;
        if (result == 0) continue;
        ++reports;
        if (!lever_from(buf, got, in_len, &value)) { ++unreadable; continue; }
        ++lever_reports;
        if (value < min) min = value;
        if (value > max) max = value;
        if (previous >= 0 && value != previous) ++changes;
        previous = value;
        last = value;
        if (value >= 1024 - JITTER_WINDOW && value <= 1024 + JITTER_WINDOW) {
            counts[value - 1024 + JITTER_WINDOW]++;
        } else {
            ++outside;
        }
    }
    CloseHandle(dev);

    printf("\n%u report(s) in %d ms\n", reports, ms);
    if (reports == 0) {
        puts("No reports arrived with the lever untouched, so the firmware does");
        puts("not resend unchanged state and there is no idle noise to gate.");
        return 0;
    }
    printf("  %.1f reports/s\n", reports * 1000.0 / (double)ms);
    if (lever_reports == 0) {
        printf("  none of them carried a lever field at offset %d (%u too short)\n",
               LEVER_WIRE_OFFSET, unreadable);
        return 0;
    }
    printf("  lever readings: %u\n", lever_reports);
    printf("  range: %d .. %d  (spread %d)\n", min, max, max - min);
    printf("  last value: %d\n", last);
    printf("  changes between consecutive reports: %u\n", changes);
    printf("  values seen with the stick untouched (offset from 1024):\n");
    for (i = 0; i < 2 * JITTER_WINDOW + 1; ++i) {
        if (counts[i]) printf("    %+4d: %u\n", i - JITTER_WINDOW, counts[i]);
    }
    if (outside) {
        printf("    outside +/-%d: %u (the lever was moved, not noise)\n",
               JITTER_WINDOW, outside);
    }
    printf("\n");
    if (max - min <= 1) {
        printf("Spread is %d counts: the reading is already quiet. A noise gate\n", max - min);
        puts("would not help and could suppress genuine slow movement.");
    } else {
        printf("Spread is %d counts. At the default sensitivity 2 that is %d units\n",
               max - min, (max - min) * 64);
        puts("of lever output of pure noise, against about 13696 for full left");
        puts("travel. A noise gate of about that spread would absorb it.");
    }
    if (last >= 0) {
        printf("\nResting value here is %d, i.e. %+d from the nominal centre 1024\n",
               last, last - 1024);
        puts("(about 64 units of lever output per count at sensitivity 2). This is");
        puts("NOT a drift measurement and NOT a calibration: the stick is mechanical");
        puts("and cannot be parked exactly at the electrical centre, so the offset is");
        puts("whatever position it happens to rest in. What the numbers above do show");
        puts("is that it is stable and reproducible while untouched.");
    }
    return 0;
}

/* One report's ten button bytes, read out of the wire layout.
 *
 * Delegates to src/button_map.c so the probe and the DLL share one definition
 * of where the field sits; a disagreement there would misread which key moved,
 * which looks exactly like a real input fault. */
static int buttons_from(const uint8_t *buf, DWORD got, size_t in_len,
                        uint8_t out[MU3_BUTTON_COUNT])
{
    return mu3_buttons_from_report(buf, got, in_len, out) ? 1 : 0;
}

/* Name a key by payload offset. The physical key behind each offset is still
 * unconfirmed (see src/button_map.h), so the census is reported by offset and
 * side rather than by a button name that might be wrong. */
static void key_name(unsigned key, char *out, size_t size)
{
    snprintf(out, size, "%s%d", key < MU3_BUTTONS_PER_SIDE ? "L" : "R",
             (int)(key % MU3_BUTTONS_PER_SIDE) + 1);
}

/* Device-side truth for a combination-dependent key fault.
 *
 * The question this answers: when several keys are held and one more is pressed,
 * does the controller report the new key at all? It reads the ten button bytes
 * straight off the wire, so it is independent of the IO DLL, of segatools and of
 * the game. A key counted at "4 others held" proves the device reported it under
 * that combination, and the fault is therefore downstream; a key never counted
 * there while the user is certain they pressed it means the controller's own key
 * scanning is dropping it, which no DLL change can fix.
 *
 * Run it with the game closed: the DLL opens the device for shared read/write,
 * so both could otherwise consume the same reports. */
static int chord_census(int ms)
{
    size_t in_len = 65;
    HANDLE dev;
    ULONGLONG start, end;
    unsigned reports = 0, unreadable = 0;
    mu3_chord_census census;
    mu3_chord_step step;
    uint8_t buttons[MU3_BUTTON_COUNT];
    char name[8];

    memset(&census, 0, sizeof(census));
    dev = open_controller(&in_len);
    if (dev == INVALID_HANDLE_VALUE) { fprintf(stderr, "Controller not found\n"); return 1; }
    if (in_len > 512 || in_len == 0) in_len = 65;

    puts("Chord census on the ten button bytes.");
    puts("");
    puts("Reproduce the gesture you are unsure about, deliberately and SLOWLY,");
    puts("many times over. Two things separate a controller fault from a timing");
    puts("slip: a slip disappears when you slow down, and it does not care which");
    puts("keys are involved.");
    puts("");
    puts("  1. Hold four keys (the same four each time), then press a fifth while");
    puts("     releasing one of the four. Repeat ten or more times.");
    puts("  2. Then hold the same four and press the fifth with nothing released.");
    puts("  3. Also try pressing the fifth alone, from rest, as a control.");
    puts("");
    puts("Every change is printed as it happens. Ctrl+C stops early.");
    printf("Recording for %d ms...\n\n", ms);
    fflush(stdout);

    start = GetTickCount64();
    end = start + (ULONGLONG)ms;
    while (GetTickCount64() < end) {
        uint8_t buf[512];
        DWORD got = 0;
        int result = read_report(dev, buf, in_len, &got, 500);
        if (result < 0) break;
        if (result == 0) continue;
        ++reports;
        if (!buttons_from(buf, got, in_len, buttons)) { ++unreadable; continue; }
        if (!mu3_chord_update(&census, buttons, &step)) continue;

        /* Print every change, including one where the bytes moved but the
         * decoded mask did not; that shape would falsify the polarity model. */
        printf("[%6llu] held %u -> %u ",
               (unsigned long long) GetTickCount64(), step.held_before,
               step.held_after);
        if (step.down) {
            unsigned k;
            printf(" DOWN:");
            for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
                if (step.down & (1u << k)) {
                    key_name(k, name, sizeof(name));
                    printf(" %s", name);
                }
            }
        }
        if (step.up) {
            unsigned k;
            printf(" UP:");
            for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
                if (step.up & (1u << k)) {
                    key_name(k, name, sizeof(name));
                    printf(" %s", name);
                }
            }
        }
        if (!step.mask_changed) printf(" (bytes changed, decoded mask did not)");
        printf("  bytes:");
        {
            unsigned k;
            for (k = 0; k < MU3_BUTTON_COUNT; ++k) printf(" %02X", buttons[k]);
        }
        printf("\n");
        fflush(stdout);
    }
    CloseHandle(dev);

    printf("\n%u report(s) read", reports);
    if (unreadable) printf(", %u too short to hold the button field", unreadable);
    printf("\n%u change(s) in the button bytes\n", census.raw_changes);

    if (census.raw_changes == 0) {
        puts("\nNo button byte ever changed. Either no key was pressed, or the");
        puts("button field is not at payload offsets 0..9 on this device. Press");
        puts("one key at a time and re-run `dump` to find which bytes move.");
        return 0;
    }

    puts("\nPer-key census (L=left, R=right, by payload offset; a physical");
    puts("key name is not assumed because the mapping is still unconfirmed):");
    printf("  %-4s %8s %8s  %s\n", "key", "presses", "releases",
           "presses by how many other keys were already held");
    {
        unsigned k, n;
        int any_high = 0;
        for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
            if (!census.presses[k] && !census.releases[k]) continue;
            key_name(k, name, sizeof(name));
            printf("  %-4s %8u %8u  ", name, census.presses[k],
                   census.releases[k]);
            for (n = 0; n < MU3_BUTTON_COUNT; ++n) {
                if (census.presses_with_others[k][n]) {
                    printf("%u held:%u  ", n, census.presses_with_others[k][n]);
                    if (n >= 4) any_high = 1;
                }
            }
            printf("\n");
        }
        if (!any_high) {
            puts("\n  No key was ever recorded going down while four or more");
            puts("  others were held.");
        }
    }

    printf("\nMost keys held at once: %u\n", census.max_held);
    puts("\nByte values seen per key (a binary device shows only 00 and one");
    puts("other value; a third distinct value means the byte is not a switch):");
    {
        unsigned k, v;
        for (k = 0; k < MU3_BUTTON_COUNT; ++k) {
            if (census.value_count[k] == 0) continue;
            key_name(k, name, sizeof(name));
            printf("  %-4s:", name);
            for (v = 0; v < census.value_count[k] && v < MU3_CHORD_VALUES_MAX; ++v) {
                printf(" %02X", census.values_seen[k][v]);
            }
            printf("\n");
        }
    }

    puts("\nHow to read this:");
    puts("  * The key you pressed while four others were held appears with a");
    puts("    count under \"4 held\" -> the controller reported it. The wire is");
    puts("    not losing your press; look downstream (DLL/segatools/game).");
    puts("  * It never appears there while you are certain you pressed it, and");
    puts("    the same specific combination keeps failing at slow speed -> the");
    puts("    controller's own key scanning is dropping it. That is a matrix");
    puts("    ghosting/jamming signature and no DLL change can fix it.");
    puts("  * Failures that move around between combinations, or that vanish");
    puts("    when you slow down, look like human timing rather than the device.");
    puts("  * A key showing a third distinct byte value, or a change printed");
    puts("    with no DOWN/UP at all, points at the polarity/layout hypothesis");
    puts("    rather than at dropped input.");
    return 0;
}

static int dump_reports(int ms)
{
    size_t in_len = 65;
    HANDLE found;
    ULONGLONG end;
    int printed = 0;
    uint8_t prev[512];
    memset(prev, 0, sizeof(prev));
    found = open_controller(&in_len);
    if (found == INVALID_HANDLE_VALUE) { fprintf(stderr, "Controller not found\n"); return 1; }
    if (in_len > sizeof(prev) || in_len == 0) in_len = 65;

    puts("Dumping reports. Press/release ONE button at a time, then move the");
    puts("lever to both extremes. Ctrl+C to stop early.");
    end = GetTickCount64() + (ULONGLONG)ms;
    while (GetTickCount64() < end) {
        uint8_t buf[512];
        DWORD got = 0;
        int result;
        memset(buf, 0, sizeof(buf));
        result = read_report(found, buf, in_len, &got, 500);
        if (result < 0) break;
        if (result == 0) continue;
        if (memcmp(buf, prev, got) == 0) continue; /* print only changes */
        memcpy(prev, buf, got);
        printf("[%6llu] %3lu bytes:", (unsigned long long) GetTickCount64(), (unsigned long) got);
        for (DWORD i = 0; i < got && i < 32; ++i) printf(" %02X", buf[i]);
        printf("\n");
        fflush(stdout);
        printed++;
    }
    CloseHandle(found);
    printf("\n%d distinct report(s) captured.\n", printed);
    if (!printed) {
        puts("No reports arrived. The device may be report-driven only on change,");
        puts("or it may need an initial output/feature write to start streaming.");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ms = 20000;
    if (argc >= 2 && _stricmp(argv[1], "dump") == 0) {
        if (argc >= 3) ms = atoi(argv[2]);
        return dump_reports(ms > 0 ? ms : 20000);
    }
    if (argc >= 2 && _stricmp(argv[1], "jitter") == 0) {
        if (argc >= 3) ms = atoi(argv[2]);
        return measure_jitter(ms > 0 ? ms : 20000);
    }
    if (argc >= 2 && _stricmp(argv[1], "chord") == 0) {
        if (argc >= 3) ms = atoi(argv[2]);
        return chord_census(ms > 0 ? ms : 60000);
    }
    puts("MU3 HID probe\n");
    return list_devices();
}

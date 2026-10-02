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
 *
 * Press and release each button one at a time while dumping; the byte that
 * changes identifies that button. Move the lever to its extremes to get the
 * real ADC range for calibration.
 */
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VID 0x2341
#define PID 0x8036

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

static int dump_reports(int ms)
{
    GUID guid;
    HDEVINFO set;
    DWORD index;
    HANDLE found = INVALID_HANDLE_VALUE;
    size_t in_len = 65;
    ULONGLONG end;
    int printed = 0;
    uint8_t prev[512];
    memset(prev, 0, sizeof(prev));
    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 1;
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
                        in_len = caps.InputReportByteLength;
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
    if (found == INVALID_HANDLE_VALUE) { fprintf(stderr, "Controller not found\n"); return 1; }
    if (in_len > sizeof(prev) || in_len == 0) in_len = 65;

    puts("Dumping reports. Press/release ONE button at a time, then move the");
    puts("lever to both extremes. Ctrl+C to stop early.");
    end = GetTickCount64() + (ULONGLONG)ms;
    while (GetTickCount64() < end) {
        OVERLAPPED ov = {0};
        uint8_t buf[512];
        DWORD got = 0;
        BOOL ok;
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) break;
        memset(buf, 0, sizeof(buf));
        ok = ReadFile(found, buf, (DWORD)in_len, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0) {
                CancelIoEx(found, &ov);
                GetOverlappedResult(found, &ov, &got, TRUE);
                CloseHandle(ov.hEvent);
                continue;
            }
            ok = GetOverlappedResult(found, &ov, &got, FALSE);
        }
        CloseHandle(ov.hEvent);
        if (!ok || got == 0) continue;
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
    puts("MU3 HID probe\n");
    return list_devices();
}

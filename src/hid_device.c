#include "hid_device.h"
#include <hidsdi.h>
#include <setupapi.h>
#include <string.h>

#define VID 0x2341
#define PID 0x8036
#define RETRY_MS 500
/* Matches the frozen DLL's Receive(..., timeout=1000). Idle reads timing out is
 * normal for a change-triggered controller and must not invalidate input. */
#define IO_TIMEOUT_MS 1000

static bool stopping(mu3_hid_device *dev) { return WaitForSingleObject(dev->stop, 0) == WAIT_OBJECT_0; }

/* The frozen, cabinet-tested DLL called Open(1, VID, PID, -1, -1): it filtered
 * on VID/PID and top-level usage only (the -1s disable the usage checks) and
 * never constrained report lengths. Requiring 65/65 here would reject a device
 * that works fine, giving "DLL loads but no input at all". So a VID/PID match
 * with valid caps is accepted, and a 65/65 interface is merely preferred. */
typedef struct candidate {
    HANDLE handle;
    size_t in_len, out_len;
} candidate;

static bool open_interface(const wchar_t *path, candidate *out)
{
    HANDLE handle;
    HIDD_ATTRIBUTES attrs = {0};
    PHIDP_PREPARSED_DATA prep = NULL;
    HIDP_CAPS caps = {0};
    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    attrs.Size = sizeof(attrs);
    if (HidD_GetAttributes(handle, &attrs) && attrs.VendorID == VID && attrs.ProductID == PID &&
        HidD_GetPreparsedData(handle, &prep) && HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
        caps.InputReportByteLength > 0 && caps.InputReportByteLength <= MU3_HID_MAX_REPORT &&
        caps.OutputReportByteLength > 0 && caps.OutputReportByteLength <= MU3_HID_MAX_REPORT) {
        HidD_FreePreparsedData(prep);
        out->handle = handle;
        out->in_len = caps.InputReportByteLength;
        out->out_len = caps.OutputReportByteLength;
        return true;
    }
    if (prep) HidD_FreePreparsedData(prep);
    CloseHandle(handle);
    return false;
}

/* Returns a handle plus the descriptor's transfer lengths, preferring an exact
 * 65/65 interface but never rejecting an otherwise valid VID/PID match. */
static HANDLE find_device(size_t *in_len, size_t *out_len)
{
    GUID guid;
    HDEVINFO set;
    DWORD index;
    candidate fallback = {INVALID_HANDLE_VALUE, 0, 0};
    candidate exact = {INVALID_HANDLE_VALUE, 0, 0};
    HidD_GetHidGuid(&guid);
    set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    for (index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA data = {0};
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        DWORD required = 0;
        candidate found;
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &guid, index, &data)) break;
        SetupDiGetDeviceInterfaceDetailW(set, &data, NULL, 0, &required, NULL);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || required > 65536) continue;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, NULL, NULL) &&
            open_interface(detail->DevicePath, &found)) {
            if (found.in_len == MU3_HID_WIRE && found.out_len == MU3_HID_WIRE) {
                if (exact.handle == INVALID_HANDLE_VALUE) exact = found;
                else CloseHandle(found.handle);
            } else if (fallback.handle == INVALID_HANDLE_VALUE) {
                fallback = found;
            } else {
                CloseHandle(found.handle);
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (exact.handle != INVALID_HANDLE_VALUE) {
        if (fallback.handle != INVALID_HANDLE_VALUE) CloseHandle(fallback.handle);
        *in_len = exact.in_len;
        *out_len = exact.out_len;
        return exact.handle;
    }
    if (fallback.handle != INVALID_HANDLE_VALUE) {
        *in_len = fallback.in_len;
        *out_len = fallback.out_len;
    }
    return fallback.handle;
}

/* Every path drains pending OVERLAPPED I/O before local buffer/event lifetime ends. */
/* 1 = complete, 0 = read timeout, -1 = transport failure or shutdown. */
static int transfer(mu3_hid_device *dev, HANDLE handle, bool write,
                    uint8_t *buffer, size_t want, size_t *count)
{
    OVERLAPPED ov = {0};
    BOOL ok;
    DWORD done = 0;
    DWORD waited;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    ok = write ? WriteFile(handle, buffer, (DWORD)want, &done, &ov)
               : ReadFile(handle, buffer, (DWORD)want, &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE events[] = {dev->stop, ov.hEvent};
        waited = WaitForMultipleObjects(2, events, FALSE, IO_TIMEOUT_MS);
        if (waited != WAIT_OBJECT_0 + 1) {
            CancelIoEx(handle, &ov);
            GetOverlappedResult(handle, &ov, &done, TRUE);
            CloseHandle(ov.hEvent);
            return waited == WAIT_TIMEOUT && !write ? 0 : -1;
        }
        ok = GetOverlappedResult(handle, &ov, &done, FALSE);
    }
    CloseHandle(ov.hEvent);
    if (!ok || done == 0) return -1;
    if (count) *count = done;
    return 1;
}

/* Normalise a wire report into the 65-byte frame the core parses: byte 0 is the
 * report ID, bytes 1..64 the payload. This is exactly what the frozen DLL did
 * (ReadFile into a 65-byte buffer, then skip byte 0 and marshal the remaining 64
 * bytes as OutputData). A device that reports no report ID is shifted instead. */
static void normalise(const uint8_t *raw, size_t got, uint8_t frame[MU3_HID_WIRE])
{
    memset(frame, 0, MU3_HID_WIRE);
    if (got >= MU3_HID_WIRE) {
        frame[0] = raw[0];
        memcpy(frame + 1, raw + 1, MU3_HID_PAYLOAD);
    } else {
        frame[0] = 0;
        memcpy(frame + 1, raw, got < MU3_HID_PAYLOAD ? got : MU3_HID_PAYLOAD);
    }
}

static DWORD WINAPI worker(void *context)
{
    mu3_hid_device *dev = (mu3_hid_device *) context;
    while (!stopping(dev)) {
        HANDLE handle;
        size_t in_len = 0, out_len = 0;
        if (dev->tick) dev->tick(dev->ctx);
        handle = find_device(&in_len, &out_len);
        if (handle == INVALID_HANDLE_VALUE) { WaitForSingleObject(dev->stop, RETRY_MS); continue; }
        dev->in_len = in_len;
        dev->out_len = out_len;
        dev->state(dev->ctx, true);
        while (!stopping(dev)) {
            uint8_t raw[MU3_HID_MAX_REPORT];
            uint8_t frame[MU3_HID_WIRE];
            uint8_t output[MU3_HID_WIRE];
            bool pending;
            size_t count = 0;
            if (dev->tick) dev->tick(dev->ctx);
            AcquireSRWLockExclusive(&dev->queue_lock);
            pending = dev->pending;
            if (pending) { memcpy(output, dev->queued, sizeof(output)); dev->pending = false; }
            ReleaseSRWLockExclusive(&dev->queue_lock);
            /* Writes use the descriptor's output length; the queued frame is the
             * frozen 65-byte layout with report ID 0 at byte 0. */
            if (pending && transfer(dev, handle, true, output,
                                    out_len <= sizeof(output) ? out_len : sizeof(output), &count) != 1) break;
            {
                int result = transfer(dev, handle, false, raw, in_len, &count);
                if (result < 0 || stopping(dev)) break;
                if (result == 0) continue; /* Idle read: never expire held input. */
            }
            normalise(raw, count, frame);
            dev->frame(dev->ctx, frame, MU3_HID_WIRE, GetTickCount64());
        }
        dev->state(dev->ctx, false);
        CloseHandle(handle);
        dev->in_len = dev->out_len = 0;
        AcquireSRWLockExclusive(&dev->queue_lock);
        dev->pending = false;
        ReleaseSRWLockExclusive(&dev->queue_lock);
        if (!stopping(dev)) WaitForSingleObject(dev->stop, RETRY_MS);
    }
    return 0;
}

bool mu3_hid_start(mu3_hid_device *dev, void *ctx, mu3_hid_frame_fn frame, mu3_hid_state_fn state, void (*tick)(void *))
{
    if (!dev || !frame || !state) return false;
    memset(dev, 0, sizeof(*dev));
    InitializeSRWLock(&dev->queue_lock);
    dev->ctx = ctx; dev->frame = frame; dev->state = state; dev->tick = tick;
    dev->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!dev->stop) return false;
    dev->thread = CreateThread(NULL, 0, worker, dev, 0, NULL);
    if (!dev->thread) { CloseHandle(dev->stop); dev->stop = NULL; return false; }
    return true;
}

void mu3_hid_stop(mu3_hid_device *dev)
{
    if (!dev || !dev->thread) return;
    SetEvent(dev->stop);
    WaitForSingleObject(dev->thread, INFINITE);
    CloseHandle(dev->thread); CloseHandle(dev->stop);
    dev->thread = NULL; dev->stop = NULL;
}

void mu3_hid_queue(mu3_hid_device *dev, const uint8_t report[MU3_HID_WIRE])
{
    if (!dev || !report || !dev->thread) return;
    AcquireSRWLockExclusive(&dev->queue_lock);
    memcpy(dev->queued, report, MU3_HID_WIRE);
    dev->pending = true;
    ReleaseSRWLockExclusive(&dev->queue_lock);
}

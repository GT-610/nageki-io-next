#ifndef MU3_HID_DEVICE_H
#define MU3_HID_DEVICE_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
/* Reports are at most 65 bytes on the wire (Report ID + 64-byte payload) for
 * this controller, but the descriptor decides the exact transfer length, so
 * transfers are sized from HIDP_CAPS rather than assumed. */
#define MU3_HID_MAX_REPORT 512u
#define MU3_HID_PAYLOAD 64u
#define MU3_HID_WIRE 65u
typedef void (*mu3_hid_frame_fn)(void *, const uint8_t *, size_t, uint64_t);
typedef void (*mu3_hid_state_fn)(void *, bool);
typedef struct mu3_hid_device {
    HANDLE thread, stop;
    SRWLOCK queue_lock;
    uint8_t queued[MU3_HID_WIRE];
    bool pending;
    void *ctx;
    mu3_hid_frame_fn frame;
    mu3_hid_state_fn state;
    void (*tick)(void *);
    /* Lengths reported by the opened device's descriptor. */
    size_t in_len, out_len;
} mu3_hid_device;
bool mu3_hid_start(mu3_hid_device *dev, void *ctx, mu3_hid_frame_fn frame, mu3_hid_state_fn state, void (*tick)(void *));
/* Stop only during orderly teardown; never from DllMain. */
void mu3_hid_stop(mu3_hid_device *dev);
/* Coalesce to the most recent color frame; never blocks on USB. */
void mu3_hid_queue(mu3_hid_device *dev, const uint8_t report[MU3_HID_WIRE]);
#endif

#ifndef MU3_HID_DEVICE_H
#define MU3_HID_DEVICE_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
/* The frame this DLL exchanges with the core is always 65 bytes: a Report ID
 * byte followed by a 64-byte payload. That is a fixed internal layout; the
 * device's descriptor decides how many bytes actually cross the wire, so
 * transfers are sized from HIDP_CAPS and marshalled at the boundary. */
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
} mu3_hid_device;
bool mu3_hid_start(mu3_hid_device *dev, void *ctx, mu3_hid_frame_fn frame, mu3_hid_state_fn state, void (*tick)(void *));
/* Coalesce to the most recent color frame; never blocks on USB. */
void mu3_hid_queue(mu3_hid_device *dev, const uint8_t report[MU3_HID_WIRE]);
/* Marshalling between the fixed 65-byte frame and a descriptor's transfer
 * length. A device whose reports carry no Report ID uses length 64, so the
 * frame's ID byte is dropped on write and re-supplied as 0 on read. Pure
 * functions so the mapping can be tested without a device. */
void mu3_hid_pack(const uint8_t frame[MU3_HID_WIRE], uint8_t *wire, size_t wire_len);
void mu3_hid_unpack(const uint8_t *wire, size_t got, uint8_t frame[MU3_HID_WIRE]);
#endif

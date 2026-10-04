#include "../src/io_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void neutral(const mu3_sample *s)
{
    const mu3_sample zero = {0};
    assert(memcmp(s, &zero, sizeof(zero)) == 0);
}

int main(void)
{
    mu3_core core;
    mu3_sample sample;
    mu3_health health;
    uint8_t frame[MU3_REPORT_SIZE] = {0};

    assert(!mu3_core_init(&core, 256));
    assert(mu3_core_init(&core, 0));
    assert(!mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_NO_DEVICE);
    neutral(&sample);
    assert(!mu3_core_publish(&core, frame, sizeof(frame), 100));
    mu3_core_open(&core);
    assert(!mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_WAITING_FOR_FRAME);
    frame[0] = 1;
    assert(!mu3_core_publish(&core, frame, sizeof(frame), 100)); /* wrong ID */
    frame[0] = 0;
    frame[1] = frame[3] = frame[5] = 1;
    frame[6] = frame[10] = 1;
    frame[11] = 0x34;
    frame[12] = 0x12;
    frame[13] = 1;
    frame[14] = 0xAA;
    frame[24] = 4;
    assert(!mu3_core_publish(&core, frame, sizeof(frame) - 1, 100)); /* short */
    assert(mu3_core_publish(&core, frame, sizeof(frame), 100));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_FRESH && sample.sequence == 1);
    assert(sample.left == 0x15 && sample.right == 0x11);
    assert(sample.raw_lever == 0x1234 && sample.scan == 1);
    assert(sample.card[0] == 0xAA && sample.operator_buttons == 4);

    /* REGRESSION: a held report must survive an arbitrarily long quiet period.
     * A change-triggered controller sends nothing while nothing moves; expiring
     * on a timer would silently release held buttons. The reported frame still
     * carries its original timestamp, which is what an age-based check would
     * have tripped over. */
    assert(sample.received_ms == 100);
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_FRESH && sample.left == 0x15 && sample.sequence == 1);
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x15);

    frame[1] = 2; /* nonzero counts as pressed, as the frozen DLL assumed */
    assert(mu3_core_publish(&core, frame, sizeof(frame), 101));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x15 && sample.sequence == 2);
    frame[1] = 1;
    assert(!mu3_core_publish(&core, frame, sizeof(frame), 99)); /* older stamp */
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.sequence == 2);
    frame[13] = 3; /* out-of-range scan means "no card", not a dropped frame */
    assert(mu3_core_publish(&core, frame, sizeof(frame), 110));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.sequence == 3 && sample.scan == 0);
    assert(sample.left == 0x15); /* buttons survive the unknown scan value */
    frame[13] = 1;
    assert(mu3_core_publish(&core, frame, sizeof(frame), 111));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.sequence == 4 && sample.scan == 1);

    /* 0xFF button bytes must still produce input. */
    memset(frame, 0, sizeof(frame));
    frame[1] = frame[6] = 0xFF;
    assert(mu3_core_publish(&core, frame, sizeof(frame), 112));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x01 && sample.right == 0x01);

    mu3_core_disconnect(&core);
    assert(!mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_DISCONNECTED);
    neutral(&sample);
    assert(!mu3_core_publish(&core, frame, sizeof(frame), 113));

    /* Reconnect must not replay the old sample and restarts sequence numbering. */
    mu3_core_open(&core);
    assert(!mu3_core_snapshot(&core, &sample, &health));
    neutral(&sample);
    memset(frame, 0, sizeof(frame));
    assert(mu3_core_publish(&core, frame, sizeof(frame), 114));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0 && sample.scan == 0 && sample.sequence == 1);

    /* report_id == -1 accepts any report ID until a descriptor is captured. */
    assert(mu3_core_init(&core, -1));
    mu3_core_open(&core);
    frame[0] = 7;
    assert(mu3_core_publish(&core, frame, sizeof(frame), 200));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.sequence == 1);

    puts("offline IO core tests passed");
    return 0;
}

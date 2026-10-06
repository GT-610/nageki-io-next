#include "../src/io_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void neutral(const mu3_sample *s)
{
    const mu3_sample zero = {0};
    assert(memcmp(s, &zero, sizeof(zero)) == 0);
}

/* The cross-process serving policy. The case that matters: a copy that raced
 * the owner's write must NOT be answered with neutral, because that releases
 * every held button for one poll. It must serve the previous consistent sample.
 * The opposite case matters just as much: a departed owner must lose its cache,
 * so a held button cannot be replayed after the process is gone. */
static void test_sample_cache(void)
{
    mu3_sample_cache cache;
    mu3_sample a = {0}, b = {0}, out = {0};
    mu3_sample_source source;

    memset(&cache, 0, sizeof(cache));
    a.left = 0x0F;
    a.right = 0x01;
    a.received_ms = 10;
    b.left = 0x10;
    b.received_ms = 20;

    /* Nothing has been read yet: a transient with an empty cache has nothing to
     * serve, so neutral is correct here. */
    source = mu3_sample_cache_apply(&cache, MU3_READ_RETRY, NULL, &out);
    assert(source == MU3_SAMPLE_NONE);
    neutral(&out);

    source = mu3_sample_cache_apply(&cache, MU3_READ_OK, &a, &out);
    assert(source == MU3_SAMPLE_FRESH);
    assert(out.left == 0x0F && out.right == 0x01);

    /* The held keys survive a torn copy. */
    memset(&out, 0, sizeof(out));
    source = mu3_sample_cache_apply(&cache, MU3_READ_RETRY, NULL, &out);
    assert(source == MU3_SAMPLE_CACHED);
    assert(out.left == 0x0F && out.right == 0x01 && out.received_ms == 10);

    /* A newer consistent read replaces the cache. */
    source = mu3_sample_cache_apply(&cache, MU3_READ_OK, &b, &out);
    assert(source == MU3_SAMPLE_FRESH && out.left == 0x10);
    source = mu3_sample_cache_apply(&cache, MU3_READ_RETRY, NULL, &out);
    assert(source == MU3_SAMPLE_CACHED && out.left == 0x10);

    /* The owner is gone: neutral, and the cache is dropped so the next
     * transient cannot resurrect the dead owner's buttons. */
    source = mu3_sample_cache_apply(&cache, MU3_READ_GONE, NULL, &out);
    assert(source == MU3_SAMPLE_NONE);
    neutral(&out);
    assert(!cache.have_last);
    source = mu3_sample_cache_apply(&cache, MU3_READ_RETRY, NULL, &out);
    assert(source == MU3_SAMPLE_NONE);
    neutral(&out);

    /* Degenerate arguments must not crash or leave a stale cache behind. */
    source = mu3_sample_cache_apply(NULL, MU3_READ_OK, &a, &out);
    assert(source == MU3_SAMPLE_NONE);
    source = mu3_sample_cache_apply(&cache, MU3_READ_OK, &a, NULL);
    assert(source == MU3_SAMPLE_NONE);
    source = mu3_sample_cache_apply(&cache, MU3_READ_OK, NULL, &out);
    assert(source == MU3_SAMPLE_NONE);
    assert(!cache.have_last);
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
    assert(health == MU3_FRESH);
    assert(sample.left == 0x15 && sample.right == 0x11);
    assert(sample.raw_lever == 0x1234 && sample.scan == 1);
    assert(sample.card[0] == 0xAA && sample.operator_buttons == 4);

    /* REGRESSION: a held report must survive an arbitrarily long quiet period.
     * A newer report is the only thing allowed to supersede one, because a
     * button or a lever-at-stop must stay in effect while it is held. The held
     * frame still carries its original timestamp, which is what an age-based
     * check would have tripped over. (The deployed controller actually streams
     * at about 200 reports/s, so this guards the rule rather than the common
     * path.) */
    assert(sample.received_ms == 100);
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(health == MU3_FRESH && sample.left == 0x15);
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x15);

    frame[1] = 2; /* nonzero counts as pressed, as the frozen DLL assumed */
    assert(mu3_core_publish(&core, frame, sizeof(frame), 101));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x15);
    frame[1] = 1;
    /* A report older than the held one must not replace it, so the sample is
     * still the frame published at 101. */
    assert(!mu3_core_publish(&core, frame, sizeof(frame), 99)); /* older stamp */
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0x15);
    frame[13] = 3; /* out-of-range scan means "no card", not a dropped frame */
    assert(mu3_core_publish(&core, frame, sizeof(frame), 110));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.scan == 0);
    assert(sample.left == 0x15); /* buttons survive the unknown scan value */
    frame[13] = 1;
    assert(mu3_core_publish(&core, frame, sizeof(frame), 111));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.scan == 1);

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

    /* Reconnect must not replay the old sample. */
    mu3_core_open(&core);
    assert(!mu3_core_snapshot(&core, &sample, &health));
    neutral(&sample);
    memset(frame, 0, sizeof(frame));
    assert(mu3_core_publish(&core, frame, sizeof(frame), 114));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.left == 0 && sample.scan == 0);

    /* report_id == -1 accepts any report ID. The real wire ID is 0x00, but the
     * header-vs-payload decision belongs to mu3_hid_unpack, so no ID test is
     * enforced here; see io_core.h. */
    assert(mu3_core_init(&core, -1));
    mu3_core_open(&core);
    frame[0] = 7;
    assert(mu3_core_publish(&core, frame, sizeof(frame), 200));
    assert(mu3_core_snapshot(&core, &sample, &health));
    assert(sample.scan == 0);

    test_sample_cache();

    puts("offline IO core tests passed");
    return 0;
}

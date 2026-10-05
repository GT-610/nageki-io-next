/* Analyze a button trace produced by MU3CustomIO.dll (MU3CustomIO-wire.log and
 * MU3CustomIO-served.log) and report, per key, the evidence for a bouncing
 * contact.
 *
 * The measurement: a healthy switch produces one DOWN and later one UP per
 * actuation. A worn or dirty switch does not fail cleanly - the contact opens
 * for a few milliseconds in the middle of a hold - so it produces DOWN, UP,
 * DOWN in quick succession. That pattern is invisible in any aggregate "did the
 * key register" count, which is why a key can feel unreliable while every
 * summary says it works.
 *
 * Usage:
 *   trace_analyze.exe <log> [more.log ...]
 *
 * Reporting the count alone would be misleading, so the two normalisations that
 * matter are printed alongside it:
 *
 *   Per press rate: clicks / presses. A key that is played twice as often as
 *   another will show twice the clicks for the same defect rate, so the raw
 *   count cannot compare keys. This can: "L1 clicks on 0.8% of its presses" is
 *   comparable with "R2 clicks on 0.7% of its presses".
 *
 *   Short-release share: of the releases on a key, how many were shorter than
 *   the threshold. A human releasing a key for 3 ms is not doing it on purpose;
 *   this is the part of the click count that is mechanical rather than playing.
 *
 * The decisive one for a single key, though, is not any of these - it is the
 * control. If L1 clicks and L4 does not, on similar press counts, that is a
 * difference between two switches under the same player. If every key clicks at
 * a similar rate, the common factor is the player, not the hardware.
 *
 * Reads one or more logs. Passing several files (for example a wire and a
 * served log from the same session) pools them, which is what the comparison
 * against the software path needs. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 10
static const char *key_names[MAX_KEYS] = {"L1", "L2", "L3", "L4", "L5",
                                          "R1", "R2", "R3", "R4", "R5"};
/* A release shorter than this, immediately followed by the same key going down
 * again, is counted as a click. Human release-and-repress is far slower; a
 * bouncing contact is single-digit milliseconds. */
#define DEFAULT_CLICK_US 15000u
/* A press shorter than this is not a deliberate hit on a rhythm game. Not
 * counted as a click (nothing was held to break contact in), but reported. */
#define DEFAULT_MIN_HOLD_US 5000u

typedef struct key_stats {
    uint64_t presses;
    uint64_t releases;
    uint64_t clicks;            /* short up followed by the same key down */
    uint64_t min_hold_us;       /* shortest complete press-release */
    uint64_t max_hold_us;
    uint64_t hold_sum_us;
    uint64_t holds;
    uint64_t short_releases;    /* releases whose preceding hold was short */
    uint64_t bytes_nonbinary;   /* changes where this key's byte was not 00/01 */
    uint8_t values_seen[8];
    unsigned value_count;
} key_stats;

static int key_index(const char *name)
{
    /* Accept L1..L5 / R1..R5 case-insensitively. */
    char c = name[0];
    int n = name[1] - '0';
    if (n < 1 || n > 5) return -1;
    if (c == 'L' || c == 'l') return n - 1;
    if (c == 'R' || c == 'r') return n + 4;
    return -1;
}

static void note_value(key_stats *stats, uint8_t value)
{
    unsigned i;
    if (stats == NULL) return;
    for (i = 0; i < stats->value_count; ++i) {
        if (stats->values_seen[i] == value) return;
    }
    if (stats->value_count < sizeof(stats->values_seen)) {
        stats->values_seen[stats->value_count++] = value;
    }
}

/* Parse "down=L1,R3" / "up=-" into a bitmask. Returns -1 on an unrecognised
 * token, so a malformed line is reported rather than silently counted as
 * "no keys". */
static int parse_keys(const char *text)
{
    int mask = 0;
    while (*text != '\0' && *text != ' ' && *text != '\n') {
        int index;
        if (*text == '-') return mask;
        index = key_index(text);
        if (index < 0) return -1;
        mask |= 1 << index;
        text += 2;
        if (*text == ',') ++text;
    }
    return mask;
}

/* Parse the ten hex byte columns after "bytes=". Leading whitespace is
 * tolerated: these logs are plain text that an operator may open and edit, and
 * a stray space should not turn a real session into "malformed". */
static int parse_bytes(const char *text, uint8_t out[MAX_KEYS])
{
    unsigned count = 0;
    while (*text == ' ' || *text == '\t') ++text;
    while (count < MAX_KEYS && text[0] != '\0' && text[0] != '\n' &&
           text[0] != ' ' && text[0] != '\t') {
        unsigned long value;
        char *end;
        value = strtoul(text, &end, 16);
        if (end == text) return -1;
        out[count++] = (uint8_t) value;
        text = end;
        while (*text == ' ' || *text == '\t') ++text;
    }
    return (int) count;
}

typedef struct pending {
    uint64_t down_us[MAX_KEYS]; /* 0 = not currently held */
    uint64_t last_up_us[MAX_KEYS];
    bool click_candidate[MAX_KEYS];
} pending;

int main(int argc, char **argv)
{
    key_stats stats[MAX_KEYS];
    pending state;
    uint64_t click_us = DEFAULT_CLICK_US;
    uint64_t min_hold_us = DEFAULT_MIN_HOLD_US;
    uint64_t lines = 0, changes = 0, bad_lines = 0, inconsistent_lines = 0;
    uint64_t first_us = 0, last_us = 0;
    uint8_t previous_bytes[MAX_KEYS];
    bool have_previous = false;
    int arg;
    int i;

    memset(stats, 0, sizeof(stats));
    memset(&state, 0, sizeof(state));
    memset(previous_bytes, 0, sizeof(previous_bytes));

    for (i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "--click-us") == 0 && i + 1 < argc) {
            click_us = _strtoui64(argv[++i], NULL, 10);
            continue;
        }
        if (_stricmp(argv[i], "--min-hold-us") == 0 && i + 1 < argc) {
            min_hold_us = _strtoui64(argv[++i], NULL, 10);
            continue;
        }
    }
    if (argc < 2) {
        fprintf(stderr, "usage: trace_analyze.exe <log> [more.log ...]\n"
                        "       [--click-us N] [--min-hold-us N]\n");
        return 2;
    }

    for (arg = 1; arg < argc; ++arg) {
        FILE *file;
        char line[512];
        if (argv[arg][0] == '-') continue; /* option or its value */
        if (_stricmp(argv[arg], "--click-us") == 0 ||
            _stricmp(argv[arg], "--min-hold-us") == 0) {
            ++arg;
            continue;
        }
        file = fopen(argv[arg], "r");
        if (file == NULL) {
            fprintf(stderr, "cannot open %s\n", argv[arg]);
            return 1;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            char *down_text, *up_text, *bytes_text;
            uint64_t us;
            int down_mask, up_mask, byte_count;
            int derived_down, derived_up;
            uint8_t bytes[MAX_KEYS];
            if (line[0] == '#' || line[0] == '\n') continue;
            ++lines;
            us = _strtoui64(line + 2, NULL, 10); /* "t=<us>" */
            down_text = strstr(line, "down=");
            up_text = strstr(line, "up=");
            bytes_text = strstr(line, "bytes=");
            if (down_text == NULL || up_text == NULL || bytes_text == NULL) {
                ++bad_lines;
                continue;
            }
            down_text += 5;
            up_text += 3;
            bytes_text += 6;
            down_mask = parse_keys(down_text);
            up_mask = parse_keys(up_text);
            byte_count = parse_bytes(bytes_text, bytes);
            if (down_mask < 0 || up_mask < 0 || byte_count != MAX_KEYS) {
                ++bad_lines;
                continue;
            }
            /* Derive the transition from the byte columns instead of trusting
             * the down=/up= text, then report any disagreement. This is the same
             * independence the DLL relies on (bytes are the evidence, the
             * decoded names are an interpretation), and it means a log with a
             * wrong down= column still produces the right verdict instead of a
             * confidently wrong one. */
            {
                if (!have_previous) {
                    memcpy(previous_bytes, bytes, sizeof(bytes));
                    have_previous = true;
                    if (first_us == 0) first_us = us;
                    last_us = us;
                    ++changes;
                    continue;
                }
                derived_down = 0;
                derived_up = 0;
                for (i = 0; i < MAX_KEYS; ++i) {
                    if (previous_bytes[i] == 0 && bytes[i] != 0) {
                        derived_down |= 1 << i;
                    } else if (previous_bytes[i] != 0 && bytes[i] == 0) {
                        derived_up |= 1 << i;
                    }
                }
                if (derived_down != down_mask || derived_up != up_mask) {
                    ++inconsistent_lines;
                }
                memcpy(previous_bytes, bytes, sizeof(bytes));
                /* A line where nothing actually changed is not a transition. */
                if (derived_down == 0 && derived_up == 0) continue;
                down_mask = derived_down;
                up_mask = derived_up;
            }
            if (first_us == 0) first_us = us;
            last_us = us;
            ++changes;

            for (i = 0; i < MAX_KEYS; ++i) {
                note_value(&stats[i], bytes[i]);
                if (bytes[i] != 0 && bytes[i] != 1) ++stats[i].bytes_nonbinary;
            }

            for (i = 0; i < MAX_KEYS; ++i) {
                key_stats *ks = &stats[i];
                if (down_mask & (1 << i)) {
                    /* A down within click_us of this key's own last up is a
                     * bounce, not a new hit: the player never let go for long
                     * enough to mean it. */
                    if (state.click_candidate[i] &&
                        us >= state.last_up_us[i] &&
                        us - state.last_up_us[i] <= click_us) {
                        ++ks->clicks;
                    }
                    state.click_candidate[i] = false;
                    state.down_us[i] = us;
                    ++ks->presses;
                }
                if (up_mask & (1 << i)) {
                    if (state.down_us[i] != 0) {
                        uint64_t held = us - state.down_us[i];
                        ++ks->releases;
                        ++ks->holds;
                        ks->hold_sum_us += held;
                        if (ks->min_hold_us == 0 || held < ks->min_hold_us) {
                            ks->min_hold_us = held;
                        }
                        if (held > ks->max_hold_us) ks->max_hold_us = held;
                        if (held < min_hold_us) ++ks->short_releases;
                        state.down_us[i] = 0;
                    }
                    state.last_up_us[i] = us;
                    state.click_candidate[i] = true;
                }
            }
        }
        fclose(file);
    }

    printf("trace analysis\n");
    printf("  lines parsed      : %llu\n", (unsigned long long) lines);
    printf("  state changes     : %llu\n", (unsigned long long) changes);
    if (bad_lines) printf("  malformed lines   : %llu\n", (unsigned long long) bad_lines);
    if (inconsistent_lines) {
        printf("  down=/up= disagreed with the byte columns on %llu line(s).\n"
               "  The byte columns were used; the text columns are ignored for\n"
               "  the verdict.\n", (unsigned long long) inconsistent_lines);
    }
    if (changes) {
        printf("  span              : %.1f s\n",
               (double) (last_us - first_us) / 1e6);
    }
    printf("  click threshold   : %llu us (release shorter than this, then the\n"
           "                      same key down again, counts as a bounce)\n",
           (unsigned long long) click_us);

    printf("\n  %-4s %9s %9s %8s %10s %12s\n", "key", "presses",
           "releases", "clicks", "clicks/press", "shortest hold");
    for (i = 0; i < MAX_KEYS; ++i) {
        key_stats *ks = &stats[i];
        if (ks->presses == 0 && ks->releases == 0) continue;
        printf("  %-4s %9llu %9llu %8llu ", key_names[i],
               (unsigned long long) ks->presses,
               (unsigned long long) ks->releases,
               (unsigned long long) ks->clicks);
        if (ks->presses) {
            printf("%9.2f%% ", 100.0 * (double) ks->clicks / (double) ks->presses);
        } else {
            printf("%10s ", "-");
        }
        if (ks->min_hold_us) {
            printf("%9llu us", (unsigned long long) ks->min_hold_us);
        } else {
            printf("%12s", "-");
        }
        printf("\n");
    }

    printf("\n  byte values seen per key (00 and one nonzero value = a clean\n"
           "  switch; a third value means the byte is not binary):\n");
    for (i = 0; i < MAX_KEYS; ++i) {
        unsigned v;
        key_stats *ks = &stats[i];
        if (ks->value_count == 0) continue;
        printf("    %-4s:", key_names[i]);
        for (v = 0; v < ks->value_count; ++v) {
            printf(" %02X", ks->values_seen[v]);
        }
        if (ks->bytes_nonbinary) {
            printf("   (%llu change(s) with a non-binary byte)",
                   (unsigned long long) ks->bytes_nonbinary);
        }
        printf("\n");
    }

    printf("\nhow to read this\n");
    printf("  A click is a release and re-press of one key within %llu us. A\n",
           (unsigned long long) click_us);
    printf("  player cannot do that on purpose in a rhythm game, so clicks are\n");
    printf("  contact bounce: the switch opened briefly while you were holding it.\n\n");
    printf("  Do NOT compare raw click counts between keys. Compare clicks per\n");
    printf("  press: a key you play twice as often shows twice the clicks for the\n");
    printf("  same defect rate, so the raw count mostly measures how often you use\n");
    printf("  the key. The rate is what compares two switches.\n\n");
    printf("  The strongest evidence for one bad switch is not any single number\n");
    printf("  but the contrast: one key clicking on a percent of its presses while\n");
    printf("  the others stay near zero, at comparable press counts. If instead\n");
    printf("  every key clicks at a similar rate, the common factor is the player\n");
    printf("  or the environment, not the hardware.\n\n");
    printf("  'shortest hold' is the quickest full press-release seen on that key.\n");
    printf("  A few hundred microseconds there is not human: it is a contact\n");
    printf("  making and breaking, and it is the clearest single sign of a worn\n");
    printf("  switch.\n");
    return 0;
}

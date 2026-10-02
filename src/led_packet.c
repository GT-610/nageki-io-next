#include "led_packet.h"
#include <string.h>
void mu3_led_packet(const uint8_t rgb[18], uint8_t report[65])
{
    unsigned i;
    memset(report, 0, 65);
    report[0] = 0; /* HID report ID */
    report[1] = 0; /* payload Type */
    report[2] = 255; /* payload brightness */
    for (i = 0; i < 9; i++) {
        report[3 + i] = rgb[i] ? 255 : 0;
        report[18 + i] = rgb[9 + i] ? 255 : 0;
    }
}

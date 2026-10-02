#ifndef MU3_LED_PACKET_H
#define MU3_LED_PACKET_H
#include <stdint.h>
void mu3_led_packet(const uint8_t rgb[18], uint8_t report[65]);
#endif

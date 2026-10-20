#pragma once

#include <stdint.h>

#define WIRELESS_PROTOCOL_VERSION 1
#define WIRELESS_PAYLOAD_MAX 96

typedef enum {
    WIRELESS_PACKET_PAIR_REQUEST = 1,
    WIRELESS_PACKET_PAIR_ACCEPT,
    WIRELESS_PACKET_READY,
    WIRELESS_PACKET_COMMAND,
    WIRELESS_PACKET_RESPONSE,
    WIRELESS_PACKET_BUTTON_EVENT,
    WIRELESS_PACKET_ACK,
} wireless_packet_type_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint8_t payload_length;
    uint8_t payload[WIRELESS_PAYLOAD_MAX];
} wireless_packet_t;

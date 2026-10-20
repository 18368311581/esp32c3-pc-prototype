#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wireless_protocol.h"

typedef struct {
    uint8_t source[ESP_NOW_ETH_ALEN];
    bool is_broadcast;
    wireless_packet_t packet;
} wireless_rx_event_t;

esp_err_t wireless_link_init(QueueHandle_t receive_queue);
esp_err_t wireless_link_add_encrypted_peer(const uint8_t mac[ESP_NOW_ETH_ALEN]);
void wireless_link_remove_peer(const uint8_t mac[ESP_NOW_ETH_ALEN]);
esp_err_t wireless_link_send_peer(const uint8_t mac[ESP_NOW_ETH_ALEN],
                                  const wireless_packet_t *packet);
esp_err_t wireless_link_send_broadcast(const wireless_packet_t *packet);

bool wireless_pair_load(uint8_t mac[ESP_NOW_ETH_ALEN]);
esp_err_t wireless_pair_save(const uint8_t mac[ESP_NOW_ETH_ALEN]);
void wireless_pair_clear(void);

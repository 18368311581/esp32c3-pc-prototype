#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "espnow_link.h"
#include "wireless_config.h"

#define PAIR_NAMESPACE "espnow"
#define PAIR_MAC_KEY "peer_mac"

static QueueHandle_t s_receive_queue;
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void wireless_receive_callback(const esp_now_recv_info_t *receive_info,
                                      const uint8_t *data, int data_length)
{
    if (receive_info == NULL || data == NULL || data_length != sizeof(wireless_packet_t) ||
        s_receive_queue == NULL) {
        return;
    }

    wireless_rx_event_t event = {0};
    memcpy(event.source, receive_info->src_addr, ESP_NOW_ETH_ALEN);
    event.is_broadcast = memcmp(receive_info->des_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0;
    memcpy(&event.packet, data, sizeof(event.packet));

    if (event.packet.version != WIRELESS_PROTOCOL_VERSION ||
        event.packet.payload_length > WIRELESS_PAYLOAD_MAX) {
        return;
    }

    /* The Wi-Fi callback must stay short; roles consume this queue in their main task. */
    (void)xQueueSend(s_receive_queue, &event, 0);
}

esp_err_t wireless_link_init(QueueHandle_t receive_queue)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        return error;
    }

    s_receive_queue = receive_queue;
    ESP_RETURN_ON_ERROR(esp_netif_init(), "espnow_link", "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "espnow_link", "event loop init failed");

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), "espnow_link", "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "espnow_link", "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "espnow_link", "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "espnow_link", "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(WIRELESS_CHANNEL, WIFI_SECOND_CHAN_NONE),
                        "espnow_link", "wifi channel failed");

    ESP_RETURN_ON_ERROR(esp_now_init(), "espnow_link", "esp-now init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(wireless_receive_callback),
                        "espnow_link", "receive callback failed");
    ESP_RETURN_ON_ERROR(esp_now_set_pmk((const uint8_t *)WIRELESS_PMK),
                        "espnow_link", "PMK setup failed");

    esp_now_peer_info_t broadcast_peer = {0};
    memcpy(broadcast_peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = WIRELESS_CHANNEL;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;
    return esp_now_add_peer(&broadcast_peer);
}

esp_err_t wireless_link_add_encrypted_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (esp_now_is_peer_exist(mac)) {
        ESP_RETURN_ON_ERROR(esp_now_del_peer(mac), "espnow_link", "remove old peer failed");
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    memcpy(peer.lmk, WIRELESS_LMK, ESP_NOW_KEY_LEN);
    peer.channel = WIRELESS_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    return esp_now_add_peer(&peer);
}

void wireless_link_remove_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (esp_now_is_peer_exist(mac)) {
        (void)esp_now_del_peer(mac);
    }
}

esp_err_t wireless_link_send_peer(const uint8_t mac[ESP_NOW_ETH_ALEN],
                                  const wireless_packet_t *packet)
{
    return esp_now_send(mac, (const uint8_t *)packet, sizeof(*packet));
}

esp_err_t wireless_link_send_broadcast(const wireless_packet_t *packet)
{
    return esp_now_send(s_broadcast_mac, (const uint8_t *)packet, sizeof(*packet));
}

bool wireless_pair_load(uint8_t mac[ESP_NOW_ETH_ALEN])
{
    nvs_handle_t handle;
    size_t mac_length = ESP_NOW_ETH_ALEN;
    if (nvs_open(PAIR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    const esp_err_t error = nvs_get_blob(handle, PAIR_MAC_KEY, mac, &mac_length);
    nvs_close(handle);
    return error == ESP_OK && mac_length == ESP_NOW_ETH_ALEN;
}

esp_err_t wireless_pair_save(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(PAIR_NAMESPACE, NVS_READWRITE, &handle),
                        "espnow_link", "open NVS failed");
    esp_err_t error = nvs_set_blob(handle, PAIR_MAC_KEY, mac, ESP_NOW_ETH_ALEN);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

void wireless_pair_clear(void)
{
    nvs_handle_t handle;
    if (nvs_open(PAIR_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_erase_key(handle, PAIR_MAC_KEY);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

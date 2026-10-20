#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "espnow_link.h"

#define USER_BUTTON_GPIO GPIO_NUM_3
#define USER_LED_GPIO GPIO_NUM_7
#define BUTTON_SAMPLE_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 2
#define BUTTON_LONG_PRESS_MS 3000
#define PAIRING_WINDOW_MS 60000
#define PAIR_REQUEST_INTERVAL_MS 1000
#define RETRY_INTERVAL_MS 500
#define RETRY_MAX_ATTEMPTS 3
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ADC_SAMPLES 8
#define BATTERY_DIVIDER_TOP_OHMS 1000000
#define BATTERY_DIVIDER_BOTTOM_OHMS 330000

typedef struct {
    bool stable_pressed;
    bool last_sample_pressed;
    uint8_t stable_samples;
    bool long_press_handled;
    uint32_t pressed_at_ms;
} button_state_t;

typedef struct {
    bool active;
    wireless_packet_t packet;
    uint8_t attempts;
    uint32_t last_sent_ms;
} retry_packet_t;

static adc_oneshot_unit_handle_t s_battery_adc;
static adc_cali_handle_t s_battery_cali;
static bool s_battery_calibrated;
static uint16_t s_next_sequence;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool same_mac(const uint8_t left[ESP_NOW_ETH_ALEN], const uint8_t right[ESP_NOW_ETH_ALEN])
{
    return memcmp(left, right, ESP_NOW_ETH_ALEN) == 0;
}

static void init_board_gpio(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << USER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << USER_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_ERROR_CHECK(gpio_config(&led_config));
    gpio_set_level(USER_LED_GPIO, 1);
}

static void set_user_led(bool led_on)
{
    gpio_set_level(USER_LED_GPIO, led_on ? 0 : 1);
}

static void init_battery_adc(void)
{
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &s_battery_adc));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_battery_adc, BATTERY_ADC_CHANNEL, &channel_config));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_battery_calibrated = adc_cali_create_scheme_curve_fitting(
                               &calibration_config, &s_battery_cali) == ESP_OK;
#endif
}

static void get_battery_response(char *response, size_t response_size)
{
    int raw_sum = 0;
    for (int sample = 0; sample < BATTERY_ADC_SAMPLES; ++sample) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_battery_adc, BATTERY_ADC_CHANNEL, &raw));
        raw_sum += raw;
    }

    const int raw_average = raw_sum / BATTERY_ADC_SAMPLES;
    if (!s_battery_calibrated) {
        (void)snprintf(response, response_size, "ADC raw=%d adc_mv=uncalibrated", raw_average);
        return;
    }

    int adc_mv = 0;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_battery_cali, raw_average, &adc_mv));
    const int battery_mv = adc_mv * (BATTERY_DIVIDER_TOP_OHMS + BATTERY_DIVIDER_BOTTOM_OHMS) /
                           BATTERY_DIVIDER_BOTTOM_OHMS;
    (void)snprintf(response, response_size, "ADC raw=%d adc_mv=%d battery_mv=%d",
                   raw_average, adc_mv, battery_mv);
}

static bool button_update(button_state_t *state, bool *pressed, bool *long_pressed)
{
    const bool sampled_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0;
    *pressed = false;
    *long_pressed = false;

    if (sampled_pressed == state->last_sample_pressed) {
        if (state->stable_samples < BUTTON_DEBOUNCE_SAMPLES) {
            state->stable_samples++;
        }
    } else {
        state->last_sample_pressed = sampled_pressed;
        state->stable_samples = 1;
    }

    if (state->stable_samples == BUTTON_DEBOUNCE_SAMPLES &&
        sampled_pressed != state->stable_pressed) {
        state->stable_pressed = sampled_pressed;
        if (sampled_pressed) {
            state->pressed_at_ms = now_ms();
            state->long_press_handled = false;
            *pressed = true;
        }
    }

    if (state->stable_pressed && !state->long_press_handled &&
        time_reached(now_ms(), state->pressed_at_ms + BUTTON_LONG_PRESS_MS)) {
        state->long_press_handled = true;
        *long_pressed = true;
    }
    return state->stable_pressed;
}

static void packet_set_text(wireless_packet_t *packet, wireless_packet_type_t type,
                            uint16_t sequence, const char *text)
{
    const size_t text_length = strnlen(text, WIRELESS_PAYLOAD_MAX);
    memset(packet, 0, sizeof(*packet));
    packet->version = WIRELESS_PROTOCOL_VERSION;
    packet->type = type;
    packet->sequence = sequence;
    packet->payload_length = text_length;
    memcpy(packet->payload, text, text_length);
}

static void send_ready(const uint8_t receiver_mac[ESP_NOW_ETH_ALEN])
{
    wireless_packet_t packet;
    packet_set_text(&packet, WIRELESS_PACKET_READY, s_next_sequence++, "READY 1");
    (void)wireless_link_send_peer(receiver_mac, &packet);
}

static void start_pairing(uint8_t receiver_mac[ESP_NOW_ETH_ALEN], bool *paired,
                          uint32_t *pairing_until_ms, uint32_t *next_pair_request_ms)
{
    if (*paired) {
        wireless_link_remove_peer(receiver_mac);
    }
    wireless_pair_clear();
    memset(receiver_mac, 0, ESP_NOW_ETH_ALEN);
    *paired = false;
    *pairing_until_ms = now_ms() + PAIRING_WINDOW_MS;
    *next_pair_request_ms = 0;
    printf("DEVICE PAIRING 60s\n");
}

static void send_pair_request(void)
{
    wireless_packet_t packet;
    packet_set_text(&packet, WIRELESS_PACKET_PAIR_REQUEST, s_next_sequence++, "PAIR");
    (void)wireless_link_send_broadcast(&packet);
}

static void queue_button_event(retry_packet_t *button_retry,
                               const uint8_t receiver_mac[ESP_NOW_ETH_ALEN], bool paired)
{
    if (!paired) {
        printf("BUTTON 1 (not paired)\n");
        return;
    }

    packet_set_text(&button_retry->packet, WIRELESS_PACKET_BUTTON_EVENT,
                    s_next_sequence++, "BUTTON 1");
    button_retry->attempts = 1;
    button_retry->last_sent_ms = now_ms();
    button_retry->active = true;
    (void)wireless_link_send_peer(receiver_mac, &button_retry->packet);
}

static void process_command(const wireless_packet_t *command, bool *led_on,
                            const uint8_t receiver_mac[ESP_NOW_ETH_ALEN],
                            wireless_packet_t *last_response, uint16_t *last_sequence,
                            bool *has_last_response)
{
    if (*has_last_response && command->sequence == *last_sequence) {
        (void)wireless_link_send_peer(receiver_mac, last_response);
        return;
    }

    char command_text[WIRELESS_PAYLOAD_MAX + 1] = {0};
    memcpy(command_text, command->payload, command->payload_length);
    char response_text[WIRELESS_PAYLOAD_MAX + 1] = {0};

    if (strcmp(command_text, "ADC?") == 0) {
        get_battery_response(response_text, sizeof(response_text));
    } else if (strcmp(command_text, "LED 1") == 0) {
        *led_on = true;
        set_user_led(*led_on);
        (void)snprintf(response_text, sizeof(response_text), "OK LED 1");
    } else if (strcmp(command_text, "LED 0") == 0) {
        *led_on = false;
        set_user_led(*led_on);
        (void)snprintf(response_text, sizeof(response_text), "OK LED 0");
    } else if (strcmp(command_text, "LED TOGGLE") == 0) {
        *led_on = !*led_on;
        set_user_led(*led_on);
        (void)snprintf(response_text, sizeof(response_text), "OK LED %d", *led_on ? 1 : 0);
    } else if (strcmp(command_text, "INFO?") == 0) {
        uint8_t mac[ESP_NOW_ETH_ALEN] = {0};
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
        (void)snprintf(response_text, sizeof(response_text),
                       "INFO fw=espnow-v1 mac=%02X:%02X:%02X:%02X:%02X:%02X up=%lu heap=%lu",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                       (unsigned long)(esp_timer_get_time() / 1000000),
                       (unsigned long)esp_get_free_heap_size());
    } else {
        (void)snprintf(response_text, sizeof(response_text), "ERROR BAD_COMMAND");
    }

    packet_set_text(last_response, WIRELESS_PACKET_RESPONSE, command->sequence, response_text);
    *last_sequence = command->sequence;
    *has_last_response = true;
    (void)wireless_link_send_peer(receiver_mac, last_response);
}

void app_main(void)
{
    QueueHandle_t receive_queue = xQueueCreate(8, sizeof(wireless_rx_event_t));
    if (receive_queue == NULL) {
        printf("DEVICE ERROR QUEUE\n");
        return;
    }

    init_board_gpio();
    init_battery_adc();
    ESP_ERROR_CHECK(wireless_link_init(receive_queue));

    uint8_t receiver_mac[ESP_NOW_ETH_ALEN] = {0};
    bool paired = wireless_pair_load(receiver_mac);
    if (paired) {
        ESP_ERROR_CHECK(wireless_link_add_encrypted_peer(receiver_mac));
    }

    button_state_t button = {
        .stable_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0,
        .last_sample_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0,
        .stable_samples = BUTTON_DEBOUNCE_SAMPLES,
    };
    retry_packet_t button_retry = {0};
    wireless_packet_t last_response = {0};
    uint16_t last_command_sequence = 0;
    bool has_last_response = false;
    bool led_on = false;
    bool ready_pending = paired;
    uint32_t ready_at_ms = now_ms() + 500;
    uint32_t pairing_until_ms = paired ? 0 : now_ms() + PAIRING_WINDOW_MS;
    uint32_t next_pair_request_ms = 0;

    printf("DEVICE role=device paired=%d\n", paired ? 1 : 0);

    while (true) {
        const uint32_t current_ms = now_ms();
        bool just_pressed;
        bool long_pressed;
        (void)button_update(&button, &just_pressed, &long_pressed);

        if (just_pressed) {
            led_on = !led_on;
            set_user_led(led_on);
            queue_button_event(&button_retry, receiver_mac, paired);
        }
        if (long_pressed) {
            start_pairing(receiver_mac, &paired, &pairing_until_ms, &next_pair_request_ms);
            ready_pending = false;
            button_retry.active = false;
        }

        if (!paired && pairing_until_ms != 0 && !time_reached(current_ms, pairing_until_ms) &&
            time_reached(current_ms, next_pair_request_ms)) {
            send_pair_request();
            next_pair_request_ms = current_ms + PAIR_REQUEST_INTERVAL_MS;
        }

        if (ready_pending && time_reached(current_ms, ready_at_ms)) {
            send_ready(receiver_mac);
            ready_pending = false;
        }

        if (button_retry.active && time_reached(current_ms, button_retry.last_sent_ms + RETRY_INTERVAL_MS)) {
            if (button_retry.attempts >= RETRY_MAX_ATTEMPTS) {
                button_retry.active = false;
            } else {
                (void)wireless_link_send_peer(receiver_mac, &button_retry.packet);
                button_retry.attempts++;
                button_retry.last_sent_ms = current_ms;
            }
        }

        wireless_rx_event_t event;
        while (xQueueReceive(receive_queue, &event, 0) == pdTRUE) {
            if (event.packet.type == WIRELESS_PACKET_PAIR_ACCEPT && event.is_broadcast &&
                !paired && event.packet.payload_length == ESP_NOW_ETH_ALEN) {
                uint8_t own_mac[ESP_NOW_ETH_ALEN] = {0};
                ESP_ERROR_CHECK(esp_read_mac(own_mac, ESP_MAC_WIFI_STA));
                if (memcmp(event.packet.payload, own_mac, ESP_NOW_ETH_ALEN) == 0) {
                    ESP_ERROR_CHECK(wireless_link_add_encrypted_peer(event.source));
                    ESP_ERROR_CHECK(wireless_pair_save(event.source));
                    memcpy(receiver_mac, event.source, ESP_NOW_ETH_ALEN);
                    paired = true;
                    pairing_until_ms = 0;
                    ready_pending = true;
                    ready_at_ms = now_ms() + 100;
                    printf("DEVICE PAIRED\n");
                }
                continue;
            }

            if (!paired || event.is_broadcast || !same_mac(event.source, receiver_mac)) {
                continue;
            }
            if (event.packet.type == WIRELESS_PACKET_COMMAND) {
                process_command(&event.packet, &led_on, receiver_mac, &last_response,
                                &last_command_sequence, &has_last_response);
            } else if (event.packet.type == WIRELESS_PACKET_ACK && button_retry.active &&
                       event.packet.sequence == button_retry.packet.sequence) {
                button_retry.active = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

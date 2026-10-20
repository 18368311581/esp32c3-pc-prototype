#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "espnow_link.h"

#define USER_BUTTON_GPIO GPIO_NUM_3
#define BUTTON_SAMPLE_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 2
#define BUTTON_LONG_PRESS_MS 3000
#define PAIRING_WINDOW_MS 60000
#define RETRY_INTERVAL_MS 500
#define RETRY_MAX_ATTEMPTS 3
#define COMMAND_MAX_LENGTH WIRELESS_PAYLOAD_MAX

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
} pending_command_t;

typedef struct {
    bool active;
    uint8_t device_mac[ESP_NOW_ETH_ALEN];
    uint8_t sends_remaining;
    uint32_t next_send_ms;
} pairing_candidate_t;

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

static void init_button_gpio(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << USER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static void init_serial_input(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();

    const int flags = fcntl(STDIN_FILENO, F_GETFL);
    ESP_ERROR_CHECK(flags < 0 ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0 ? ESP_FAIL : ESP_OK);
}

static bool button_update(button_state_t *state, bool *long_pressed)
{
    const bool sampled_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0;
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

static void terminal_prompt(const char *command, size_t command_length)
{
    printf("> %.*s", (int)command_length, command);
}

static void terminal_print_async(const char *line, const char *command, size_t command_length)
{
    printf("\r\n%s\r\n", line);
    terminal_prompt(command, command_length);
}

static void print_help(const char *command, size_t command_length)
{
    terminal_print_async("Commands: ADC? | LED 0 | LED 1 | LED TOGGLE | INFO? | HELP", command, command_length);
}

static void send_ack(const uint8_t device_mac[ESP_NOW_ETH_ALEN], uint16_t sequence)
{
    wireless_packet_t packet;
    packet_set_text(&packet, WIRELESS_PACKET_ACK, sequence, "ACK");
    (void)wireless_link_send_peer(device_mac, &packet);
}

static void start_pairing(uint8_t device_mac[ESP_NOW_ETH_ALEN], bool *paired,
                          uint32_t *pairing_until_ms, pairing_candidate_t *candidate,
                          const char *command, size_t command_length)
{
    if (*paired) {
        wireless_link_remove_peer(device_mac);
    }
    wireless_pair_clear();
    memset(device_mac, 0, ESP_NOW_ETH_ALEN);
    *paired = false;
    *pairing_until_ms = now_ms() + PAIRING_WINDOW_MS;
    *candidate = (pairing_candidate_t){0};
    terminal_print_async("PAIRING window=60s", command, command_length);
}

static void send_pair_accept(pairing_candidate_t *candidate)
{
    wireless_packet_t packet = {0};
    packet.version = WIRELESS_PROTOCOL_VERSION;
    packet.type = WIRELESS_PACKET_PAIR_ACCEPT;
    packet.sequence = s_next_sequence++;
    packet.payload_length = ESP_NOW_ETH_ALEN;
    memcpy(packet.payload, candidate->device_mac, ESP_NOW_ETH_ALEN);
    (void)wireless_link_send_broadcast(&packet);
}

static void begin_command(const char *command, const uint8_t device_mac[ESP_NOW_ETH_ALEN],
                          bool paired, pending_command_t *pending,
                          const char *input, size_t input_length)
{
    if (pending->active) {
        terminal_print_async("ERROR BUSY", input, input_length);
        return;
    }
    if (!paired) {
        terminal_print_async("ERROR RADIO_OFFLINE (hold receiver button 3s to pair)",
                             input, input_length);
        return;
    }

    packet_set_text(&pending->packet, WIRELESS_PACKET_COMMAND, s_next_sequence++, command);
    pending->active = true;
    pending->attempts = 1;
    pending->last_sent_ms = now_ms();
    if (wireless_link_send_peer(device_mac, &pending->packet) != ESP_OK) {
        pending->last_sent_ms = 0;
    }
}

static void handle_terminal_line(char *command, size_t command_length,
                                 const uint8_t device_mac[ESP_NOW_ETH_ALEN], bool paired,
                                 pending_command_t *pending)
{
    command[command_length] = '\0';
    while (command_length > 0 && isspace((unsigned char)command[command_length - 1])) {
        command[--command_length] = '\0';
    }
    char *first = command;
    while (*first != '\0' && isspace((unsigned char)*first)) {
        first++;
    }
    if (*first == '\0') {
        return;
    }

    printf("\r\n");
    if (strcmp(first, "HELP") == 0) {
        print_help("", 0);
        return;
    }
    begin_command(first, device_mac, paired, pending, "", 0);
}

static void poll_terminal(char *command, size_t *command_length,
                          const uint8_t device_mac[ESP_NOW_ETH_ALEN], bool paired,
                          pending_command_t *pending)
{
    fd_set read_fds;
    struct timeval timeout = {0};
    char received;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    if (select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout) <= 0 ||
        read(STDIN_FILENO, &received, 1) != 1) {
        return;
    }

    if (received == '\r') {
        return;
    }
    if (received == '\n') {
        handle_terminal_line(command, *command_length, device_mac, paired, pending);
        *command_length = 0;
        terminal_prompt(command, *command_length);
        return;
    }
    if (received == '\b' || received == 0x7f) {
        if (*command_length > 0) {
            (*command_length)--;
            printf("\b \b");
        }
        return;
    }
    if (!isprint((unsigned char)received)) {
        return;
    }
    if (*command_length >= COMMAND_MAX_LENGTH - 1) {
        terminal_print_async("ERROR COMMAND_TOO_LONG", command, *command_length);
        *command_length = 0;
        return;
    }
    command[(*command_length)++] = received;
    putchar(received);
}

void app_main(void)
{
    QueueHandle_t receive_queue = xQueueCreate(8, sizeof(wireless_rx_event_t));
    if (receive_queue == NULL) {
        printf("RECEIVER ERROR QUEUE\n");
        return;
    }

    init_button_gpio();
    init_serial_input();
    ESP_ERROR_CHECK(wireless_link_init(receive_queue));

    uint8_t device_mac[ESP_NOW_ETH_ALEN] = {0};
    bool paired = wireless_pair_load(device_mac);
    if (paired) {
        ESP_ERROR_CHECK(wireless_link_add_encrypted_peer(device_mac));
    }

    uint8_t receiver_mac[ESP_NOW_ETH_ALEN] = {0};
    ESP_ERROR_CHECK(esp_read_mac(receiver_mac, ESP_MAC_WIFI_STA));
    printf("RECEIVER role=receiver mac=%02X:%02X:%02X:%02X:%02X:%02X paired=%d\r\n",
           receiver_mac[0], receiver_mac[1], receiver_mac[2], receiver_mac[3],
           receiver_mac[4], receiver_mac[5], paired ? 1 : 0);

    button_state_t button = {
        .stable_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0,
        .last_sample_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0,
        .stable_samples = BUTTON_DEBOUNCE_SAMPLES,
    };
    pending_command_t pending = {0};
    pairing_candidate_t candidate = {0};
    uint32_t pairing_until_ms = 0;
    uint16_t last_button_sequence = 0;
    bool have_last_button_sequence = false;
    char command[COMMAND_MAX_LENGTH] = {0};
    size_t command_length = 0;
    terminal_prompt(command, command_length);

    while (true) {
        const uint32_t current_ms = now_ms();
        bool long_pressed;
        (void)button_update(&button, &long_pressed);
        if (long_pressed) {
            start_pairing(device_mac, &paired, &pairing_until_ms, &candidate,
                          command, command_length);
        }

        poll_terminal(command, &command_length, device_mac, paired, &pending);

        if (pending.active && time_reached(current_ms, pending.last_sent_ms + RETRY_INTERVAL_MS)) {
            if (pending.attempts >= RETRY_MAX_ATTEMPTS) {
                pending.active = false;
                terminal_print_async("ERROR RADIO_TIMEOUT", command, command_length);
            } else {
                (void)wireless_link_send_peer(device_mac, &pending.packet);
                pending.attempts++;
                pending.last_sent_ms = current_ms;
            }
        }

        if (candidate.active && candidate.sends_remaining > 0 &&
            time_reached(current_ms, candidate.next_send_ms)) {
            send_pair_accept(&candidate);
            candidate.sends_remaining--;
            candidate.next_send_ms = current_ms + 200;
        }
        if (pairing_until_ms != 0 && time_reached(current_ms, pairing_until_ms)) {
            if (candidate.active) {
                wireless_link_remove_peer(candidate.device_mac);
                candidate.active = false;
            }
            pairing_until_ms = 0;
            terminal_print_async("PAIRING timeout", command, command_length);
        }

        wireless_rx_event_t event;
        while (xQueueReceive(receive_queue, &event, 0) == pdTRUE) {
            if (event.packet.type == WIRELESS_PACKET_PAIR_REQUEST && event.is_broadcast &&
                pairing_until_ms != 0 && !time_reached(now_ms(), pairing_until_ms)) {
                if (!candidate.active) {
                    ESP_ERROR_CHECK(wireless_link_add_encrypted_peer(event.source));
                    candidate.active = true;
                    memcpy(candidate.device_mac, event.source, ESP_NOW_ETH_ALEN);
                    candidate.sends_remaining = 3;
                    candidate.next_send_ms = 0;
                    terminal_print_async("PAIRING candidate found", command, command_length);
                } else if (same_mac(candidate.device_mac, event.source)) {
                    candidate.sends_remaining = 3;
                    candidate.next_send_ms = 0;
                }
                continue;
            }

            const bool from_candidate = candidate.active && same_mac(event.source, candidate.device_mac);
            const bool from_paired_device = paired && same_mac(event.source, device_mac);
            if (event.is_broadcast || (!from_candidate && !from_paired_device)) {
                continue;
            }

            if (event.packet.type == WIRELESS_PACKET_READY) {
                if (from_candidate) {
                    memcpy(device_mac, candidate.device_mac, ESP_NOW_ETH_ALEN);
                    ESP_ERROR_CHECK(wireless_pair_save(device_mac));
                    paired = true;
                    candidate.active = false;
                    pairing_until_ms = 0;
                    have_last_button_sequence = false;
                }
                terminal_print_async("READY device=paired", command, command_length);
            } else if (event.packet.type == WIRELESS_PACKET_RESPONSE && pending.active &&
                       event.packet.sequence == pending.packet.sequence) {
                char response[WIRELESS_PAYLOAD_MAX + 1] = {0};
                memcpy(response, event.packet.payload, event.packet.payload_length);
                pending.active = false;
                terminal_print_async(response, command, command_length);
            } else if (event.packet.type == WIRELESS_PACKET_BUTTON_EVENT) {
                send_ack(event.source, event.packet.sequence);
                if (!have_last_button_sequence || event.packet.sequence != last_button_sequence) {
                    have_last_button_sequence = true;
                    last_button_sequence = event.packet.sequence;
                    terminal_print_async("BUTTON 1", command, command_length);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

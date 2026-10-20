/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#define USER_BUTTON_GPIO GPIO_NUM_3
#define USER_LED_GPIO    GPIO_NUM_7
#define BUTTON_SAMPLE_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 2
#define COMMAND_MAX_LENGTH 16
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ADC_SAMPLES 8
#define BATTERY_REPORT_INTERVAL_MS 5000
#define BATTERY_DIVIDER_TOP_OHMS 1000000
#define BATTERY_DIVIDER_BOTTOM_OHMS 330000

static adc_oneshot_unit_handle_t battery_adc_handle;
static adc_cali_handle_t battery_cali_handle;
static bool battery_adc_calibrated;

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

    gpio_config(&button_config);
    gpio_config(&led_config);
    gpio_set_level(USER_LED_GPIO, 1); // GPIO7 sinks current, so low turns the LED on.
}

static void set_user_led(bool led_on)
{
    gpio_set_level(USER_LED_GPIO, led_on ? 0 : 1);
}

static void init_serial_command_input(void)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();

    // select() needs the USB Serial/JTAG driver; non-blocking reads preserve button timing.
    const int flags = fcntl(STDIN_FILENO, F_GETFL);
    ESP_ERROR_CHECK(flags < 0 ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0 ? ESP_FAIL : ESP_OK);
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

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &battery_adc_handle));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc_handle,
                                               BATTERY_ADC_CHANNEL, &channel_config));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    battery_adc_calibrated = adc_cali_create_scheme_curve_fitting(
                                 &calibration_config, &battery_cali_handle) == ESP_OK;
#endif

    printf("BATTERY_ADC gpio=0 divider=1M/330k calibrated=%d\n",
           battery_adc_calibrated ? 1 : 0);
}

static void print_battery_voltage(void)
{
    int raw_sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; ++i) {
        int raw;
        ESP_ERROR_CHECK(adc_oneshot_read(battery_adc_handle, BATTERY_ADC_CHANNEL, &raw));
        raw_sum += raw;
    }

    const int raw_average = raw_sum / BATTERY_ADC_SAMPLES;
    if (!battery_adc_calibrated) {
        printf("BATTERY raw_avg=%d adc_mv=uncalibrated\n", raw_average);
        return;
    }

    int adc_mv;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(battery_cali_handle, raw_average, &adc_mv));
    const int battery_mv = adc_mv * (BATTERY_DIVIDER_TOP_OHMS + BATTERY_DIVIDER_BOTTOM_OHMS) /
                           BATTERY_DIVIDER_BOTTOM_OHMS;
    printf("BATTERY raw_avg=%d adc_mv=%d battery_mv=%d\n", raw_average, adc_mv, battery_mv);
}

static void handle_command(const char *command, bool *led_on)
{
    if (strcmp(command, "LED 1") == 0) {
        *led_on = true;
        set_user_led(*led_on);
        printf("OK LED 1\n");
    } else if (strcmp(command, "LED 0") == 0) {
        *led_on = false;
        set_user_led(*led_on);
        printf("OK LED 0\n");
    } else {
        printf("ERROR BAD_COMMAND\n");
    }
}

static void poll_serial_command(char *command, size_t *command_length,
                                bool *discard_until_newline, bool *led_on)
{
    fd_set read_fds;
    struct timeval timeout = {0};
    char received;

    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    // A zero timeout keeps USB command handling from delaying 10 ms button sampling.
    if (select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout) <= 0 ||
        read(STDIN_FILENO, &received, 1) != 1) {
        return;
    }

    if (received == '\r') {
        return;
    }

    if (received == '\n') {
        if (!*discard_until_newline) {
            command[*command_length] = '\0';
            if (*command_length > 0) {
                handle_command(command, led_on);
            }
        }
        *command_length = 0;
        *discard_until_newline = false;
        return;
    }

    if (*discard_until_newline) {
        return;
    }

    if (*command_length >= COMMAND_MAX_LENGTH - 1) {
        *command_length = 0;
        *discard_until_newline = true;
        printf("ERROR COMMAND_TOO_LONG\n");
        return;
    }

    command[(*command_length)++] = received;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("BOOT reset_reason=%d\n", esp_reset_reason());
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    init_board_gpio();
    init_serial_command_input();
    init_battery_adc();

    bool button_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0;
    bool last_sample_pressed = button_pressed;
    bool led_on = false;
    uint32_t stable_sample_count = 0;
    uint32_t uptime_seconds = 0;
    uint32_t heartbeat_sample_count = 0;
    uint32_t battery_report_sample_count = 0;
    char command[COMMAND_MAX_LENGTH];
    size_t command_length = 0;
    bool discard_until_newline = false;

    printf("BOARD_TEST button_gpio=%d led_gpio=%d\n", USER_BUTTON_GPIO, USER_LED_GPIO);

    while (true) {
        const bool sampled_pressed = gpio_get_level(USER_BUTTON_GPIO) == 0;

        poll_serial_command(command, &command_length, &discard_until_newline, &led_on);

        if (sampled_pressed == last_sample_pressed) {
            if (stable_sample_count < BUTTON_DEBOUNCE_SAMPLES) {
                stable_sample_count++;
            }
        } else {
            last_sample_pressed = sampled_pressed;
            stable_sample_count = 1;
        }

        if (stable_sample_count == BUTTON_DEBOUNCE_SAMPLES && sampled_pressed != button_pressed) {
            button_pressed = sampled_pressed;
            printf("BUTTON %d\n", button_pressed ? 1 : 0);

            if (button_pressed) {
                led_on = !led_on;
                set_user_led(led_on);
                printf("LED %d\n", led_on ? 1 : 0);
            }
        }

        heartbeat_sample_count++;
        if (heartbeat_sample_count == 1000 / BUTTON_SAMPLE_MS) {
            heartbeat_sample_count = 0;
            uptime_seconds++;
            printf("HEARTBEAT uptime=%" PRIu32 "s heap=%" PRIu32 "\n",
                   uptime_seconds, esp_get_free_heap_size());
        }

        battery_report_sample_count++;
        if (battery_report_sample_count == BATTERY_REPORT_INTERVAL_MS / BUTTON_SAMPLE_MS) {
            battery_report_sample_count = 0;
            print_battery_voltage();
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

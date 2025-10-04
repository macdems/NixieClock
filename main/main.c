/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "clock.h"
#include "effects.h"
#include "gpio.h"
#include "leds.h"
#include "log.h"
#include "mqtt.h"
#include "nixies.h"
#include "nvs.h"
#include "ota.h"

// static const char* const TAG = "main";

#define LED_PIN GPIO_NUM_8

static bool primary_wifi = true;

static void connect_wifi() {
    wifi_config_t wifi_config = {
        .sta =
            {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
    };

    if (!primary_wifi) {
        strncpy((char*)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    } else {
        strncpy((char*)wifi_config.sta.ssid, CONFIG_WIFI_SSID_2, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, CONFIG_WIFI_PASSWORD_2, sizeof(wifi_config.sta.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void on_wifi_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START: connect_wifi(); return;
        case WIFI_EVENT_STA_CONNECTED:
            progress_boot_dots("WiFi connected");  // (4)
            return;
        case WIFI_EVENT_STA_DISCONNECTED:
            primary_wifi = !primary_wifi;
            connect_wifi();
            return;
        default: return;
    }
}

static void on_ip_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            // Start network services
            progress_boot_dots("Got IP");  // (5)
            init_mqtt_client();
            progress_boot_dots("MQTT client initialized");  // (6)
            init_clock();
            gpio_set_level(LED_PIN, 1);
            return;
        default: return;
    }
    // ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    // ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

static esp_err_t init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t* sta = esp_netif_create_default_wifi_sta();
    assert(sta);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void app_main() {
    // Initialize logging
    init_logging();

    // Configure on-board LED GPIO and turn it on (will turn off after successful WiFi connection)
    configure_gpio(HOUR_PIN, 0);
    configure_gpio(MINUTE_PIN, 0);
    configure_gpio(DOTS_PIN, 0);
    configure_gpio(COLON_PIN, 0);
    configure_gpio(SRCLK_PIN, 0);
    configure_gpio(RCLK_PIN, 0);
    configure_gpio(LED_PIN, 0);

    // Turn off any display
    display_digits(-1, -1, -1, -1);
    progress_boot_dots("GPIO initialized");  // (1)

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    init_nvs();
    init_leds();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_wifi());
    progress_boot_dots("WiFi initialized");  // (2)

    start_digits_effect(DIGITS_EFFECT_BOOT);
    progress_boot_dots("After digits scan");  // (3)

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    ota_cancel_rollback();
#endif
}

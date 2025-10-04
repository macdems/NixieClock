#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <led_strip.h>

#include "leds.h"
#include "mqtt.h"
#include "nvs.h"

#define LED_STRIP_GPIO_PIN 21

_Atomic bool leds_on = true;
_Atomic uint16_t leds_hue = 240;
_Atomic uint8_t leds_saturation = 255;
_Atomic uint8_t leds_value = 255;

static const char* const TAG = "leds";

static led_strip_handle_t led_strip;

void init_leds() {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN,                         // The GPIO that connected to the LED strip's data line
        .max_leds = 4,                                                // The number of LEDs in the strip,
        .led_model = LED_MODEL_WS2812,                                // LED strip model
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,  // set the color order of the strip: GRB
        .flags = {.invert_out = false}                                // don't invert the output signal
    };

    // LED strip backend configuration: SPI
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,  // different clock source can lead to different power consumption
        .spi_bus = SPI2_HOST,            // SPI bus ID
        .flags = {.with_dma = true}      // Using DMA can improve performance and help drive more LEDs
    };

    // LED Strip object handle
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
    ESP_LOGI(TAG, "Created LED strip object with SPI backend");

    update_leds();
}

void update_leds() {
    if (led_strip == NULL) {
        ESP_LOGW(TAG, "LED strip not initialized");
        return;
    }

    if (leds_on) {
        // Set all pixels to the specified HSV color
        for (uint32_t i = 0; i < 4; i++) {
            ESP_ERROR_CHECK(led_strip_set_pixel_hsv(led_strip, i, leds_hue, leds_saturation, leds_value));
        }
        mqtt_client_publish("leds/state", "ON", 1);
        ESP_LOGI(TAG, "LEDs updated: H=%d, S=%d, V=%d", leds_hue, leds_saturation, leds_value);
    } else {
        // Turn off all LEDs
        for (uint32_t i = 0; i < 4; i++) {
            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, i, 0, 0, 0));
        }
        mqtt_client_publish("leds/state", "OFF", 1);
        ESP_LOGI(TAG, "LEDs turned off");
    }

    // Refresh the strip to apply changes
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));

    save_state_to_nvs();
}

void publish_initial_leds_state() {
    const char* state = leds_on ? "ON" : "OFF";
    mqtt_client_publish("leds/state", state, 1);

    char hs_state[32];
    float s_percent = leds_saturation * 100.0f / 255.0f;
    snprintf(hs_state, sizeof(hs_state), "%d.000,%.3f", leds_hue, s_percent);
    mqtt_client_publish("leds/hs/state", hs_state, 1);
}

bool handle_led_command(const char* topic, int topic_len, const char* data, int data_len) {
    if (strncmp(topic, "leds/set", topic_len) == 0) {
        // Expect "ON" or "OFF"
        if (data_len >= 2 && !strncmp(data, "ON", 2)) {
            leds_on = true;
        } else {
            leds_on = false;
        }
        update_leds();
        // Echo state back
        const char* state = leds_on ? "ON" : "OFF";
        mqtt_client_publish("leds/state", state, 1);
        ESP_LOGI(TAG, "'Leds' set -> %s", state);
        return true;
    }

    if (strncmp(topic, "leds/brightness/set", topic_len) == 0) {
        const char value[16];
        copy_str_to_buffer(data, data_len, (char*)value, sizeof(value));
        int brightness = atoi(value);
        // Expect brightness value 0-255
        if (brightness < 0) brightness = 0; else if (brightness > 255) brightness = 255;
        leds_value = (uint8_t)brightness;
        update_leds();
        // Echo state back
        char state[16];
        snprintf(state, sizeof(state), "%d", brightness);
        mqtt_client_publish("leds/brightness/state", state, 1);
        ESP_LOGI(TAG, "'Leds' brightness set -> %d", brightness);
        return true;
    }

    if (strncmp(topic, "leds/hs/set", topic_len) == 0) {
        // Expect "H,S" where H=0-360., S=0-100.
        const char value[128];
        copy_str_to_buffer(data, data_len, (char*)value, sizeof(value));
        float h = 0.0f, s = 0.0f;
        int parsed = sscanf(value, "%f,%f", &h, &s);
        if (parsed == 2) {
            if (h < 0.0f || h >= 360.0f) h = 0.0f;
            if (s < 0.0f) s = 0.0f; else if (s > 100.0f) s = 100.0f;
            leds_hue = (uint16_t)h;
            leds_saturation = (uint8_t)(s * 255.0f / 100.0f);
            update_leds();
            // Echo state back
            char state[32];
            snprintf(state, sizeof(state), "%.1f,%.1f", h, s);
            mqtt_client_publish("leds/hs/state", state, 1);
            ESP_LOGI(TAG, "'Leds' HS set -> %.1f, %.1f  [%d, %d]", h, s, leds_hue, leds_saturation);
            return true;
        }
    }

    return false;
}

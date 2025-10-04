#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include "clock.h"
#include "effects.h"
#include "leds.h"
#include "nvs.h"

#define SAVE_DELAY_US (30 * 1000000LL)  // 30 seconds

static const char* const TAG = "nvs";

static nvs_handle_t nixie_clock_nvs_handle = 0;

static esp_timer_handle_t save_state_timer = NULL;
static bool save_state_timer_pending = false;

void init_nvs() {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_open("nixie-clock", NVS_READWRITE, &nixie_clock_nvs_handle));
    load_state_from_nvs();
}

void load_state_from_nvs() {
    if (nixie_clock_nvs_handle) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "leds_on", (uint8_t*)&leds_on));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u16(nixie_clock_nvs_handle, "leds_hue", (uint16_t*)&leds_hue));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "leds_saturation", (uint8_t*)&leds_saturation));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "leds_value", (uint8_t*)&leds_value));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "blink_colon", (uint8_t*)&blink_colon));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "use_vegas", (uint8_t*)&use_vegas));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_u8(nixie_clock_nvs_handle, "cathode_protect", (uint8_t*)&cathode_protection));
        blink_colon = (blink_colon != 0);
        use_vegas = (use_vegas != 0);
        cathode_protection = (cathode_protection != 0);
        ESP_LOGI(TAG, "Restored state from NVS: leds_on=%d, H=%d, S=%d, V=%d, blink_colon=%d, use_vegas=%d, cathode_protect=%d",
                 leds_on, leds_hue, leds_saturation, leds_value, blink_colon, use_vegas, cathode_protection);
    }
}

static void save_state_job(void* arg) {
    if (nixie_clock_nvs_handle) {
        uint8_t o = 1 - leds_on, s = 255 - leds_saturation, v = 255 - leds_value;
        uint16_t h = leds_hue + 360;
        uint8_t bc = 1 - blink_colon;
        uint8_t uv = 1 - use_vegas;
        uint8_t cp = 1 - cathode_protection;
        // Check if anything changed since last save
        nvs_get_u8(nixie_clock_nvs_handle, "leds_on", &o);
        nvs_get_u16(nixie_clock_nvs_handle, "leds_hue", &h);
        nvs_get_u8(nixie_clock_nvs_handle, "leds_saturation", &s);
        nvs_get_u8(nixie_clock_nvs_handle, "leds_value", &v);
        nvs_get_u8(nixie_clock_nvs_handle, "blink_colon", &bc);
        nvs_get_u8(nixie_clock_nvs_handle, "use_vegas", &uv);
        nvs_get_u8(nixie_clock_nvs_handle, "cathode_protect", &cp);
        if (o != (uint8_t)leds_on || h != leds_hue || s != leds_saturation || v != leds_value ||  //
            bc != (uint8_t)blink_colon || uv != (uint8_t)use_vegas || cp != (uint8_t)cathode_protection) {
            ESP_LOGI(TAG, "Saving LEDs state to NVS: on=%d, H=%d, S=%d, V=%d, blink_colon=%d, use_vegas=%d, cathode_protect=%d",
                     leds_on, leds_hue, leds_saturation, leds_value, blink_colon, use_vegas, cathode_protection);
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "leds_on", (uint8_t)leds_on));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u16(nixie_clock_nvs_handle, "leds_hue", leds_hue));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "leds_saturation", leds_saturation));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "leds_value", leds_value));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "blink_colon", (uint8_t)blink_colon));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "use_vegas", (uint8_t)use_vegas));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nixie_clock_nvs_handle, "cathode_protect", (uint8_t)cathode_protection));
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nixie_clock_nvs_handle));
        }
    }
    save_state_timer_pending = false;
}

void save_state_to_nvs() {
    if (save_state_timer_pending) return;  // Timer already pending, do nothing

    // Schedule save after delay
    if (save_state_timer == NULL) {
        esp_timer_create_args_t timer_args = {.callback = save_state_job, .arg = NULL, .name = "nvs_save_state_timer"};
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &save_state_timer));
    }
    ESP_ERROR_CHECK(esp_timer_start_once(save_state_timer, SAVE_DELAY_US));
    save_state_timer_pending = true;
}

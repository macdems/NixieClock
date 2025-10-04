#include <stdlib.h>
#include <time.h>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "effects.h"
#include "gpio.h"
#include "nixies.h"

static const char* const TAG = "clock";

_Atomic bool force_clock_update = false;
_Atomic bool date_shown = false;
_Atomic bool blink_colon = false;
_Atomic bool use_vegas = true;
_Atomic bool do_vegas = true;

static void clock_task(void* param);

void init_clock() {
    ESP_LOGI(TAG, "Setting time zone to CET/CEST");
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    // Set up NTP
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        5,
        ESP_SNTP_SERVER_LIST("ntp.p.lodz.pl", "0.pl.pool.ntp.org", "1.pl.pool.ntp.org", "2.pl.pool.ntp.org", "3.pl.pool.ntp.org"));
    esp_netif_sntp_init(&config);
    progress_boot_dots("NTP initialized");  // (8)
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)));

    ESP_LOGI(TAG, "Starting clock task");
    xTaskCreate(clock_task, "clock_task", 8192, NULL, 5, NULL);
}

void show_date(bool show) {
    date_shown = show;
    force_clock_update = true;
}

static void clock_task(void* param) {
    time_t now;
    struct tm ti;
    struct tm pti = {.tm_min = -1};

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        time(&now);
        localtime_r(&now, &ti);

        enum DigitsEffect digits_effect = current_digits_effect();

        if (ti.tm_hour == 2 && ti.tm_min == 0 && ti.tm_sec == 0 && cathode_protection && !cathode_protection_in_progress &&
            (digits_effect == DIGITS_EFFECT_NONE || digits_effect == DIGITS_EFFECT_OFF || digits_effect == DIGITS_EFFECT_VEGAS))
            start_cathode_protection_cycle();
        else if (ti.tm_hour == 3 && ti.tm_min == 0 && ti.tm_sec == 0 && cathode_protection_in_progress)
            stop_cathode_protection_cycle();

        if (digits_effect == DIGITS_EFFECT_NONE && (ti.tm_min != pti.tm_min || force_clock_update)) {
            force_clock_update = false;
            if (date_shown) {
                if (current_dots_effect() == DOTS_EFFECT_NONE) set_dots(0x88);
                if (use_vegas && do_vegas) {
                    vegas_display_numbers(ti.tm_mday, ti.tm_mon + 1, false, true);
                    do_vegas = false;
                } else {
                    display_numbers(ti.tm_mday, ti.tm_mon + 1, false, true);
                }
                display_colon(false);
            } else {
                if (current_dots_effect() == DOTS_EFFECT_NONE) set_dots(0x00);
                if (use_vegas && do_vegas) {
                    vegas_display_numbers(ti.tm_hour, ti.tm_min, true, true);
                    do_vegas = false;
                } else {
                    display_numbers(ti.tm_hour, ti.tm_min, true, true);
                }
                if (!blink_colon) display_colon(true);
            }
            pti = ti;
        }
        if (blink_colon && !date_shown && (digits_effect == DIGITS_EFFECT_NONE || digits_effect == DIGITS_EFFECT_VEGAS)) {
            display_colon(ti.tm_sec % 2);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));
    }
}

#include <stdlib.h>
#include <time.h>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>

#include "clock.h"
#include "gpio.h"
#include "nixies.h"

// static const char* const T = "nixies";

static _Atomic uint8_t hour_buf;
static _Atomic uint8_t minute_buf;
static _Atomic uint8_t dots_buf;

// --------------------------------------------------------------------------------------------------------------------------------

static portMUX_TYPE display_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void commit_display_buffers() {
    taskENTER_CRITICAL(&display_spinlock);
    gpio_set_level(RCLK_PIN, 0);
    gpio_set_level(SRCLK_PIN, 0);
    ets_delay_us(1);
    for (int i = 7; i >= 0; --i) {
        gpio_set_level(HOUR_PIN, (hour_buf >> i) & 0x01);
        gpio_set_level(MINUTE_PIN, (minute_buf >> i) & 0x01);
        gpio_set_level(DOTS_PIN, (dots_buf >> i) & 0x01);
        ets_delay_us(1);
        gpio_set_level(SRCLK_PIN, 1);
        ets_delay_us(1);
        gpio_set_level(SRCLK_PIN, 0);
    }
    ets_delay_us(1);
    gpio_set_level(RCLK_PIN, 1);
    ets_delay_us(1);
    gpio_set_level(RCLK_PIN, 0);
    gpio_set_level(HOUR_PIN, 0);
    gpio_set_level(MINUTE_PIN, 0);
    gpio_set_level(DOTS_PIN, 0);
    taskEXIT_CRITICAL(&display_spinlock);
}

// --------------------------------------------------------------------------------------------------------------------------------

static uint8_t encode_number(int num, bool leading_zero) {
    if (num < 0) return 0xFF;
    uint8_t buf = (uint8_t)(num % 10) << 4;
    if (num < 10) {
        if (!leading_zero)
            buf |= 0x0F;
    } else {
        buf |= (uint8_t)(num / 10 % 10);
    }
    return buf;
}

// --------------------------------------------------------------------------------------------------------------------------------

void display_colon(bool colon) { gpio_set_level(COLON_PIN, colon ? 1 : 0); }

void display_numbers(int num1, int num2, bool zero1, bool zero2) {
    hour_buf = encode_number(num1, zero1);
    minute_buf = encode_number(num2, zero2);
    commit_display_buffers();
}

void display_number(int num, bool zero) {
    if (num < 0) {
        hour_buf = 0xFF;
        minute_buf = 0xFF;
    } else if (num < 100) {
        hour_buf = zero? 0x00 : 0xFF;
        minute_buf = encode_number(num, zero);
    } else {
        hour_buf = encode_number(num / 100 % 100, zero);
        minute_buf = encode_number(num % 100, true);
    }
    commit_display_buffers();
}

void display_digits(int8_t d1, int8_t d2, int8_t d3, int8_t d4) {
    hour_buf = (d2 < 0 ? 0xF0 : (uint8_t)(d2 % 10) << 4) | (d1 < 0 ? 0x0F : (uint8_t)((d1 % 10)));
    minute_buf = (d4 < 0 ? 0xF0 : (uint8_t)(d4 % 10) << 4) | (d3 < 0 ? 0x0F : (uint8_t)((d3 % 10)));
    commit_display_buffers();
}

void set_dots(uint8_t dots) {
    dots_buf = dots;
}

void display_dots(uint8_t dots) {
    dots_buf = dots;
    commit_display_buffers();
}

// --------------------------------------------------------------------------------------------------------------------------------

void progress_boot_dots(const char* msg) {
    static int step = 0;
    if (step > 8) return;
    ESP_LOGI("boot", "Boot progress: %d/8: %s", ++step, msg);
    dots_buf = ~(0xFF << step);
    commit_display_buffers();
}

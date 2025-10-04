#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "effects.h"
#include "mqtt.h"
#include "nixies.h"

const char* const TAG = "effects";

void stop_all_effects() {
    stop_dots_effect();
    stop_digits_effect();
}

// --------------------------------------------------------------------------------------------------------------------------------

static TaskHandle_t dots_effect_task_handle = NULL;
static _Atomic enum DotsEffect active_dots_effect = DOTS_EFFECT_NONE;

static void dots_blink_task(void* param) {
    for (;;) {
        display_dots(0xFF);
        vTaskDelay(pdMS_TO_TICKS(500));
        display_dots(0x00);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void dots_filling_right_task(void* param) {
    for (;;) {
        for (uint8_t i = 0; i < 8; ++i) {
            display_dots((1 << (i + 1)) - 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // for (int8_t i = 6; i >= -1; --i) {
        //     display_dots((1 << (i + 1)) - 1);
        //     vTaskDelay(pdMS_TO_TICKS(200));
        // }
        for (uint8_t i = 1; i <= 8; ++i) {
            display_dots(0xFF & (~((1 << i) - 1)));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void dots_filling_left_task(void* param) {
    for (;;) {
        for (int8_t i = 7; i >= 0; --i) {
            display_dots(0xFF & (~((1 << i) - 1)));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (uint8_t i = 1; i <= 8; ++i) {
            display_dots(0xFF & (~((1 << i) - 1)));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void dots_knight_rider_task(void* param) {
    int8_t pos = -2;
    int8_t dir = 1;
    for (;;) {
        uint8_t dots = 0;
        for (int8_t i = 0; i < 3; ++i) {
            int8_t dot = pos + i;
            if (dot >= 0 && dot < 8) {
                dots |= (1 << dot);
            }
        }
        display_dots(dots);
        vTaskDelay(pdMS_TO_TICKS(200));
        pos += dir;
        if (pos >= 7 || pos <= -2) dir = -dir;
    }
}

void start_dots_effect(enum DotsEffect effect) {
    if (dots_effect_task_handle != NULL) {
        vTaskDelete(dots_effect_task_handle);
        dots_effect_task_handle = NULL;
    }
    active_dots_effect = effect;
    switch (effect) {
        case DOTS_EFFECT_NONE:
            if (current_digits_effect() == DIGITS_EFFECT_OFF)
                display_dots(0x00);
            else
                force_clock_update = true;
            return;
        case DOTS_EFFECT_BLINKING:  //
            xTaskCreate(dots_blink_task, "dots_task", 2048, NULL, 5, &dots_effect_task_handle);
            return;
        case DOTS_EFFECT_FILLING_RIGHT:
            xTaskCreate(dots_filling_right_task, "dots_task", 2048, NULL, 5, &dots_effect_task_handle);
            return;
        case DOTS_EFFECT_FILLING_LEFT:
            xTaskCreate(dots_filling_left_task, "dots_task", 2048, NULL, 5, &dots_effect_task_handle);
            return;
        case DOTS_EFFECT_KNIGHT_RIDER:
            xTaskCreate(dots_knight_rider_task, "dots_task", 2048, NULL, 5, &dots_effect_task_handle);
            return;
        default: break;
    }
}

void stop_dots_effect() {
    active_dots_effect = DOTS_EFFECT_NONE;
    if (dots_effect_task_handle == NULL) return;
    vTaskDelete(dots_effect_task_handle);
    dots_effect_task_handle = NULL;
    display_dots(0x00);
}

enum DotsEffect current_dots_effect() { return active_dots_effect; }

// --------------------------------------------------------------------------------------------------------------------------------

static TaskHandle_t digits_task_handle = NULL;
static _Atomic enum DigitsEffect active_digits_effect = DIGITS_EFFECT_NONE;

static void digits_count_task(void* param) {
    uint8_t i = 0;
    for (;;) {
        display_digits(i, i, i, i);
        if (++i > 9) i = 0;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void digits_delayed_task(void* param) {
    display_digits(-1, -1, -1, -1);
    for (uint8_t d = 1; d <= 3; ++d) {
        display_digits(-1, -1, -1, d);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    for (uint8_t d = 4; d <= 6; ++d) {
        display_digits(-1, -1, d - 3, d);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    for (uint8_t d = 7; d <= 9; ++d) {
        display_digits(-1, d - 6, d - 3, d);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    for (int8_t d = 1;; ++d) {
        display_digits(d, (d + 3) % 10, (d + 6) % 10, (d + 9) % 10);
        if (d >= 9) d = -1;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void digits_random_task(void* param) {
    for (;;) {
        int8_t d1 = esp_random() % 10;
        int8_t d2 = esp_random() % 10;
        int8_t d3 = esp_random() % 10;
        int8_t d4 = esp_random() % 10;
        display_digits(d1, d2, d3, d4);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void digits_boot_effect() {
    for (uint8_t i = 0; i < 10; ++i) {
        display_digits(i, i, i, i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    display_digits(-1, -1, -1, -1);
}

struct VegasTaskParam {
    int8_t digit1;
    int8_t digit2;
    int8_t digit3;
    int8_t digit4;
    int cycles;
    int delay_cycles;
};

#define VEGAS_DELAY_MS 67
#define VEGAS_DELAY_CYCLES pdMS_TO_TICKS(VEGAS_DELAY_MS)

// void digits_vegas_task(void* param) {
//     TickType_t last_wake = xTaskGetTickCount();
//     struct VegasTaskParam* p = (struct VegasTaskParam*)param;
//     int cycles1 = p->cycles;
//     int cycles2 = p->cycles + p->delay_cycles;
//     int cycles3 = p->cycles + p->delay_cycles * 2;
//     int cycles4 = p->cycles + p->delay_cycles * 3;
//     for (int cycle = 0; cycle <= cycles4; ++cycle) {
//         int8_t d1 = (cycle < cycles1 && p->digit1 != -1) ? (esp_random() % 10) : p->digit1;
//         int8_t d2 = (cycle < cycles2 && p->digit2 != -1) ? (esp_random() % 10) : p->digit2;
//         int8_t d3 = (cycle < cycles3 && p->digit3 != -1) ? (esp_random() % 10) : p->digit3;
//         int8_t d4 = (cycle < cycles4 && p->digit4 != -1) ? (esp_random() % 10) : p->digit4;
//         display_digits(d1, d2, d3, d4);
//         vTaskDelayUntil(&last_wake, VEGAS_DELAY_CYCLES);
//     }
//     active_digits_effect = DIGITS_EFFECT_NONE;
//     digits_task_handle = NULL;
//     vTaskDelete(NULL);
// }

void digits_vegas_task(void* param) {
    TickType_t last_wake = xTaskGetTickCount();
    struct VegasTaskParam* p = (struct VegasTaskParam*)param;
    int d1 = p->digit1 - p->cycles;
    int d2 = p->digit2 + p->cycles + p->delay_cycles;
    int d3 = p->digit3 - p->cycles - p->delay_cycles * 2;
    int d4 = p->digit4 + p->cycles + p->delay_cycles * 3;
    for (int i = 0; i <= p->cycles; ++i, ++d1, --d2, ++d3, --d4) {
        display_digits(((d1 % 10) + 10) % 10, ((d2 % 10) + 10) % 10, ((d3 % 10) + 10) % 10, ((d4 % 10) + 10) % 10);
        vTaskDelayUntil(&last_wake, VEGAS_DELAY_CYCLES);
    }
    for (int i = 0; i < p->delay_cycles; ++i, --d2, ++d3, --d4) {
        display_digits(p->digit1, ((d2 % 10) + 10) % 10, ((d3 % 10) + 10) % 10, ((d4 % 10) + 10) % 10);
        vTaskDelayUntil(&last_wake, VEGAS_DELAY_CYCLES);
    }
    for (int i = 0; i < p->delay_cycles; ++i, ++d3, --d4) {
        display_digits(p->digit1, p->digit2, ((d3 % 10) + 10) % 10, ((d4 % 10) + 10) % 10);
        vTaskDelayUntil(&last_wake, VEGAS_DELAY_CYCLES);
    }
    for (int i = 0; i < p->delay_cycles; ++i, --d4) {
        display_digits(p->digit1, p->digit2, p->digit3, ((d4 % 10) + 10) % 10);
        vTaskDelayUntil(&last_wake, VEGAS_DELAY_CYCLES);
    }
    active_digits_effect = DIGITS_EFFECT_NONE;
    digits_task_handle = NULL;
    vTaskDelete(NULL);
}

void vegas_display_digits(int8_t digit1, int8_t digit2, int8_t digit3, int8_t digit4) {
    const int cycles = 24;
    const int delay_cycles = 6;

    if (digits_task_handle != NULL) {
        vTaskDelete(digits_task_handle);
        digits_task_handle = NULL;
    }
    active_digits_effect = DIGITS_EFFECT_VEGAS;
    static struct VegasTaskParam param;  // Static to persist across function calls
    param.digit1 = digit1;
    param.digit2 = digit2;
    param.digit3 = digit3;
    param.digit4 = digit4;
    param.cycles = cycles;
    param.delay_cycles = delay_cycles;
    xTaskCreate(digits_vegas_task, "digits_task", 2048, &param, 5, &digits_task_handle);
}

void vegas_display_numbers(int number1, int number2, bool zero1, bool zero2) {
    int8_t d1 = (number1 / 10) % 10;
    int8_t d2 = number1 % 10;
    if (!zero1 && d1 == 0) d1 = -1;
    int8_t d3 = (number2 / 10) % 10;
    int8_t d4 = number2 % 10;
    if (!zero2 && d3 == 0) d3 = -1;
    vegas_display_digits(d1, d2, d3, d4);
}

void start_digits_effect(enum DigitsEffect effect) {
    active_digits_effect = effect;
    if (digits_task_handle != NULL) {
        vTaskDelete(digits_task_handle);
        digits_task_handle = NULL;
    }
    if (effect == DIGITS_EFFECT_BOOT) {
        digits_boot_effect();
        active_digits_effect = DIGITS_EFFECT_NONE;
        return;
    }
    switch (effect) {
        case DIGITS_EFFECT_NONE:
            force_clock_update = true;
            do_vegas = true;
            return;
        case DIGITS_EFFECT_OFF: display_digits(-1, -1, -1, -1); break;
        case DIGITS_EFFECT_COUNT: xTaskCreate(digits_count_task, "digits_task", 2048, NULL, 5, &digits_task_handle); break;
        case DIGITS_EFFECT_DELAYED: xTaskCreate(digits_delayed_task, "digits_task", 2048, NULL, 5, &digits_task_handle); break;
        case DIGITS_EFFECT_RANDOM: xTaskCreate(digits_random_task, "digits_task", 2048, NULL, 5, &digits_task_handle); break;
        default: return;
    }
    display_colon(false);
    if (active_dots_effect == DOTS_EFFECT_NONE) display_dots(0x00);
}

void stop_digits_effect() {
    active_digits_effect = DIGITS_EFFECT_NONE;
    if (digits_task_handle == NULL) return;
    vTaskDelete(digits_task_handle);
    digits_task_handle = NULL;
    force_clock_update = true;
}

enum DigitsEffect current_digits_effect() { return active_digits_effect; }

// --------------------------------------------------------------------------------------------------------------------------------

_Atomic bool cathode_protection = true;

_Atomic bool cathode_protection_in_progress = false;

static _Atomic enum DigitsEffect saved_digits_effect;
static _Atomic enum DotsEffect saved_dots_effect;

void start_cathode_protection_cycle() {
    if (cathode_protection_in_progress || !cathode_protection) return;
    ESP_LOGI("EFFECTS", "Starting cathode protection cycle");
    saved_digits_effect = current_digits_effect();
    saved_dots_effect = current_dots_effect();
    cathode_protection_in_progress = true;
    mqtt_client_publish("cathode_protection_in_progress/state", "ON", 1);
    start_digits_effect(DIGITS_EFFECT_DELAYED);
    mqtt_client_publish("digits_effect/state", DIGITS_EFFECT_NAMES[DIGITS_EFFECT_DELAYED], 1);
    start_dots_effect(DOTS_EFFECT_KNIGHT_RIDER);
    mqtt_client_publish("dots_effect/state", DOTS_EFFECT_NAMES[DOTS_EFFECT_KNIGHT_RIDER], 1);
}

void stop_cathode_protection_cycle() {
    if (!cathode_protection_in_progress) return;
    ESP_LOGI("EFFECTS", "Stopping cathode protection cycle");
    cathode_protection_in_progress = false;
    mqtt_client_publish("cathode_protection_in_progress/state", "OFF", 1);
    start_digits_effect(saved_digits_effect);
    start_dots_effect(saved_dots_effect);
    mqtt_client_publish("digits_effect/state", DIGITS_EFFECT_NAMES[saved_digits_effect], 1);
    mqtt_client_publish("dots_effect/state", DOTS_EFFECT_NAMES[saved_dots_effect], 1);
}

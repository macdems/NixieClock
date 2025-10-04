#include "gpio.h"

void configure_gpio(uint8_t pin, bool initial_level) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),          // Select GPIO
        .mode = GPIO_MODE_OUTPUT,               // Set as output
        .pull_up_en = GPIO_PULLUP_DISABLE,      // Disable pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Disable pull-down
        .intr_type = GPIO_INTR_DISABLE          // Disable interrupts
    };
    gpio_config(&io_conf);
    gpio_set_level(pin, initial_level);
}

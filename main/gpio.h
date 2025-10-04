#ifndef NIXIE_CLOCK_GPIO_H
#define NIXIE_CLOCK_GPIO_H

#include <driver/gpio.h>

void configure_gpio(uint8_t pin, bool initial_level);

#endif // NIXIE_CLOCK_MAIN_GPIO_H

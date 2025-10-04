#ifndef NIXIE_CLOCK_LEDS_H
#define NIXIE_CLOCK_LEDS_H

void init_leds();

extern _Atomic bool leds_on;
extern _Atomic uint16_t leds_hue;
extern _Atomic uint8_t leds_saturation;
extern _Atomic uint8_t leds_value;

void update_leds();

bool handle_led_command(const char* topic, int topic_len, const char* data, int data_len);

void publish_initial_leds_state();

#endif  // NIXIE_CLOCK_LEDS_H

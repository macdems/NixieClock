#ifndef NIXIE_CLOCK_EFFECTS_H
#define NIXIE_CLOCK_EFFECTS_H

enum DotsEffect {
    DOTS_EFFECT_NONE = 0,
    DOTS_EFFECT_BLINKING = 1,
    DOTS_EFFECT_KNIGHT_RIDER = 2,
    DOTS_EFFECT_FILLING_RIGHT = 3,
    DOTS_EFFECT_FILLING_LEFT = 4,
};

enum DigitsEffect {
    DIGITS_EFFECT_NONE = 0,
    DIGITS_EFFECT_OFF = 1,
    DIGITS_EFFECT_COUNT = 2,
    DIGITS_EFFECT_DELAYED = 3,
    DIGITS_EFFECT_RANDOM = 4,
    DIGITS_EFFECT_VEGAS = 5,
    DIGITS_EFFECT_BOOT = 6,
    DIGITS_EFFECT_OTA = 7,
};

void start_dots_effect(enum DotsEffect effect);
void stop_dots_effect();
enum DotsEffect current_dots_effect();

void start_digits_effect(enum DigitsEffect effect);
void stop_digits_effect();
enum DigitsEffect current_digits_effect();

void vegas_display_digits(int8_t digit1, int8_t digit2, int8_t digit3, int8_t digit4);
void vegas_display_numbers(int number1, int number2, bool zero1, bool zero2);

void stop_all_effects();

extern _Atomic bool cathode_protection;
extern _Atomic bool cathode_protection_in_progress;

void start_cathode_protection_cycle();
void stop_cathode_protection_cycle();


#endif // NIXIE_CLOCK_EFFECTS_H

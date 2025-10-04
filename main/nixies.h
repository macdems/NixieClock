#ifndef NIXIES_H
#define NIXIES_H

#define HOUR_PIN GPIO_NUM_0
#define MINUTE_PIN GPIO_NUM_5
#define DOTS_PIN GPIO_NUM_20
#define COLON_PIN GPIO_NUM_10
#define SRCLK_PIN GPIO_NUM_6
#define RCLK_PIN GPIO_NUM_7

void display_colon(bool colon);

void display_numbers(int num1, int num2, bool zero1, bool zero2);
void display_number(int num, bool zero);
void display_digits(int8_t d1, int8_t d2, int8_t d3, int8_t d4);

void set_dots(uint8_t dots);
void display_dots(uint8_t dots);
void progress_boot_dots(const char* msg);

#endif // NIXIES_H

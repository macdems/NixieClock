#ifndef NIXIE_CLOCK_MQTT_H
#define NIXIE_CLOCK_MQTT_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define MQTT_READY_BIT BIT0  // EventGroup bit for MQTT connected

extern EventGroupHandle_t mqtt_evg_handle;

extern const char* DOTS_EFFECT_NAMES[];
extern const char* DIGITS_EFFECT_NAMES[];

int mqtt_client_publish(const char* topic, const char* payload, int retain);

void init_mqtt_client();

void copy_str_to_buffer(const char* src, int src_size, char* dest, int dest_size);

#endif  // NIXIE_CLOCK_MQTT_H

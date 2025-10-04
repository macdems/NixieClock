#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mqtt_client.h>

#include <time.h>

#include "clock.h"
#include "effects.h"
#include "leds.h"
#include "log.h"
#include "mqtt.h"
#include "nixies.h"
#include "nvs.h"
#include "ota.h"

static const char* const TAG = "mqtt";

#define RSSI_PUBLISH_INTERVAL_S 60
#define DISCOVERY_PUBLISH_INTERVAL_S 15*60

static esp_mqtt_client_handle_t mqtt_client = NULL;

EventGroupHandle_t mqtt_evg_handle = NULL;

#if CONFIG_NODE_ADD_SUFFIX
static char NODE_ID[32] = CONFIG_NODE_NAMESPACE;
#else
#    define NODE_ID CONFIG_NODE_NAMESPACE
#endif
static char DEVICE_IDENTIFIER[64] = CONFIG_NODE_NAMESPACE "-001";  // appears in device.identifiers
static size_t NODE_ID_LEN = 0;
static char AVAIL_TOPIC[64];

static char _TOPIC[256];

const char* DOTS_EFFECT_NAMES[] = {"Brak", "Miganie", "Nieustraszony", "Wypełnienie", NULL};
const char* DIGITS_EFFECT_NAMES[] = {"Brak", "Wyłącz", "Zmieniaj razem", "Zmieniaj z opóźnieniem", "Zmieniaj losowo", NULL};

extern const uint8_t lets_encrypt_cert_pem_start[] asm("_binary_letsencrypt_cert_pem_start");
extern const uint8_t lets_encrypt_cert_pem_end[] asm("_binary_letsencrypt_cert_pem_end");

void copy_str_to_buffer(const char* src, int src_size, char* dest, int dest_size) {
    const int n = src_size < dest_size - 1 ? src_size : dest_size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
}

int mqtt_client_publish(const char* topic, const char* payload, int retain) {
    if (mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT client not initialized, cannot publish '%s'", topic);
        return -1;
    }
    int msg_id;
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/%s", NODE_ID, topic);
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, retain);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published %s'%s'", retain ? "(retained) " : "", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
    return msg_id;
}

int mqtt_client_publish_log(const char* topic, const char* payload, int len) {
    if (mqtt_client == NULL) return -1;
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/%s", NODE_ID, topic);
    return esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, len, 0, 0);
}

void publish_rssi(void*) {
    wifi_ap_record_t ap;
    esp_wifi_sta_get_ap_info(&ap);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", ap.rssi);
    mqtt_client_publish("rssi/state", buf, 1);
}

static void finish_discovery_payload(char* payload) {
    char* ptr = payload + strlen(payload);
    // clang-format off
    ptr += snprintf(ptr, 1024 - (ptr - payload),
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"manufacturer\":\"MacDems\","
            "\"model\":\"Nixie-Clock-IN-14\","
            "\"name\":\"Zegar Nixie IN-14\""
        "}}",
        AVAIL_TOPIC, DEVICE_IDENTIFIER
    );
    // clang-format on
}

static void publish_switch_discovery(const char* obj_id, const char* name, const char* icon) {
    char payload[1024];
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/switch/%s/%s/config", CONFIG_MQTT_DISCOVERY_PREFIX, NODE_ID, obj_id);
    // clang-format off
    snprintf(payload, sizeof(payload), "{"
        "\"name\":\"%1$s\","
        "\"state_topic\":\"%2$s/%3$s/state\","
        "\"command_topic\":\"%2$s/%3$s/set\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"state_on\":\"ON\","
        "\"state_off\":\"OFF\","
        "\"icon\":\"%4$s\","
        "\"unique_id\":\"%2$s_%3$s\",",
        name, NODE_ID, obj_id, icon
    );
    // clang-format on
    finish_discovery_payload(payload);
    int msg_id;
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published (retained)'%s'", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
}

static void publish_leds_discovery(const char* obj_id, const char* name) {
    char payload[1024];
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/light/%s/%s/config", CONFIG_MQTT_DISCOVERY_PREFIX, NODE_ID, obj_id);
    // clang-format off
    snprintf(payload, sizeof(payload), "{"
        "\"name\":\"%1$s\","
        "\"retain\":true,"
        "\"brightness\":true,"
        "\"supported_color_modes\":[\"hs\"],"
        "\"state_topic\":\"%2$s/%3$s/state\","
        "\"command_topic\":\"%2$s/%3$s/set\","
        "\"brightness_state_topic\":\"%2$s/%3$s/brightness/state\","
        "\"brightness_command_topic\":\"%2$s/%3$s/brightness/set\","
        // "\"rgb_state_topic\":\"%2$s/%3$s/rgb/state\","
        // "\"rgb_command_topic\":\"%2$s/%3$s/rgb/set\","
        "\"hs_state_topic\":\"%2$s/%3$s/hs/state\","
        "\"hs_command_topic\":\"%2$s/%3$s/hs/set\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"unique_id\":\"%2$s/%3$s\",",
        name,
        NODE_ID, obj_id
    );
    // clang-format on
    finish_discovery_payload(payload);
    esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    int msg_id;
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published (retained)'%s'", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
}

static void publish_select_discovery(const char* obj_id, const char* name, const char* options[], const char* icon) {
    char payload[1024];
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/select/%s/%s/config", CONFIG_MQTT_DISCOVERY_PREFIX, NODE_ID, obj_id);
    // clang-format off
    snprintf(payload, sizeof(payload), "{"
        "\"name\":\"%1$s\","
        "\"state_topic\":\"%2$s/%3$s/state\","
        "\"command_topic\":\"%2$s/%3$s/set\","
        "\"unique_id\":\"%2$s_%3$s\","
        "\"icon\":\"%4$s\","
        "\"options\":[",
        name, NODE_ID, obj_id, icon
    );
    char* ptr = payload + strlen(payload);
    for (int i = 0; options[i] != NULL; ++i) {
        if (i > 0) ptr += snprintf(ptr, 1024 - (ptr - payload), ",");
        ptr += snprintf(ptr, 1024 - (ptr - payload), "\"%s\"", options[i]);
    }
    ptr += snprintf(ptr, 1024 - (ptr - payload), "],");
    // clang-format on
    finish_discovery_payload(payload);
    int msg_id;
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published (retained)'%s'", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
}

static void publish_sensor_discovery(const char* obj_id,
                                     const char* name,
                                     const char* unit,
                                     const char* device_class,
                                     const char* entity_category) {
    char payload[1024];
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/sensor/%s/%s/config", CONFIG_MQTT_DISCOVERY_PREFIX, NODE_ID, obj_id);
    // clang-format off
    snprintf(payload, sizeof(payload), "{"
        "\"name\":\"%1$s\","
        "\"state_topic\":\"%2$s/%3$s/state\","
        "\"unit_of_measurement\":\"%4$s\","
        "\"device_class\":\"%5$s\","
        "\"unique_id\":\"%2$s_%3$s\","
        "\"state_class\":\"measurement\","
        "\"entity_category\":\"%6$s\",",
        name, NODE_ID, obj_id, unit, device_class, entity_category
    );
    // clang-format on
    finish_discovery_payload(payload);
    int msg_id;
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published (retained)'%s'", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
}

static void publish_binary_sensor_discovery(const char* obj_id,
                                         const char* name,
                                         const char* device_class,
                                         const char* entity_category) {
    char payload[1024];
    snprintf(_TOPIC, sizeof(_TOPIC), "%s/binary_sensor/%s/%s/config", CONFIG_MQTT_DISCOVERY_PREFIX, NODE_ID, obj_id);
    // clang-format off
    snprintf(payload, sizeof(payload), "{"
        "\"name\":\"%1$s\","
        "\"state_topic\":\"%2$s/%3$s/state\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"device_class\":\"%4$s\","
        "\"unique_id\":\"%2$s_%3$s\","
        "\"entity_category\":\"%5$s\",",
        name, NODE_ID, obj_id, device_class, entity_category
    );
    // clang-format on
    finish_discovery_payload(payload);
    int msg_id;
    msg_id = esp_mqtt_client_publish(mqtt_client, _TOPIC, payload, 0, 1, 1);
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "Published (retained)'%s'", _TOPIC);
    } else {
        ESP_LOGE(TAG, "Publish failed for '%s'", _TOPIC);
    }
}

static void publish_discovery_configs(void*) {
    // Publish all discovery configs here
    publish_switch_discovery("show_date", "Wyświetl datę", "mdi:calendar");
    publish_switch_discovery("blink_colon", "Migający dwukropek", "mdi:clock-digital");
    publish_switch_discovery("vegas_effect", "Efekt Vegas", "mdi:creation");
    publish_leds_discovery("leds", "Podświetlenie LED");
    publish_switch_discovery("cathode_protection", "Ochrona katod", "mdi:shield-sun-outline");
    publish_binary_sensor_discovery("cathode_protection_in_progress", "Ochrona katod w toku", "running", "diagnostic");
    publish_select_discovery("dots_effect", "Efekt kropek", DOTS_EFFECT_NAMES, "mdi:dots-horizontal");
    publish_select_discovery("digits_effect", "Efekt cyfr", DIGITS_EFFECT_NAMES, "mdi:numeric");
    publish_sensor_discovery("rssi", "RSSI", "dBm", "signal_strength", "diagnostic");
}

static void echo_states() {
    mqtt_client_publish("show_date/state", date_shown ? "ON" : "OFF", 1);
    mqtt_client_publish("blink_colon/state", blink_colon ? "ON" : "OFF", 1);
    mqtt_client_publish("vegas_effect/state", use_vegas ? "ON" : "OFF", 1);
    mqtt_client_publish("cathode_protection/state", cathode_protection ? "ON" : "OFF", 1);
    mqtt_client_publish("cathode_protection_in_progress/state",
                        cathode_protection_in_progress ? "ON" : "OFF", 1);
    enum DotsEffect doe = current_dots_effect();
    if (doe < DOTS_EFFECT_FILLING_LEFT)
        mqtt_client_publish("dots_effect/state", DOTS_EFFECT_NAMES[doe], 1);
    else
        mqtt_client_publish("dots_effect/state", DOTS_EFFECT_NAMES[0], 1);
    enum DigitsEffect die = current_digits_effect();
    if (die < DIGITS_EFFECT_VEGAS)
        mqtt_client_publish("digits_effect/state", DIGITS_EFFECT_NAMES[die], 1);
    else
        mqtt_client_publish("digits_effect/state", DIGITS_EFFECT_NAMES[0], 1);
    publish_initial_leds_state();
}

static void handle_incoming_command(const char* topic, int topic_len, const char* data, int data_len);

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;

    static bool first_connect = true;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            if (mqtt_evg_handle) xEventGroupSetBits(mqtt_evg_handle, MQTT_READY_BIT);

            if (first_connect) {
                first_connect = false;
                publish_rssi(NULL);
                progress_boot_dots("MQTT connected");  // (7)
            }
            ESP_LOGI(TAG, "MQTT connected");
            // Publish birth + discovery (retained) and subscribe to command topics
            mqtt_client_publish("status", "online", 1);
            publish_discovery_configs(NULL);

            snprintf(_TOPIC, sizeof(_TOPIC), "%s/+/set", NODE_ID);
            esp_mqtt_client_subscribe(mqtt_client, _TOPIC, 1);
            snprintf(_TOPIC, sizeof(_TOPIC), "%s/+/+/set", NODE_ID);
            esp_mqtt_client_subscribe(mqtt_client, _TOPIC, 1);

            // Immediately publish current states so HA shows correct values
            force_clock_update = true;
            echo_states();
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "MQTT data: topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len, event->data);
            handle_incoming_command(event->topic + NODE_ID_LEN + 1, event->topic_len - NODE_ID_LEN - 1, event->data,
                                    event->data_len);
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (mqtt_evg_handle) xEventGroupClearBits(mqtt_evg_handle, MQTT_READY_BIT);
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        default: break;
    }
}

void init_periodic_timer(const char* name, esp_timer_cb_t callback, uint64_t period_s) {
        esp_timer_handle_t timer;
        esp_timer_create_args_t timer_args = {.callback = callback, .name = name};
        if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
            // Convert seconds to microseconds
            if (esp_timer_start_periodic(timer, period_s * 1000000LL) == ESP_OK) {
                ESP_LOGD(TAG, "Started periodic timer %s (%llu s)", name, period_s);
            } else {
                ESP_LOGW(TAG, "Failed to start timer %s", name);
            }
        } else {
            ESP_LOGW(TAG, "Failed to create timer %s", name);
        }
    }

void init_mqtt_client() {
    // Use MAC to build stable IDs
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#if CONFIG_NODE_ADD_SUFFIX
    snprintf(NODE_ID, sizeof(NODE_ID), CONFIG_NODE_NAMESPACE "-%02x%02x%02x", mac[3], mac[4], mac[5]);
#endif
    NODE_ID_LEN = strlen(NODE_ID);
    ESP_LOGD(TAG, "Node ID: %s [%d]", NODE_ID, NODE_ID_LEN);
    snprintf(AVAIL_TOPIC, sizeof(AVAIL_TOPIC), "%s/status", NODE_ID);
    snprintf(DEVICE_IDENTIFIER, sizeof(DEVICE_IDENTIFIER), CONFIG_NODE_NAMESPACE "-%02x%02x%02x-%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);

    // MQTT client config with LWT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URI,
        .broker.verification.certificate = (const char*)lets_encrypt_cert_pem_start,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will.topic = AVAIL_TOPIC,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        // Optional: client_id; if NULL, IDF builds one. You can also set `.network.disable_auto_reconnect=false` by default.
    };

    vTaskDelay(pdMS_TO_TICKS(3000));  // wait for wifi to stabilize

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    // Start periodic RSSI publisher (every 60 seconds)
    init_periodic_timer("rssi_publisher", publish_rssi, RSSI_PUBLISH_INTERVAL_S);

    // Start periodic discovery publisher (every 3600 seconds)
    init_periodic_timer("discovery_publisher", publish_discovery_configs, DISCOVERY_PUBLISH_INTERVAL_S);
}

#define TEST_MQTT_SWITCH(name, variable, ...)                \
    if (strncmp(topic, name "/set", topic_len) == 0) {       \
        variable = data_len >= 2 && !strncmp(data, "ON", 2); \
        const char* state = variable ? "ON" : "OFF";         \
        mqtt_client_publish(name "/state", state, 1);        \
        ESP_LOGI(TAG, "'" name "' set -> %s", state);        \
        __VA_ARGS__                                          \
        force_clock_update = true;                           \
        return;                                              \
    }

static int get_effect_index(const char* data, int data_len, const char* const options[]) {
    for (int i = 0; options[i] != NULL; ++i) {
        int opt_len = strlen(options[i]);
        if (data_len == opt_len && strncmp(data, options[i], opt_len) == 0) {
            return i;
        }
    }
    return -1;
}

static void handle_incoming_command(const char* topic, int topic_len, const char* data, int data_len) {
    // Match commands by topic

    TEST_MQTT_SWITCH("show_date", date_shown, do_vegas = true;);

    TEST_MQTT_SWITCH("blink_colon", blink_colon, save_state_to_nvs(););

    TEST_MQTT_SWITCH("vegas_effect", use_vegas, do_vegas = true; save_state_to_nvs(););

    TEST_MQTT_SWITCH("cathode_protection", cathode_protection, save_state_to_nvs();
                     if (!cathode_protection) stop_cathode_protection_cycle(); else start_cathode_protection_cycle(););

    if (strncmp(topic, "leds/", (topic_len < 5) ? topic_len : 5) == 0 && handle_led_command(topic, topic_len, data, data_len))
        return;

    if (strncmp(topic, "dots_effect/set", topic_len) == 0) {
        int idx = get_effect_index(data, data_len, DOTS_EFFECT_NAMES);
        if (idx >= 0) {
            start_dots_effect((enum DotsEffect)idx);
            mqtt_client_publish("dots_effect/state", DOTS_EFFECT_NAMES[idx], 1);
            ESP_LOGI(TAG, "Dots effect set -> %s", DOTS_EFFECT_NAMES[idx]);
        }
        return;
    }

    if (strncmp(topic, "digits_effect/set", topic_len) == 0) {
        int idx = get_effect_index(data, data_len, DIGITS_EFFECT_NAMES);
        if (idx >= 0) {
            stop_cathode_protection_cycle();
            start_digits_effect((enum DigitsEffect)idx);
            mqtt_client_publish("digits_effect/state", DIGITS_EFFECT_NAMES[idx], 1);
            ESP_LOGI(TAG, "Digits effect set -> %s", DIGITS_EFFECT_NAMES[idx]);
        }
        return;
    }

    if (strncmp(topic, "firmware/set", topic_len) == 0) {
        char* url = (char*)calloc(1, data_len + 1);
        if (url) {
            memcpy(url, data, data_len);
            url[data_len] = 0;
            ESP_LOGI(TAG, "Starting OTA from URL: %s", url);
            xTaskCreate(ota_task, "ota_task", 8192, url, 5, NULL);
        }
        return;
    }
}

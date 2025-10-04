#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "effects.h"
#include "mqtt.h"
#include "nixies.h"
#include "ota.h"

static const char* const TAG = "ota";

static const char* const FW_STATUS_TOPIC = "firmware/status";
static const char* const FW_PROGRESS_TOPIC = "firmware/progress";

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static int64_t fw_total_len = -1;  // from HTTP header if known

static esp_err_t http_evt_handler(esp_http_client_event_t* evt) {
    // After headers are parsed, content length becomes available (or -1 if chunked)
    if (evt->event_id == HTTP_EVENT_ON_DATA && fw_total_len == -1) {
        int64_t len = esp_http_client_get_content_length(evt->client);
        if (len > 0) {
            fw_total_len = len;
            ESP_LOGI(TAG, "Firmware total length: %lld KiB", (fw_total_len + 512) / 1024);
        } else {
            fw_total_len = -2;
            ESP_LOGI(TAG, "Firmware total length: unknown (chunked)");
        }
    }
    return ESP_OK;
}

void ota_task(void* param) {
    char url[256] = {0};
    if (param) {
        strncpy(url, (const char*)param, sizeof(url) - 1);
        free(param);
    }

    start_digits_effect(DIGITS_EFFECT_OTA);
    start_dots_effect(DOTS_EFFECT_FILLING_LEFT);
    display_colon(false);

    fw_total_len = -1;
    char msg[320];
    snprintf(msg, sizeof(msg), "Starting OTA from %s", url);
    mqtt_client_publish(FW_STATUS_TOPIC, msg, 0);
    mqtt_client_publish(FW_PROGRESS_TOPIC, "0", 0);

    esp_http_client_config_t http_cfg = {.url = url,
                                         .timeout_ms = 20000,
                                         .keep_alive_enable = true,
                                         .event_handler = http_evt_handler,
                                         .cert_pem = (char*)ca_cert_pem_start,
                                         .skip_cert_common_name_check = true};
    esp_https_ota_config_t ota_cfg = {.http_config = &http_cfg};
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed (%s)", esp_err_to_name(err));
        mqtt_client_publish(FW_STATUS_TOPIC, "OTA begin failed", 0);
        stop_all_effects();
        vTaskDelete(NULL);
        return;
    }

    int last_pct = -1;
    for (;;) {
        err = esp_https_ota_perform(handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int bytes = esp_https_ota_get_image_len_read(handle);
            if (fw_total_len > 0) {
                int pct = (int)((bytes * 100) / fw_total_len);
                if (pct != last_pct) {
                    display_number(pct, false);
                    if (pct % 5 == 0) {
                        char p[8];
                        snprintf(p, sizeof(p), "%d", pct);
                        mqtt_client_publish(FW_PROGRESS_TOPIC, p, 0);
                    }
                    last_pct = pct;
                }
            } else {
                if (bytes % (32 * 1024) == 0) {
                    char b[16];
                    snprintf(b, sizeof(b), "%d", bytes);
                    mqtt_client_publish(FW_PROGRESS_TOPIC, b, 0);
                }
            }
            continue;
        }
        break;
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        ESP_LOGE(TAG, "OTA failed (%s)", esp_err_to_name(err));
        mqtt_client_publish(FW_STATUS_TOPIC, "OTA perform failed", 0);
        stop_all_effects();
        vTaskDelete(NULL);
        return;
    }
    err = esp_https_ota_finish(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA complete, restarting...");
        mqtt_client_publish(FW_STATUS_TOPIC, "OTA success. Rebooting…", 0);
        mqtt_client_publish(FW_PROGRESS_TOPIC, "100", 0);
        set_dots(0x00);
        display_number(-1, false);  // clear display
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA finish failed (%s)", esp_err_to_name(err));
        mqtt_client_publish(FW_STATUS_TOPIC, "OTA finish failed", 0);
    }
    stop_all_effects();
    vTaskDelete(NULL);
}

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)

static esp_timer_handle_t ota_cancel_rollback_timer = NULL;

static void ota_cancel_rollback_task(void* param) {
    // Wait until MQTT is ready, which is the ultimate indication of successful update
    xEventGroupWaitBits(mqtt_evg_handle, MQTT_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "App is valid, rollback cancelled successfully");
    } else {
        ESP_LOGE(TAG, "Failed to cancel rollback");
    }
}

void ota_cancel_rollback() {
    /**
     * We are treating successful WiFi connection as a checkpoint to cancel rollback
     * process and mark newly updated firmware image as active. For production cases,
     * please tune the checkpoint behavior per end application requirement.
     */
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (ota_cancel_rollback_timer == NULL) {
                esp_timer_create_args_t timer_args = {
                    .callback = ota_cancel_rollback_task, .arg = NULL, .name = "ota_cancel_rollback_timer"};
                ESP_ERROR_CHECK(esp_timer_create(&timer_args, &ota_cancel_rollback_timer));
            }
            ESP_ERROR_CHECK(esp_timer_start_once(ota_cancel_rollback_timer, 30 * 1000000LL));  // 30 seconds
        }
    }
}

#endif

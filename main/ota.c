#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <unistd.h>

#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <psa/crypto.h>
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

static portMUX_TYPE ota_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ota_in_progress = false;

static bool ota_lock(void) {
    bool ok;
    portENTER_CRITICAL(&ota_mux);
    ok = !ota_in_progress;
    if (ok) ota_in_progress = true;
    portEXIT_CRITICAL(&ota_mux);
    return ok;
}

static void ota_unlock(void) {
    portENTER_CRITICAL(&ota_mux);
    ota_in_progress = false;
    portEXIT_CRITICAL(&ota_mux);
}

static bool recv_all(int sock, void* buffer, size_t len) {
    uint8_t* p = buffer;
    while (len > 0) {
        int n = recv(sock, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_all(int sock, const void* buffer, size_t len) {
    const uint8_t* p = buffer;
    while (len > 0) {
        int n = send(sock, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

/*
 * Push wire format (all integer fields are big-endian):
 *   4 bytes "NIXO", 1 byte version (=1), 12-byte GCM nonce,
 *   4-byte encrypted image length, encrypted image, 16-byte GCM tag.
 * The first 21 bytes are GCM additional authenticated data (AAD).
 * The shared key is exactly 32 bytes, represented as 64 hex digits in Kconfig.
 * Ciphertext is decrypted incrementally into the inactive OTA partition. The
 * partition is selected for boot only after the final GCM tag is authenticated.
 */
#define OTA_PUSH_HEADER_SIZE 21
#define OTA_PUSH_NONCE_SIZE 12
#define OTA_PUSH_TAG_SIZE 16
#define OTA_PUSH_CHUNK_SIZE 1024
#define OTA_PUSH_MAX_IMAGE_SIZE (2 * 1024 * 1024)

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_push_key(uint8_t key[32]) {
    const char* text = CONFIG_OTA_PUSH_KEY;
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        int hi = hex_nibble(text[i * 2]);
        int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            memset(key, 0, 32);
            return false;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void ota_push_connection_task(void* param) {
    int sock = (int)(intptr_t)param;
    struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint8_t header[OTA_PUSH_HEADER_SIZE];
    uint8_t key[32] = {0};
    uint8_t tag[OTA_PUSH_TAG_SIZE];
    uint8_t input[OTA_PUSH_CHUNK_SIZE];
    uint8_t output[OTA_PUSH_CHUNK_SIZE + 16];
    uint32_t image_len = 0;
    uint32_t offset = 0;
    uint32_t written_len = 0;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    bool locked = false;
    psa_key_id_t key_id = 0;
    bool key_imported = false;
    psa_aead_operation_t aead = PSA_AEAD_OPERATION_INIT;
    bool aead_initialized = false;
    psa_status_t crypto_ret = PSA_SUCCESS;
    esp_err_t err = ESP_FAIL;

    if (!recv_all(sock, header, sizeof(header)) || memcmp(header, "NIXO", 4) != 0 || header[4] != 1) {
        ESP_LOGW(TAG, "Invalid OTA push header");
        send_all(sock, "ERR header\n", 11);
        goto cleanup;
    }

    image_len = ((uint32_t)header[17] << 24) | ((uint32_t)header[18] << 16) |
                ((uint32_t)header[19] << 8) | header[20];
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (image_len == 0 || image_len > OTA_PUSH_MAX_IMAGE_SIZE || update_partition == NULL ||
        image_len > update_partition->size) {
        ESP_LOGW(TAG, "Invalid OTA push image length: %u", (unsigned)image_len);
        send_all(sock, "ERR length\n", 11);
        goto cleanup;
    }

    if (!parse_push_key(key)) {
        ESP_LOGE(TAG, "OTA push key is missing or invalid; push OTA disabled");
        send_all(sock, "ERR disabled\n", 13);
        goto cleanup;
    }
    if (!ota_lock()) {
        ESP_LOGW(TAG, "OTA already in progress; rejecting push");
        send_all(sock, "ERR busy\n", 9);
        goto cleanup;
    }
    locked = true;

    crypto_ret = psa_crypto_init();
    if (crypto_ret == PSA_SUCCESS) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
        crypto_ret = psa_import_key(&attributes, key, sizeof(key), &key_id);
        psa_reset_key_attributes(&attributes);
        key_imported = crypto_ret == PSA_SUCCESS;
    }
    if (crypto_ret == PSA_SUCCESS) {
        crypto_ret = psa_aead_decrypt_setup(&aead, key_id, PSA_ALG_GCM);
        aead_initialized = crypto_ret == PSA_SUCCESS;
    }
    if (crypto_ret == PSA_SUCCESS) {
        crypto_ret = psa_aead_set_nonce(&aead, header + 5, OTA_PUSH_NONCE_SIZE);
    }
    if (crypto_ret == PSA_SUCCESS) {
        crypto_ret = psa_aead_set_lengths(&aead, sizeof(header), image_len);
    }
    if (crypto_ret == PSA_SUCCESS) {
        crypto_ret = psa_aead_update_ad(&aead, header, sizeof(header));
    }
    if (crypto_ret != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Unable to initialize OTA push decryption (%d)", (int)crypto_ret);
        send_all(sock, "ERR crypto\n", 11);
        goto cleanup;
    }

    err = esp_ota_begin(update_partition, image_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA push begin failed (%s)", esp_err_to_name(err));
        send_all(sock, "ERR begin\n", 10);
        goto cleanup;
    }
    ota_started = true;

    start_digits_effect(DIGITS_EFFECT_OTA);
    start_dots_effect(DOTS_EFFECT_FILLING_LEFT);
    display_colon(false);
    mqtt_client_publish(FW_STATUS_TOPIC, "Starting authenticated push OTA", 0);
    mqtt_client_publish(FW_PROGRESS_TOPIC, "0", 0);

    while (offset < image_len) {
        size_t chunk_len = image_len - offset;
        if (chunk_len > sizeof(input)) chunk_len = sizeof(input);
        if (!recv_all(sock, input, chunk_len)) {
            ESP_LOGW(TAG, "OTA push connection ended before full image arrived");
            send_all(sock, "ERR transfer\n", 13);
            goto cleanup;
        }
        size_t output_len = 0;
        crypto_ret = psa_aead_update(&aead, input, chunk_len, output, sizeof(output), &output_len);
        if (crypto_ret != PSA_SUCCESS) {
            ESP_LOGE(TAG, "OTA push decrypt failed (%d)", (int)crypto_ret);
            send_all(sock, "ERR crypto\n", 11);
            goto cleanup;
        }
        if (output_len > 0) {
            err = esp_ota_write(ota_handle, output, output_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA push write failed (%s)", esp_err_to_name(err));
                send_all(sock, "ERR write\n", 10);
                goto cleanup;
            }
            written_len += output_len;
        }
        offset += chunk_len;
        int pct = (int)(((uint64_t)offset * 100) / image_len);
        display_number(pct, false);
        if (pct % 5 == 0) {
            char p[10];
            snprintf(p, sizeof(p), "%d", pct);
            mqtt_client_publish(FW_PROGRESS_TOPIC, p, 0);
        }
    }

    if (!recv_all(sock, tag, sizeof(tag))) {
        ESP_LOGW(TAG, "OTA push tag missing");
        send_all(sock, "ERR transfer\n", 13);
        goto cleanup;
    }
    size_t tail_len = 0;
    crypto_ret = psa_aead_verify(&aead, output, sizeof(output), &tail_len, tag, sizeof(tag));
    if (crypto_ret != PSA_SUCCESS) {
        ESP_LOGW(TAG, "OTA push authentication failed (%d)", (int)crypto_ret);
        send_all(sock, "ERR auth\n", 9);
        goto cleanup;
    }
    aead_initialized = false;  // psa_aead_verify() ended the operation successfully
    if (tail_len > 0) {
        err = esp_ota_write(ota_handle, output, tail_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA push final write failed (%s)", esp_err_to_name(err));
            send_all(sock, "ERR write\n", 10);
            goto cleanup;
        }
        written_len += tail_len;
    }
    if (written_len != image_len) {
        ESP_LOGE(TAG, "OTA push plaintext length mismatch (%u != %u)", (unsigned)written_len, (unsigned)image_len);
        send_all(sock, "ERR length\n", 11);
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    ota_started = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA push image validation failed (%s)", esp_err_to_name(err));
        send_all(sock, "ERR image\n", 10);
        goto cleanup;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA push boot partition failed (%s)", esp_err_to_name(err));
        send_all(sock, "ERR boot\n", 9);
        goto cleanup;
    }

    send_all(sock, "OK rebooting\n", 13);
    ESP_LOGI(TAG, "Authenticated push OTA complete, restarting...");
    mqtt_client_publish(FW_STATUS_TOPIC, "OTA success. Rebooting…", 0);
    mqtt_client_publish(FW_PROGRESS_TOPIC, "100", 0);
    set_dots(0x00);
    display_number(-1, false);
    ota_unlock();
    locked = false;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

cleanup:
    if (ota_started) esp_ota_abort(ota_handle);
    if (aead_initialized) psa_aead_abort(&aead);
    if (key_imported) psa_destroy_key(key_id);
    memset(key, 0, sizeof(key));
    memset(tag, 0, sizeof(tag));
    memset(input, 0, sizeof(input));
    memset(output, 0, sizeof(output));
    if (locked) {
        stop_all_effects();
        ota_unlock();
    }
    close(sock);
    vTaskDelete(NULL);
}

void ota_push_server_start(void) {
#if CONFIG_OTA_PUSH_PORT > 0
    uint8_t key[32];
    if (!parse_push_key(key)) {
        ESP_LOGW(TAG, "OTA push server disabled: configure a 64-digit hex AES-256 key");
        return;
    }
    memset(key, 0, sizeof(key));

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "OTA push socket failed: errno %d", errno);
        return;
    }
    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_OTA_PUSH_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "OTA push listen failed on port %d: errno %d", CONFIG_OTA_PUSH_PORT, errno);
        close(listen_sock);
        return;
    }
    ESP_LOGI(TAG, "Secure OTA push listener started on TCP port %d", CONFIG_OTA_PUSH_PORT);
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int sock = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGW(TAG, "OTA push accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xTaskCreate(ota_push_connection_task, "ota_push", 8192, (void*)(intptr_t)sock, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA push task");
            close(sock);
        }
    }
#endif
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include "log.h"
#include "mqtt.h"

#define RB_CAPACITY_BYTES (32 * 1024)  // 32 KiB ringbuffer for log lines
#define LOG_LINE_MAX 256
#define LINE_ACCUM_MAX 1024  // how much we can accumulate before dropping oldest
#define LOG_MQTT_TOPIC "debug"

static const char* TAG = "log";

static RingbufHandle_t ringbuf_handle = NULL;

int mqtt_client_publish_log(const char* topic, const char* payload, int len);

// ---------- Helpers ----------
static inline bool in_isr(void) { return xPortInIsrContext(); }

// Drop the oldest item from the ringbuffer (ISR-safe variant)
static inline void rb_drop_oldest_from_isr(void) {
    size_t old_sz;
    void* old = xRingbufferReceiveFromISR(ringbuf_handle, &old_sz);
    if (old) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vRingbufferReturnItemFromISR(ringbuf_handle, old, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

// Drop the oldest item from the ringbuffer (task-context variant)
static inline void rb_drop_oldest(void) {
    size_t old_sz;
    // Non-blocking; we don't care about content—just free space
    void* old = xRingbufferReceiveUpTo(ringbuf_handle, &old_sz, 0 /* no wait */, LOG_LINE_MAX);
    if (old) vRingbufferReturnItem(ringbuf_handle, old);
}

// Append bytes into accum with "drop oldest to make room" policy
static void accum_append_drop_oldest(char* accum, size_t* acc_len, const char* src, size_t src_len) {
    // If src itself is larger than the entire capacity, keep only its tail
    if (src_len > LINE_ACCUM_MAX) {
        src += (src_len - LINE_ACCUM_MAX);
        src_len = LINE_ACCUM_MAX;
        *acc_len = 0;  // we will overwrite accum with the tail only
    }

    // Ensure we have room: if not, drop oldest from the head
    size_t need = (*acc_len + src_len > LINE_ACCUM_MAX) ? (*acc_len + src_len - LINE_ACCUM_MAX) : 0;

    if (need > 0) {
        if (need >= *acc_len) {
            // Drop everything we have
            *acc_len = 0;
        } else {
            // Move remaining data to the front
            memmove(accum, accum + need, *acc_len - need);
            *acc_len -= need;
        }
    }

    memcpy(accum + *acc_len, src, src_len);
    *acc_len += src_len;
}

// ---------- Custom vprintf: send each formatted line to ringbuffer ----------
static int custom_log_vprintf(const char* fmt, va_list args) {
    char line[LOG_LINE_MAX];
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len < 0) return len;
    if (len >= LOG_LINE_MAX) {
        len = LOG_LINE_MAX - 1;
        line[len] = '\0';
    }

    // Optional: mirror to UART during bring-up (remove to silence UART)
    fwrite(line, 1, len, stdout);

    if (!ringbuf_handle) return len;

    // Try non-blocking send (include the NUL so consumer can treat as C-string)
    if (in_isr()) {
        BaseType_t hpw = pdFALSE;
        if (xRingbufferSendFromISR(ringbuf_handle, line, (size_t)(len + 1), &hpw) != pdTRUE) {
            // Buffer full: drop-oldest then retry once (still non-blocking)
            rb_drop_oldest_from_isr();
            (void)xRingbufferSendFromISR(ringbuf_handle, line, (size_t)(len + 1), &hpw);
        }
        if (hpw) portYIELD_FROM_ISR();
    } else {
        if (xRingbufferSend(ringbuf_handle, line, (size_t)(len + 1), 0 /* no wait */) != pdTRUE) {
            // Buffer full: drop-oldest then retry once
            rb_drop_oldest();
            (void)xRingbufferSend(ringbuf_handle, line, (size_t)(len + 1), 0);
        }
    }
    return len;
}

// ---- Task that publishes logs to MQTT ----

static void log_forwarder_task(void* arg) {
    (void)arg;

    char accum[LINE_ACCUM_MAX];
    size_t acc_len = 0;

    for (;;) {
        // Block until MQTT reports connected
        xEventGroupWaitBits(mqtt_evg_handle, MQTT_READY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // While connected, drain the ring buffer
        while ((xEventGroupGetBits(mqtt_evg_handle) & MQTT_READY_BIT) != 0) {
            size_t item_size = 0;
            char* item = (char*)xRingbufferReceive(ringbuf_handle, &item_size, pdMS_TO_TICKS(200));
            if (!item) {
                // timeout: loop back to re-check connection
                continue;
            }

            // The producer sent len+1 (with NUL). Treat only valid chars before the NUL.
            size_t valid = strnlen(item, item_size);

            // Walk through the chunk, splitting on '\n'
            size_t pos = 0;
            while (pos < valid) {
                char* nl = memchr(item + pos, '\n', valid - pos);
                size_t seg_len = nl ? (size_t)(nl - (item + pos)) : (valid - pos);

                // Append this segment (no newline) into accum
                accum_append_drop_oldest(accum, &acc_len, item + pos, seg_len);

                if (nl) {
                    // We have a complete line in 'accum' now. Strip trailing '\r' if present.
                    size_t send_len = acc_len;
                    if (send_len && accum[send_len - 1] == '\r') {
                        send_len--;
                    }

                    // Publish one message per line (QoS0 / retain 0)
                    if (send_len != 0) {
                        int mid = mqtt_client_publish_log(LOG_MQTT_TOPIC, accum, (int)send_len);
                        if (mid < 0) {
                            // Failed to publish: re-queue the line + '\n' and break to wait for reconnection
                            // (We re-add '\n' so the semantics remain identical when reprocessed.)
                            char tmp[LOG_LINE_MAX + 2];
                            size_t cpy = (send_len > LOG_LINE_MAX) ? LOG_LINE_MAX : send_len;
                            memcpy(tmp, accum, cpy);
                            tmp[cpy++] = '\n';
                            tmp[cpy] = '\0';

                            if (xRingbufferSend(ringbuf_handle, tmp, cpy + 1 /* include NUL */, 0) != pdTRUE) {
                                rb_drop_oldest();
                                (void)xRingbufferSend(ringbuf_handle, tmp, cpy + 1, 0);
                            }

                            acc_len = 0;  // clear the accumulator
                            break;        // leave inner loop; re-check MQTT state
                        }
                    }

                    // Success: clear the accumulator for the next line
                    acc_len = 0;

                    // Advance past '\n'
                    pos = (size_t)((nl - item) + 1);
                } else {
                    // No newline in the remainder; leave in accum
                    pos = valid;
                }
            }

            // Done with this chunk
            vRingbufferReturnItem(ringbuf_handle, item);

            // If publish failed above, we broke out of inner loop; verify connection again
        }

        // If we drop out of the while-connected loop due to disconnect:
        // keep 'accum' as-is so partial line continues when connection resumes.
    }
}

void init_logging() {
    if (ringbuf_handle != NULL) return;  // Already initialized

    // Create the MQTT event group
    mqtt_evg_handle = xEventGroupCreate();
    configASSERT(mqtt_evg_handle != NULL);
    xEventGroupClearBits(mqtt_evg_handle, MQTT_READY_BIT);

    // Create the log ringbuffer
    ringbuf_handle = xRingbufferCreate(RB_CAPACITY_BYTES, RINGBUF_TYPE_NOSPLIT);
    configASSERT(ringbuf_handle != NULL);

    // Create the log forwarder task
    if (xTaskCreate(log_forwarder_task, "log_forwarder_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create log forwarder task");
        vRingbufferDelete(ringbuf_handle);
        ringbuf_handle = NULL;
        return;
    }

    // Set the custom log function
    esp_log_set_vprintf(custom_log_vprintf);
}

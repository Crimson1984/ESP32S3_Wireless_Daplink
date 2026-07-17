#include "gateway_usb.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

#define USB_CHUNK_SIZE 256U
#define USB_CHUNK_QUEUE 12U

typedef struct {
    uint16_t length;
    uint8_t data[USB_CHUNK_SIZE];
} usb_chunk_t;

static const char *TAG = "gateway_usb";
static QueueHandle_t s_chunk_queue;
static SemaphoreHandle_t s_tx_lock;
static gateway_usb_handler_t s_handler;
static void *s_handler_context;
static uint8_t s_tx_buffer[DATLINK_USB_ENCODED_MAX];

static void rx_callback(int interface, cdcacm_event_t *event)
{
    (void)event;
    usb_chunk_t chunk = {0};
    size_t received = 0;
    if (tinyusb_cdcacm_read(interface, chunk.data, sizeof(chunk.data), &received) == ESP_OK &&
        received > 0U) {
        chunk.length = (uint16_t)received;
        (void)xQueueSend(s_chunk_queue, &chunk, 0);
    }
}

static void decoder_task(void *argument)
{
    (void)argument;
    uint8_t encoded[DATLINK_USB_ENCODED_MAX];
    size_t used = 0;
    usb_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_chunk_queue, &chunk, portMAX_DELAY) != pdTRUE) continue;
        for (uint16_t i = 0; i < chunk.length; ++i) {
            if (chunk.data[i] == 0U) {
                if (used > 0U) {
                    datlink_usb_frame_t *frame = calloc(1, sizeof(*frame));
                    if (frame != NULL) {
                        esp_err_t err = datlink_usb_decode(encoded, used, frame);
                        if (err == ESP_OK && s_handler != NULL) {
                            s_handler(frame, s_handler_context);
                        } else if (err != ESP_OK) {
                            ESP_LOGW(TAG, "invalid USB frame: %s", esp_err_to_name(err));
                        }
                        free(frame);
                    }
                }
                used = 0;
            } else if (used < sizeof(encoded)) {
                encoded[used++] = chunk.data[i];
            } else {
                ESP_LOGW(TAG, "oversized USB frame discarded");
                used = 0;
            }
        }
    }
}

esp_err_t gateway_usb_init(gateway_usb_handler_t handler, void *context)
{
    s_handler = handler;
    s_handler_context = context;
    s_chunk_queue = xQueueCreate(USB_CHUNK_QUEUE, sizeof(usb_chunk_t));
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_chunk_queue == NULL || s_tx_lock == NULL) return ESP_ERR_NO_MEM;

    const tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tinyusb_config), TAG, "TinyUSB install");
    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_config), TAG, "CDC ACM init");
    if (xTaskCreate(decoder_task, "usb_decoder", 8192, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TinyUSB CDC command interface ready");
    return ESP_OK;
}

esp_err_t gateway_usb_send(const datlink_usb_frame_t *frame)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const size_t length = datlink_usb_encode(frame, s_tx_buffer, sizeof(s_tx_buffer));
    esp_err_t err = length == 0U ? ESP_ERR_INVALID_SIZE :
                    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, s_tx_buffer, length);
    if (err == ESP_OK) err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 100);
    xSemaphoreGive(s_tx_lock);
    return err;
}

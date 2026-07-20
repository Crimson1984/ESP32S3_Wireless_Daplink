#include "gateway_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datlink_security.h"
#include "datlink_storage.h"
#include "datlink_transport.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gateway_usb.h"

#define WIRE_IMAGE_DATA_BYTES (DATLINK_WIRE_PAYLOAD_MAX - 6U)

typedef struct {
    uint32_t request_id;
    uint32_t operation_id;
} program_job_t;

static const char *TAG = "gateway_app";
static QueueHandle_t s_program_queue;
static datlink_progress_t s_progress;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t send_usb(uint8_t type, uint32_t request_id, int32_t status,
                          const void *payload, size_t length)
{
    datlink_usb_frame_t *response = calloc(1, sizeof(*response));
    if (response == NULL || length + 4U > DATLINK_USB_PAYLOAD_MAX) {
        free(response);
        return response == NULL ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_SIZE;
    }
    response->type = type;
    response->request_id = request_id;
    response->payload_length = (uint32_t)length + 4U;
    put_u32(response->payload, (uint32_t)status);
    if (payload != NULL && length != 0U) memcpy(response->payload + 4, payload, length);
    const esp_err_t err = gateway_usb_send(response);
    free(response);
    return err;
}

static void send_response(uint32_t request_id, int32_t status,
                          const void *payload, size_t length)
{
    (void)send_usb(DATLINK_USB_RESPONSE, request_id, status, payload, length);
}

static esp_err_t send_event(uint8_t event_type, const void *payload, size_t length)
{
    uint8_t event[1 + DATLINK_WIRE_PAYLOAD_MAX];
    if (length > sizeof(event) - 1U) return ESP_ERR_INVALID_SIZE;
    event[0] = event_type;
    memcpy(event + 1, payload, length);
    return send_usb(DATLINK_USB_EVENT, 0, DATLINK_OK, event, length + 1U);
}

static void send_progress_event(uint8_t event_type, const datlink_progress_t *progress)
{
    uint8_t payload[DATLINK_PROGRESS_WIRE_LEN];
    const size_t length = datlink_progress_encode(progress, payload, sizeof(payload));
    if (length != 0U) (void)send_event(event_type, payload, length);
}

static esp_err_t transport_handler(const datlink_wire_frame_t *frame, void *context)
{
    (void)context;
    if (frame->type == DATLINK_MSG_PROGRAM_PROGRESS ||
        frame->type == DATLINK_MSG_PROGRAM_RESULT) {
        if (datlink_progress_decode(frame->payload, frame->payload_length,
                                    &s_progress) == ESP_OK) {
            return send_event(frame->type, frame->payload, frame->payload_length);
        }
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame->type == DATLINK_MSG_TARGET_INFO ||
        frame->type == DATLINK_MSG_LOADER_TEST ||
        frame->type == DATLINK_MSG_TARGET_BACKUP_DATA ||
        frame->type == DATLINK_MSG_TARGET_BACKUP_RESULT) {
        return send_event(frame->type, frame->payload, frame->payload_length);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t transfer_image(uint32_t operation_id)
{
    const datlink_image_manifest_t *manifest = datlink_storage_manifest();
    uint8_t payload[DATLINK_WIRE_PAYLOAD_MAX];
    const size_t manifest_length = datlink_manifest_encode(manifest, payload, sizeof(payload));
    if (manifest_length == 0U) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(datlink_transport_send(DATLINK_MSG_IMAGE_BEGIN, 0, payload,
                                                manifest_length, pdMS_TO_TICKS(1000)),
                        TAG, "queue manifest");

    uint8_t data[WIRE_IMAGE_DATA_BYTES];
    for (uint32_t offset = 0; offset < manifest->total_length;) {
        const size_t length = manifest->total_length - offset > sizeof(data)
                                  ? sizeof(data)
                                  : manifest->total_length - offset;
        ESP_RETURN_ON_ERROR(datlink_storage_read(offset, data, length), TAG, "read staged image");
        put_u32(payload, offset);
        payload[4] = (uint8_t)length;
        payload[5] = (uint8_t)(length >> 8);
        memcpy(payload + 6, data, length);
        ESP_RETURN_ON_ERROR(datlink_transport_send(DATLINK_MSG_IMAGE_DATA, 0, payload,
                                                    length + 6U, pdMS_TO_TICKS(1000)),
                            TAG, "queue image data");
        offset += (uint32_t)length;
        s_progress.phase = 2;
        s_progress.completed = offset;
        s_progress.total = manifest->total_length;
    }
    ESP_RETURN_ON_ERROR(datlink_transport_send(DATLINK_MSG_IMAGE_END, 0, NULL, 0,
                                                pdMS_TO_TICKS(1000)),
                        TAG, "queue image end");
    put_u32(payload, operation_id);
    ESP_RETURN_ON_ERROR(datlink_transport_send(DATLINK_MSG_PROGRAM_START, 0, payload, 4,
                                                pdMS_TO_TICKS(1000)),
                        TAG, "queue program start");
    return datlink_transport_flush(pdMS_TO_TICKS(30000));
}

static void program_task(void *argument)
{
    (void)argument;
    program_job_t job;
    for (;;) {
        if (xQueueReceive(s_program_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        memset(&s_progress, 0, sizeof(s_progress));
        s_progress.phase = 1;
        s_progress.total = datlink_storage_manifest()->total_length;
        const esp_err_t err = transfer_image(job.operation_id);
        if (err != ESP_OK) {
            s_progress.status = err;
            s_progress.phase = 0xFF;
            send_progress_event(DATLINK_MSG_PROGRAM_RESULT, &s_progress);
        }
    }
}

static void usb_handler(const datlink_usb_frame_t *frame, void *context)
{
    (void)context;
    esp_err_t err = ESP_OK;
    switch (frame->type) {
    case DATLINK_USB_GET_INFO: {
        char info[192];
        snprintf(info, sizeof(info),
                 "{\"role\":\"gateway\",\"session\":%" PRIu32
                 ",\"local\":\"" MACSTR "\",\"peer\":\"" MACSTR "\"}",
                 datlink_transport_session_id(), MAC2STR(datlink_security_local_mac()),
                 MAC2STR(datlink_security_peer_mac()));
        send_response(frame->request_id, DATLINK_OK, info, strlen(info));
        return;
    }
    case DATLINK_USB_GET_LINK_STATUS: {
        const uint8_t linked = datlink_transport_link_up() ? 1U : 0U;
        send_response(frame->request_id, DATLINK_OK, &linked, sizeof(linked));
        return;
    }
    case DATLINK_USB_IMAGE_BEGIN: {
        datlink_image_manifest_t manifest;
        err = datlink_manifest_decode(frame->payload, frame->payload_length, &manifest);
        if (err == ESP_OK) err = datlink_storage_begin(&manifest);
        break;
    }
    case DATLINK_USB_IMAGE_DATA:
        if (frame->payload_length < 5U) err = ESP_ERR_INVALID_SIZE;
        else err = datlink_storage_write(get_u32(frame->payload), frame->payload + 4,
                                         frame->payload_length - 4U);
        break;
    case DATLINK_USB_IMAGE_END:
        err = datlink_storage_finalize();
        break;
    case DATLINK_USB_PROGRAM_START: {
        if (!datlink_storage_ready() || !datlink_transport_link_up()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        program_job_t job = {
            .request_id = frame->request_id,
            .operation_id = frame->payload_length >= 4U
                                ? get_u32(frame->payload)
                                : datlink_storage_manifest()->operation_id,
        };
        if (xQueueSend(s_program_queue, &job, 0) != pdTRUE) err = ESP_ERR_NO_MEM;
        break;
    }
    case DATLINK_USB_PROGRAM_ABORT:
        err = datlink_transport_send(DATLINK_MSG_PROGRAM_ABORT, 0, NULL, 0,
                                     pdMS_TO_TICKS(100));
        break;
    case DATLINK_USB_GET_PROGRESS:
        {
            uint8_t payload[DATLINK_PROGRESS_WIRE_LEN];
            const size_t length = datlink_progress_encode(&s_progress, payload, sizeof(payload));
            send_response(frame->request_id, DATLINK_OK, payload, length);
        }
        return;
    case DATLINK_USB_TARGET_RESET:
        err = datlink_transport_send(DATLINK_MSG_TARGET_RESET, 0, NULL, 0,
                                     pdMS_TO_TICKS(100));
        break;
    case DATLINK_USB_TARGET_READ_INFO:
        err = datlink_transport_send(DATLINK_MSG_TARGET_INFO, 0, NULL, 0,
                                     pdMS_TO_TICKS(100));
        break;
    case DATLINK_USB_LOADER_TEST:
        err = datlink_transport_send(DATLINK_MSG_LOADER_TEST, 0, NULL, 0,
                                     pdMS_TO_TICKS(100));
        break;
    case DATLINK_USB_TARGET_BACKUP_START:
        if (frame->payload_length != 4U || !datlink_transport_link_up()) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = datlink_transport_send(DATLINK_MSG_TARGET_BACKUP_START, 0,
                                         frame->payload, frame->payload_length,
                                         pdMS_TO_TICKS(100));
        }
        break;
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    send_response(frame->request_id, err == ESP_OK ? DATLINK_OK : err, NULL, 0);
}

esp_err_t gateway_app_start(void)
{
    s_program_queue = xQueueCreate(2, sizeof(program_job_t));
    if (s_program_queue == NULL) return ESP_ERR_NO_MEM;
    datlink_transport_set_handler(transport_handler, NULL);
    ESP_RETURN_ON_ERROR(gateway_usb_init(usb_handler, NULL), TAG, "USB init");
    if (xTaskCreatePinnedToCore(program_task, "gateway_program", 8192, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

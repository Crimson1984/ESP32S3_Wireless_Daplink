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

typedef struct {
    datlink_wire_frame_t frame;
    datlink_rx_token_t token;
} app_rx_item_t;

static const char *TAG = "gateway_app";
static QueueHandle_t s_program_queue;
static QueueHandle_t s_app_rx_queue;
static datlink_progress_t s_progress;
static portMUX_TYPE s_error_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending_command_error;
static uint8_t s_command_error[DATLINK_COMMAND_ERROR_WIRE_LEN];

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

static esp_err_t send_best_effort_event(uint8_t event_type,
                                        const void *payload, size_t length)
{
    const esp_err_t err = send_event(event_type, payload, length);
    if (err != ESP_OK) {
        /* A CLI process closes the CDC port after every command. A late SWD
         * result must still be acknowledged on the reliable radio link;
         * otherwise it permanently occupies rx_base and blocks every later
         * Probe event until a board reset. The caller can safely issue the
         * idempotent query again after reconnecting. */
        ESP_LOGW(TAG, "drop event type=%u while USB is unavailable: %s",
                 event_type, esp_err_to_name(err));
    }
    return ESP_OK;
}

static void send_progress_event(uint8_t event_type, const datlink_progress_t *progress)
{
    uint8_t payload[DATLINK_PROGRESS_WIRE_LEN];
    const size_t length = datlink_progress_encode(progress, payload, sizeof(payload));
    if (length != 0U) (void)send_event(event_type, payload, length);
}

static datlink_status_t status_from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return DATLINK_OK;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE: return DATLINK_ERR_ARGUMENT;
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_NO_MEM: return DATLINK_ERR_STATE;
    case ESP_ERR_TIMEOUT: return DATLINK_ERR_TIMEOUT;
    case ESP_ERR_INVALID_CRC: return DATLINK_ERR_CRC;
    default: return DATLINK_ERR_LINK;
    }
}

static esp_err_t process_transport_frame(const datlink_wire_frame_t *frame)
{
    if (frame->type == DATLINK_MSG_PROGRAM_PROGRESS ||
        frame->type == DATLINK_MSG_PROGRAM_RESULT) {
        if (datlink_progress_decode(frame->payload, frame->payload_length,
                                    &s_progress) == ESP_OK) {
            return send_best_effort_event(frame->type, frame->payload,
                                          frame->payload_length);
        }
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame->type == DATLINK_MSG_TARGET_INFO ||
        frame->type == DATLINK_MSG_LOADER_TEST) {
        return send_best_effort_event(frame->type, frame->payload,
                                      frame->payload_length);
    }
    if (frame->type == DATLINK_MSG_TARGET_BACKUP_DATA ||
        frame->type == DATLINK_MSG_TARGET_BACKUP_RESULT) {
        return send_event(frame->type, frame->payload, frame->payload_length);
    }
    if (frame->type == DATLINK_MSG_COMMAND_ERROR) {
        if (frame->payload_length != DATLINK_COMMAND_ERROR_WIRE_LEN) {
            return ESP_ERR_INVALID_SIZE;
        }
        const esp_err_t err = send_event(frame->type, frame->payload,
                                         frame->payload_length);
        if (err != ESP_OK) {
            taskENTER_CRITICAL(&s_error_lock);
            memcpy(s_command_error, frame->payload, sizeof(s_command_error));
            s_pending_command_error = true;
            taskEXIT_CRITICAL(&s_error_lock);
            ESP_LOGW(TAG, "cache COMMAND_ERROR until the next USB request");
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static datlink_rx_disposition_t transport_handler(
    const datlink_wire_frame_t *frame, const datlink_rx_token_t *token,
    void *context)
{
    (void)context;
    const app_rx_item_t item = {.frame = *frame, .token = *token};
    return xQueueSend(s_app_rx_queue, &item, 0) == pdTRUE
               ? DATLINK_RX_ASYNC : DATLINK_RX_DEFER;
}

static void app_rx_task(void *argument)
{
    (void)argument;
    app_rx_item_t item;
    for (;;) {
        if (xQueueReceive(s_app_rx_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        const esp_err_t err = process_transport_frame(&item.frame);
        (void)datlink_transport_complete_rx(&item.token, status_from_esp(err),
                                            (uint32_t)err);
    }
}

static void transport_event_handler(datlink_transport_event_t event, void *context)
{
    (void)context;
    if (event == DATLINK_TRANSPORT_EVENT_LOCAL_EPOCH_RESET) {
        s_progress.status = DATLINK_ERR_LINK;
        s_progress.phase = 9U;
        s_progress.detail = (uint32_t)event;
        send_progress_event(DATLINK_MSG_PROGRAM_RESULT, &s_progress);
    }
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
    uint8_t pending_error[DATLINK_COMMAND_ERROR_WIRE_LEN];
    bool have_error;
    taskENTER_CRITICAL(&s_error_lock);
    have_error = s_pending_command_error;
    if (have_error) memcpy(pending_error, s_command_error, sizeof(pending_error));
    taskEXIT_CRITICAL(&s_error_lock);
    if (have_error && send_event(DATLINK_MSG_COMMAND_ERROR, pending_error,
                                 sizeof(pending_error)) == ESP_OK) {
        taskENTER_CRITICAL(&s_error_lock);
        if (s_pending_command_error &&
            memcmp(s_command_error, pending_error, sizeof(pending_error)) == 0) {
            s_pending_command_error = false;
        }
        taskEXIT_CRITICAL(&s_error_lock);
    }
    esp_err_t err = ESP_OK;
    switch (frame->type) {
    case DATLINK_USB_GET_INFO: {
        char info[192];
        snprintf(info, sizeof(info),
                 "{\"role\":\"gateway\",\"protocol\":%u,\"session\":%" PRIu32
                 ",\"local\":\"" MACSTR "\",\"peer\":\"" MACSTR "\"}",
                 DATLINK_PROTOCOL_VERSION, datlink_transport_session_id(),
                 MAC2STR(datlink_security_local_mac()),
                 MAC2STR(datlink_security_peer_mac()));
        send_response(frame->request_id, DATLINK_OK, info, strlen(info));
        return;
    }
    case DATLINK_USB_GET_LINK_STATUS: {
        datlink_transport_status_t status;
        datlink_transport_get_status(&status);
        uint8_t payload[40] = {0};
        payload[0] = status.up ? 1U : 0U;
        payload[1] = status.recovering ? 1U : 0U;
        put_u32(payload + 4U, status.local_session);
        put_u32(payload + 8U, status.peer_session);
        put_u32(payload + 12U, status.next_tx_sequence);
        put_u32(payload + 16U, status.rx_base);
        payload[20] = (uint8_t)status.tx_pending;
        payload[21] = (uint8_t)(status.tx_pending >> 8);
        payload[22] = (uint8_t)status.rx_pending;
        payload[23] = (uint8_t)(status.rx_pending >> 8);
        payload[24] = status.head_state;
        put_u32(payload + 28U, status.head_age_ms);
        put_u32(payload + 32U, (uint32_t)status.last_error);
        put_u32(payload + 36U, status.recovery_count);
        send_response(frame->request_id, DATLINK_OK, payload, sizeof(payload));
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
    case DATLINK_USB_TRANSPORT_RECOVER:
        err = datlink_transport_recover();
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
    s_app_rx_queue = xQueueCreate(8, sizeof(app_rx_item_t));
    if (s_program_queue == NULL || s_app_rx_queue == NULL) return ESP_ERR_NO_MEM;
    datlink_transport_set_handler(transport_handler, NULL);
    datlink_transport_set_event_handler(transport_event_handler, NULL);
    ESP_RETURN_ON_ERROR(gateway_usb_init(usb_handler, NULL), TAG, "USB init");
    if (xTaskCreatePinnedToCore(program_task, "gateway_program", 8192, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(app_rx_task, "gateway_app_rx", 6144, NULL, 7, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#include "probe_app.h"

#include <inttypes.h>
#include <string.h>

#include "datlink_storage.h"
#include "datlink_transport.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "programmer.h"

static const char *TAG = "probe_app";

typedef struct {
    datlink_wire_frame_t frame;
    datlink_rx_token_t token;
} app_rx_item_t;

static QueueHandle_t s_app_rx_queue;
static portMUX_TYPE s_active_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_active_type;
static TickType_t s_active_since;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void progress_callback(const datlink_progress_t *progress, void *context)
{
    (void)context;
    const uint8_t type = progress->phase == PROGRAMMER_PHASE_DONE ||
                         progress->phase == PROGRAMMER_PHASE_FAILED
                             ? DATLINK_MSG_PROGRAM_RESULT
                             : DATLINK_MSG_PROGRAM_PROGRESS;
    uint8_t payload[DATLINK_PROGRESS_WIRE_LEN];
    const size_t length = datlink_progress_encode(progress, payload, sizeof(payload));
    if (length != 0U) {
        (void)datlink_transport_send(type, 0, payload, length, pdMS_TO_TICKS(100));
    }
}

static esp_err_t send_target_result(uint8_t message_type, const char *operation,
                                    datlink_status_t status,
                                    const programmer_target_info_t *info,
                                    const programmer_target_diagnostic_t *diagnostic)
{
    uint8_t payload[20U + DATLINK_TARGET_INFO_WIRE_LEN];
    put_u32(payload, (uint32_t)status);
    size_t length = 4U;
    if (status == DATLINK_OK) {
        const size_t info_length = datlink_target_info_encode(
            info, payload + 4U, sizeof(payload) - 4U);
        if (info_length == 0U) return ESP_ERR_INVALID_SIZE;
        length += info_length;
    } else {
        put_u32(payload + 4U, diagnostic->stage);
        put_u32(payload + 8U, diagnostic->dpidr);
        put_u32(payload + 12U, diagnostic->ap_idr);
        put_u32(payload + 16U, diagnostic->swd_clock_khz);
        length = 20U;
        ESP_LOGW(TAG, "%s failed: %s (%d), stage=%" PRIu32
                      " DPIDR=0x%08" PRIx32 " APIDR=0x%08" PRIx32
                      " @%" PRIu32 "kHz",
                 operation, datlink_status_name(status), (int)status,
                 diagnostic->stage, diagnostic->dpidr, diagnostic->ap_idr,
                 diagnostic->swd_clock_khz);
    }
    /* Always return a result frame, including on SWD/VTref failure. This
     * acknowledges the reliable request instead of re-executing it until a
     * transport timeout. */
    return datlink_transport_send(message_type, 0, payload, length,
                                  pdMS_TO_TICKS(100));
}

static esp_err_t backup_data_callback(uint32_t operation_id, uint32_t offset,
                                      const uint8_t *data, size_t length,
                                      void *context)
{
    (void)context;
    if (data == NULL || length == 0U || length > DATLINK_BACKUP_DATA_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t payload[DATLINK_BACKUP_DATA_HEADER_LEN + DATLINK_BACKUP_DATA_BYTES];
    put_u32(payload + 0U, operation_id);
    put_u32(payload + 4U, offset);
    payload[8] = (uint8_t)length;
    payload[9] = (uint8_t)(length >> 8);
    memcpy(payload + DATLINK_BACKUP_DATA_HEADER_LEN, data, length);
    return datlink_transport_send(DATLINK_MSG_TARGET_BACKUP_DATA, 0, payload,
                                  DATLINK_BACKUP_DATA_HEADER_LEN + length,
                                  pdMS_TO_TICKS(1000));
}

static void backup_done_callback(
    uint32_t operation_id, datlink_status_t status, uint32_t total_length,
    const uint8_t sha256[DATLINK_SHA256_LEN],
    const programmer_target_diagnostic_t *diagnostic, void *context)
{
    (void)context;
    uint8_t payload[DATLINK_BACKUP_RESULT_WIRE_LEN] = {0};
    put_u32(payload + 0U, operation_id);
    put_u32(payload + 4U, (uint32_t)status);
    put_u32(payload + 8U, total_length);
    if (sha256 != NULL) memcpy(payload + 12U, sha256, DATLINK_SHA256_LEN);
    if (diagnostic != NULL) {
        put_u32(payload + 44U, diagnostic->stage);
        put_u32(payload + 48U, diagnostic->dpidr);
        put_u32(payload + 52U, diagnostic->ap_idr);
        put_u32(payload + 56U, diagnostic->swd_clock_khz);
    }
    const esp_err_t err = datlink_transport_send(
        DATLINK_MSG_TARGET_BACKUP_RESULT, 0, payload, sizeof(payload),
        pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "queue backup result failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t process_message(const datlink_wire_frame_t *frame)
{
    switch (frame->type) {
    case DATLINK_MSG_IMAGE_BEGIN: {
        datlink_image_manifest_t manifest;
        ESP_RETURN_ON_ERROR(datlink_manifest_decode(frame->payload, frame->payload_length,
                                                     &manifest),
                            TAG, "decode manifest");
        return datlink_storage_begin(&manifest);
    }
    case DATLINK_MSG_IMAGE_DATA: {
        if (frame->payload_length < 6U) return ESP_ERR_INVALID_SIZE;
        const uint32_t offset = get_u32(frame->payload);
        const uint16_t length = (uint16_t)frame->payload[4] |
                                ((uint16_t)frame->payload[5] << 8);
        if ((size_t)length + 6U != frame->payload_length) return ESP_ERR_INVALID_SIZE;
        return datlink_storage_write(offset, frame->payload + 6, length);
    }
    case DATLINK_MSG_IMAGE_END:
        return datlink_storage_finalize();
    case DATLINK_MSG_PROGRAM_START:
        if (frame->payload_length != 4U || !datlink_storage_ready()) {
            return ESP_ERR_INVALID_STATE;
        }
        return programmer_start(get_u32(frame->payload));
    case DATLINK_MSG_PROGRAM_ABORT:
        programmer_abort();
        return ESP_OK;
    case DATLINK_MSG_TARGET_RESET:
        return programmer_reset_target();
    case DATLINK_MSG_TARGET_INFO: {
        programmer_target_info_t info = {0};
        programmer_target_diagnostic_t diagnostic = {0};
        const datlink_status_t status = programmer_read_target_info(&info, &diagnostic);
        return send_target_result(DATLINK_MSG_TARGET_INFO, "target info",
                                  status, &info, &diagnostic);
    }
    case DATLINK_MSG_LOADER_TEST: {
        programmer_target_info_t info = {0};
        programmer_target_diagnostic_t diagnostic = {0};
        const datlink_status_t status = programmer_test_loader(&info, &diagnostic);
        return send_target_result(DATLINK_MSG_LOADER_TEST, "loader test",
                                  status, &info, &diagnostic);
    }
    case DATLINK_MSG_TARGET_BACKUP_START:
        if (frame->payload_length != 4U) return ESP_ERR_INVALID_SIZE;
        return programmer_backup_start(get_u32(frame->payload),
                                       backup_data_callback,
                                       backup_done_callback, NULL);
    case DATLINK_MSG_COMMAND_ERROR:
        if (frame->payload_length != DATLINK_COMMAND_ERROR_WIRE_LEN) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (frame->payload[8] == DATLINK_MSG_TARGET_BACKUP_DATA ||
            frame->payload[8] == DATLINK_MSG_TARGET_BACKUP_RESULT) {
            programmer_abort();
        }
        ESP_LOGW(TAG, "peer rejected type=%u status=%" PRId32 " detail=0x%08" PRIx32,
                 frame->payload[8], (int32_t)get_u32(frame->payload + 12U),
                 get_u32(frame->payload + 16U));
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
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
    default: return DATLINK_ERR_STORAGE;
    }
}

static datlink_rx_disposition_t handle_message(
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
        taskENTER_CRITICAL(&s_active_lock);
        s_active_type = item.frame.type;
        s_active_since = xTaskGetTickCount();
        taskEXIT_CRITICAL(&s_active_lock);
        const esp_err_t err = process_message(&item.frame);
        (void)datlink_transport_complete_rx(&item.token, status_from_esp(err),
                                            (uint32_t)err);
        taskENTER_CRITICAL(&s_active_lock);
        s_active_type = 0U;
        taskEXIT_CRITICAL(&s_active_lock);
    }
}

static void worker_watchdog_task(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint8_t type;
        TickType_t since;
        taskENTER_CRITICAL(&s_active_lock);
        type = s_active_type;
        since = s_active_since;
        taskEXIT_CRITICAL(&s_active_lock);
        uint32_t limit_ms = type == DATLINK_MSG_LOADER_TEST ? 17000U : 12000U;
        if ((type != DATLINK_MSG_TARGET_INFO && type != DATLINK_MSG_LOADER_TEST) ||
            xTaskGetTickCount() - since < pdMS_TO_TICKS(limit_ms)) continue;

        datlink_progress_t progress = {0};
        (void)programmer_get_progress(&progress);
        if (progress.phase == PROGRAMMER_PHASE_ERASE ||
            progress.phase == PROGRAMMER_PHASE_PROGRAM ||
            progress.phase == PROGRAMMER_PHASE_VERIFY) {
            ESP_LOGE(TAG, "worker timeout during flash; automatic restart suppressed");
            programmer_abort();
            taskENTER_CRITICAL(&s_active_lock);
            s_active_type = 0U;
            taskEXIT_CRITICAL(&s_active_lock);
            continue;
        }
        ESP_LOGE(TAG, "read-only worker timeout type=%u; restarting Probe", type);
        programmer_abort();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

static void transport_event_handler(datlink_transport_event_t event, void *context)
{
    (void)context;
    if (event == DATLINK_TRANSPORT_EVENT_PEER_EPOCH_RESET) {
        if (programmer_is_busy()) {
            ESP_LOGW(TAG, "Gateway epoch changed during target operation; staged image retained");
        } else {
            datlink_storage_invalidate();
            ESP_LOGW(TAG, "Gateway epoch changed; incomplete staged image invalidated");
        }
    }
}

esp_err_t probe_app_start(void)
{
    ESP_RETURN_ON_ERROR(programmer_init(progress_callback, NULL), TAG, "programmer init");
    s_app_rx_queue = xQueueCreate(8, sizeof(app_rx_item_t));
    if (s_app_rx_queue == NULL) return ESP_ERR_NO_MEM;
    datlink_transport_set_handler(handle_message, NULL);
    datlink_transport_set_event_handler(transport_event_handler, NULL);
    if (xTaskCreatePinnedToCore(app_rx_task, "probe_app_rx", 8192, NULL, 7, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(worker_watchdog_task, "probe_worker_wd", 3072, NULL, 5,
                                NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

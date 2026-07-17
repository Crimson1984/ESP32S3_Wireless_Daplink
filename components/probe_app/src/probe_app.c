#include "probe_app.h"

#include <string.h>

#include "datlink_storage.h"
#include "datlink_transport.h"
#include "esp_check.h"
#include "esp_log.h"
#include "programmer.h"

static const char *TAG = "probe_app";

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

static esp_err_t handle_message(const datlink_wire_frame_t *frame, void *context)
{
    (void)context;
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
        programmer_target_info_t info;
        ESP_RETURN_ON_ERROR(programmer_read_target_info(&info), TAG, "target info");
        uint8_t payload[DATLINK_TARGET_INFO_WIRE_LEN];
        const size_t length = datlink_target_info_encode(&info, payload, sizeof(payload));
        return length == 0U ? ESP_ERR_INVALID_SIZE
                            : datlink_transport_send(DATLINK_MSG_TARGET_INFO, 0, payload,
                                                     length, pdMS_TO_TICKS(100));
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t probe_app_start(void)
{
    ESP_RETURN_ON_ERROR(programmer_init(progress_callback, NULL), TAG, "programmer init");
    datlink_transport_set_handler(handle_message, NULL);
    return ESP_OK;
}

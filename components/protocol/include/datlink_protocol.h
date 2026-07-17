#pragma once

#include <stddef.h>
#include <stdint.h>

#include "datlink_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATLINK_WIRE_MAGIC 0x4C44U
#define DATLINK_WIRE_HEADER_LEN 26U
#define DATLINK_WIRE_PAYLOAD_MAX 192U
#define DATLINK_WIRE_FRAME_MAX \
    (DATLINK_WIRE_HEADER_LEN + DATLINK_WIRE_PAYLOAD_MAX + 4U)

#define DATLINK_USB_PAYLOAD_MAX 4096U
#define DATLINK_USB_RAW_MAX (12U + DATLINK_USB_PAYLOAD_MAX + 4U)
#define DATLINK_USB_ENCODED_MAX (DATLINK_USB_RAW_MAX + DATLINK_USB_RAW_MAX / 254U + 2U)

#define DATLINK_IMAGE_MAX_SEGMENTS 8U
#define DATLINK_TARGET_MSPM0G3507 0x00003507U
#define DATLINK_PROGRESS_WIRE_LEN 20U
#define DATLINK_TARGET_INFO_WIRE_LEN 28U

typedef enum {
    DATLINK_MSG_HELLO = 1,
    DATLINK_MSG_HEARTBEAT = 2,
    DATLINK_MSG_ACK = 3,
    DATLINK_MSG_IMAGE_BEGIN = 16,
    DATLINK_MSG_IMAGE_DATA = 17,
    DATLINK_MSG_IMAGE_END = 18,
    DATLINK_MSG_PROGRAM_START = 19,
    DATLINK_MSG_PROGRAM_ABORT = 20,
    DATLINK_MSG_PROGRAM_PROGRESS = 21,
    DATLINK_MSG_PROGRAM_RESULT = 22,
    DATLINK_MSG_TARGET_RESET = 23,
    DATLINK_MSG_TARGET_INFO = 24,
} datlink_message_type_t;

typedef enum {
    DATLINK_USB_GET_INFO = 1,
    DATLINK_USB_GET_LINK_STATUS = 2,
    DATLINK_USB_IMAGE_BEGIN = 16,
    DATLINK_USB_IMAGE_DATA = 17,
    DATLINK_USB_IMAGE_END = 18,
    DATLINK_USB_PROGRAM_START = 19,
    DATLINK_USB_PROGRAM_ABORT = 20,
    DATLINK_USB_GET_PROGRESS = 21,
    DATLINK_USB_TARGET_RESET = 22,
    DATLINK_USB_TARGET_READ_INFO = 23,
    DATLINK_USB_RESPONSE = 0x80,
    DATLINK_USB_EVENT = 0x81,
} datlink_usb_type_t;

typedef struct {
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t ack_base;
    uint32_t ack_bitmap;
    uint16_t payload_length;
    uint8_t payload[DATLINK_WIRE_PAYLOAD_MAX];
} datlink_wire_frame_t;

typedef struct {
    uint8_t type;
    uint16_t flags;
    uint32_t request_id;
    uint32_t payload_length;
    uint8_t payload[DATLINK_USB_PAYLOAD_MAX];
} datlink_usb_frame_t;

typedef struct {
    uint32_t address;
    uint32_t length;
    uint32_t data_offset;
    uint32_t crc32c;
} datlink_image_segment_t;

typedef struct {
    uint16_t format_version;
    uint16_t segment_count;
    uint32_t target;
    uint32_t operation_id;
    uint32_t total_length;
    uint8_t sha256[DATLINK_SHA256_LEN];
    datlink_image_segment_t segments[DATLINK_IMAGE_MAX_SEGMENTS];
} datlink_image_manifest_t;

typedef struct {
    int32_t status;
    uint8_t phase;
    uint8_t reserved[3];
    uint32_t completed;
    uint32_t total;
    uint32_t detail;
} datlink_progress_t;

typedef struct {
    uint32_t dpidr;
    uint32_t ap_idr;
    uint32_t cpuid;
    uint32_t factory_device_id;
    uint32_t factory_user_id;
    uint32_t factory_sramflash;
    uint32_t swd_clock_khz;
} datlink_target_info_t;

size_t datlink_wire_encode(const datlink_wire_frame_t *frame,
                           uint8_t *output, size_t capacity);
esp_err_t datlink_wire_decode(const uint8_t *input, size_t length,
                              datlink_wire_frame_t *frame);

size_t datlink_usb_encode(const datlink_usb_frame_t *frame,
                          uint8_t *output, size_t capacity);
esp_err_t datlink_usb_decode(const uint8_t *encoded, size_t encoded_length,
                             datlink_usb_frame_t *frame);

size_t datlink_cobs_encode(const uint8_t *input, size_t length,
                           uint8_t *output, size_t capacity);
size_t datlink_cobs_decode(const uint8_t *input, size_t length,
                           uint8_t *output, size_t capacity);

esp_err_t datlink_manifest_validate(const datlink_image_manifest_t *manifest);
size_t datlink_manifest_encode(const datlink_image_manifest_t *manifest,
                               uint8_t *output, size_t capacity);
esp_err_t datlink_manifest_decode(const uint8_t *input, size_t length,
                                  datlink_image_manifest_t *manifest);
size_t datlink_progress_encode(const datlink_progress_t *progress,
                               uint8_t *output, size_t capacity);
esp_err_t datlink_progress_decode(const uint8_t *input, size_t length,
                                  datlink_progress_t *progress);
size_t datlink_target_info_encode(const datlink_target_info_t *info,
                                  uint8_t *output, size_t capacity);
esp_err_t datlink_target_info_decode(const uint8_t *input, size_t length,
                                     datlink_target_info_t *info);

#ifdef __cplusplus
}
#endif

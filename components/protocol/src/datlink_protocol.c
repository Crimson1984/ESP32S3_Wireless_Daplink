#include "datlink_protocol.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t datlink_wire_encode(const datlink_wire_frame_t *frame,
                           uint8_t *output, size_t capacity)
{
    if (frame == NULL || output == NULL ||
        frame->payload_length > DATLINK_WIRE_PAYLOAD_MAX) {
        return 0;
    }
    const size_t total = DATLINK_WIRE_HEADER_LEN + frame->payload_length + 4U;
    if (capacity < total) {
        return 0;
    }
    put_u16(output + 0, DATLINK_WIRE_MAGIC);
    output[2] = DATLINK_PROTOCOL_VERSION;
    output[3] = frame->type;
    put_u16(output + 4, frame->flags);
    put_u32(output + 6, frame->session_id);
    put_u32(output + 10, frame->sequence);
    put_u32(output + 14, frame->ack_base);
    put_u32(output + 18, frame->ack_bitmap);
    put_u16(output + 22, frame->payload_length);
    put_u16(output + 24, datlink_crc16_ccitt(output, 24));
    memcpy(output + DATLINK_WIRE_HEADER_LEN, frame->payload, frame->payload_length);
    put_u32(output + DATLINK_WIRE_HEADER_LEN + frame->payload_length,
            datlink_crc32c(0, output, DATLINK_WIRE_HEADER_LEN + frame->payload_length));
    return total;
}

esp_err_t datlink_wire_decode(const uint8_t *input, size_t length,
                              datlink_wire_frame_t *frame)
{
    if (input == NULL || frame == NULL || length < DATLINK_WIRE_HEADER_LEN + 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t payload_length = get_u16(input + 22);
    if (get_u16(input) != DATLINK_WIRE_MAGIC || input[2] != DATLINK_PROTOCOL_VERSION ||
        payload_length > DATLINK_WIRE_PAYLOAD_MAX ||
        length != DATLINK_WIRE_HEADER_LEN + payload_length + 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (get_u16(input + 24) != datlink_crc16_ccitt(input, 24)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (get_u32(input + DATLINK_WIRE_HEADER_LEN + payload_length) !=
        datlink_crc32c(0, input, DATLINK_WIRE_HEADER_LEN + payload_length)) {
        return ESP_ERR_INVALID_CRC;
    }
    memset(frame, 0, sizeof(*frame));
    frame->type = input[3];
    frame->flags = get_u16(input + 4);
    frame->session_id = get_u32(input + 6);
    frame->sequence = get_u32(input + 10);
    frame->ack_base = get_u32(input + 14);
    frame->ack_bitmap = get_u32(input + 18);
    frame->payload_length = payload_length;
    memcpy(frame->payload, input + DATLINK_WIRE_HEADER_LEN, payload_length);
    return ESP_OK;
}

size_t datlink_cobs_encode(const uint8_t *input, size_t length,
                           uint8_t *output, size_t capacity)
{
    if ((input == NULL && length != 0U) || output == NULL || capacity == 0U) {
        return 0;
    }
    size_t read = 0, write = 1, code_index = 0;
    uint8_t code = 1;
    while (read < length) {
        if (input[read] == 0) {
            if (code_index >= capacity) return 0;
            output[code_index] = code;
            code = 1;
            code_index = write++;
            if (write > capacity) return 0;
            ++read;
        } else {
            if (write >= capacity) return 0;
            output[write++] = input[read++];
            if (++code == 0xFF) {
                if (code_index >= capacity) return 0;
                output[code_index] = code;
                code = 1;
                code_index = write++;
                if (write > capacity) return 0;
            }
        }
    }
    if (code_index >= capacity) return 0;
    output[code_index] = code;
    return write;
}

size_t datlink_cobs_decode(const uint8_t *input, size_t length,
                           uint8_t *output, size_t capacity)
{
    if (input == NULL || output == NULL) return 0;
    size_t read = 0, write = 0;
    while (read < length) {
        const uint8_t code = input[read++];
        if (code == 0 || read + (size_t)code - 1U > length) return 0;
        for (uint8_t i = 1; i < code; ++i) {
            if (write >= capacity) return 0;
            output[write++] = input[read++];
        }
        if (code != 0xFF && read < length) {
            if (write >= capacity) return 0;
            output[write++] = 0;
        }
    }
    return write;
}

size_t datlink_usb_encode(const datlink_usb_frame_t *frame,
                          uint8_t *output, size_t capacity)
{
    if (frame == NULL || output == NULL || frame->payload_length > DATLINK_USB_PAYLOAD_MAX) {
        return 0;
    }
    uint8_t raw[DATLINK_USB_RAW_MAX];
    raw[0] = DATLINK_PROTOCOL_VERSION;
    raw[1] = frame->type;
    put_u16(raw + 2, frame->flags);
    put_u32(raw + 4, frame->request_id);
    put_u32(raw + 8, frame->payload_length);
    memcpy(raw + 12, frame->payload, frame->payload_length);
    put_u32(raw + 12 + frame->payload_length,
            datlink_crc32c(0, raw, 12 + frame->payload_length));
    const size_t encoded = datlink_cobs_encode(raw, 16 + frame->payload_length,
                                               output, capacity > 0 ? capacity - 1U : 0U);
    if (encoded == 0 || encoded >= capacity) return 0;
    output[encoded] = 0;
    return encoded + 1U;
}

esp_err_t datlink_usb_decode(const uint8_t *encoded, size_t encoded_length,
                             datlink_usb_frame_t *frame)
{
    if (encoded == NULL || frame == NULL || encoded_length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[DATLINK_USB_RAW_MAX];
    const size_t raw_length = datlink_cobs_decode(encoded, encoded_length, raw, sizeof(raw));
    if (raw_length < 16U || raw[0] != DATLINK_PROTOCOL_VERSION) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t payload_length = get_u32(raw + 8);
    if (payload_length > DATLINK_USB_PAYLOAD_MAX || raw_length != 16U + payload_length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (get_u32(raw + 12 + payload_length) != datlink_crc32c(0, raw, 12 + payload_length)) {
        return ESP_ERR_INVALID_CRC;
    }
    memset(frame, 0, sizeof(*frame));
    frame->type = raw[1];
    frame->flags = get_u16(raw + 2);
    frame->request_id = get_u32(raw + 4);
    frame->payload_length = payload_length;
    memcpy(frame->payload, raw + 12, payload_length);
    return ESP_OK;
}

esp_err_t datlink_manifest_validate(const datlink_image_manifest_t *manifest)
{
    if (manifest == NULL || manifest->format_version != 1U ||
        manifest->target != DATLINK_TARGET_MSPM0G3507 ||
        manifest->segment_count == 0U ||
        manifest->segment_count > DATLINK_IMAGE_MAX_SEGMENTS ||
        manifest->total_length == 0U || manifest->total_length > 0x20000U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t sum = 0;
    for (uint16_t i = 0; i < manifest->segment_count; ++i) {
        const datlink_image_segment_t *segment = &manifest->segments[i];
        if (segment->length == 0U || (segment->address & 7U) != 0U ||
            segment->address >= 0x20000U ||
            segment->length > 0x20000U - segment->address ||
            segment->data_offset > manifest->total_length ||
            segment->length > manifest->total_length - segment->data_offset) {
            return ESP_ERR_INVALID_ARG;
        }
        sum += segment->length;
        for (uint16_t j = 0; j < i; ++j) {
            const datlink_image_segment_t *other = &manifest->segments[j];
            if (segment->address < other->address + other->length &&
                other->address < segment->address + segment->length) {
                return ESP_ERR_INVALID_ARG;
            }
            if (segment->data_offset < other->data_offset + other->length &&
                other->data_offset < segment->data_offset + segment->length) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return sum == manifest->total_length ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

size_t datlink_manifest_encode(const datlink_image_manifest_t *manifest,
                               uint8_t *output, size_t capacity)
{
    if (datlink_manifest_validate(manifest) != ESP_OK || output == NULL) return 0;
    const size_t length = 48U + (size_t)manifest->segment_count * 16U;
    if (capacity < length) return 0;
    put_u16(output + 0, manifest->format_version);
    put_u16(output + 2, manifest->segment_count);
    put_u32(output + 4, manifest->target);
    put_u32(output + 8, manifest->operation_id);
    put_u32(output + 12, manifest->total_length);
    memcpy(output + 16, manifest->sha256, DATLINK_SHA256_LEN);
    size_t offset = 48;
    for (uint16_t i = 0; i < manifest->segment_count; ++i) {
        put_u32(output + offset + 0, manifest->segments[i].address);
        put_u32(output + offset + 4, manifest->segments[i].length);
        put_u32(output + offset + 8, manifest->segments[i].data_offset);
        put_u32(output + offset + 12, manifest->segments[i].crc32c);
        offset += 16;
    }
    return length;
}

esp_err_t datlink_manifest_decode(const uint8_t *input, size_t length,
                                  datlink_image_manifest_t *manifest)
{
    if (input == NULL || manifest == NULL || length < 64U) return ESP_ERR_INVALID_ARG;
    const uint16_t count = get_u16(input + 2);
    if (count == 0U || count > DATLINK_IMAGE_MAX_SEGMENTS ||
        length != 48U + (size_t)count * 16U) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(manifest, 0, sizeof(*manifest));
    manifest->format_version = get_u16(input + 0);
    manifest->segment_count = count;
    manifest->target = get_u32(input + 4);
    manifest->operation_id = get_u32(input + 8);
    manifest->total_length = get_u32(input + 12);
    memcpy(manifest->sha256, input + 16, DATLINK_SHA256_LEN);
    size_t offset = 48;
    for (uint16_t i = 0; i < count; ++i) {
        manifest->segments[i].address = get_u32(input + offset + 0);
        manifest->segments[i].length = get_u32(input + offset + 4);
        manifest->segments[i].data_offset = get_u32(input + offset + 8);
        manifest->segments[i].crc32c = get_u32(input + offset + 12);
        offset += 16;
    }
    return datlink_manifest_validate(manifest);
}

size_t datlink_progress_encode(const datlink_progress_t *progress,
                               uint8_t *output, size_t capacity)
{
    if (progress == NULL || output == NULL || capacity < DATLINK_PROGRESS_WIRE_LEN) return 0;
    put_u32(output + 0, (uint32_t)progress->status);
    output[4] = progress->phase;
    memset(output + 5, 0, 3);
    put_u32(output + 8, progress->completed);
    put_u32(output + 12, progress->total);
    put_u32(output + 16, progress->detail);
    return DATLINK_PROGRESS_WIRE_LEN;
}

esp_err_t datlink_progress_decode(const uint8_t *input, size_t length,
                                  datlink_progress_t *progress)
{
    if (input == NULL || progress == NULL || length != DATLINK_PROGRESS_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(progress, 0, sizeof(*progress));
    progress->status = (int32_t)get_u32(input + 0);
    progress->phase = input[4];
    progress->completed = get_u32(input + 8);
    progress->total = get_u32(input + 12);
    progress->detail = get_u32(input + 16);
    return ESP_OK;
}

size_t datlink_target_info_encode(const datlink_target_info_t *info,
                                  uint8_t *output, size_t capacity)
{
    if (info == NULL || output == NULL || capacity < DATLINK_TARGET_INFO_WIRE_LEN) return 0;
    put_u32(output + 0, info->dpidr);
    put_u32(output + 4, info->ap_idr);
    put_u32(output + 8, info->cpuid);
    put_u32(output + 12, info->factory_device_id);
    put_u32(output + 16, info->factory_user_id);
    put_u32(output + 20, info->factory_sramflash);
    put_u32(output + 24, info->swd_clock_khz);
    return DATLINK_TARGET_INFO_WIRE_LEN;
}

esp_err_t datlink_target_info_decode(const uint8_t *input, size_t length,
                                     datlink_target_info_t *info)
{
    if (input == NULL || info == NULL || length != DATLINK_TARGET_INFO_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    info->dpidr = get_u32(input + 0);
    info->ap_idr = get_u32(input + 4);
    info->cpuid = get_u32(input + 8);
    info->factory_device_id = get_u32(input + 12);
    info->factory_user_id = get_u32(input + 16);
    info->factory_sramflash = get_u32(input + 20);
    info->swd_clock_khz = get_u32(input + 24);
    return ESP_OK;
}

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "datlink_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t datlink_storage_init(void);
esp_err_t datlink_storage_begin(const datlink_image_manifest_t *manifest);
esp_err_t datlink_storage_write(uint32_t offset, const void *data, size_t length);
esp_err_t datlink_storage_finalize(void);
esp_err_t datlink_storage_read(uint32_t offset, void *data, size_t length);
bool datlink_storage_ready(void);
uint32_t datlink_storage_bytes_written(void);
const datlink_image_manifest_t *datlink_storage_manifest(void);
void datlink_storage_invalidate(void);

#ifdef __cplusplus
}
#endif

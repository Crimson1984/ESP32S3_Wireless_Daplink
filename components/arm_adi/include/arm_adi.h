#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "datlink_common.h"
#include "esp_err.h"

typedef struct {
    uint32_t dpidr;
    uint32_t ap_idr;
    uint32_t clock_khz;
} arm_adi_info_t;

esp_err_t arm_adi_init(void);
datlink_status_t arm_adi_connect(bool under_reset, arm_adi_info_t *info);
void arm_adi_disconnect(void);
datlink_status_t arm_adi_read_dp(uint8_t address, uint32_t *value);
datlink_status_t arm_adi_write_dp(uint8_t address, uint32_t value);
datlink_status_t arm_adi_read_ap(uint8_t address, uint32_t *value);
datlink_status_t arm_adi_write_ap(uint8_t address, uint32_t value);
datlink_status_t arm_adi_mem_read32(uint32_t address, uint32_t *value);
datlink_status_t arm_adi_mem_write32(uint32_t address, uint32_t value);
datlink_status_t arm_adi_mem_read(uint32_t address, void *data, size_t length);
datlink_status_t arm_adi_mem_write(uint32_t address, const void *data, size_t length);


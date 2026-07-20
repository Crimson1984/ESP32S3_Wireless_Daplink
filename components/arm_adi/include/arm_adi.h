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
    uint32_t stage;
} arm_adi_info_t;

typedef enum {
    ARM_ADI_STAGE_NONE = 0,
    ARM_ADI_STAGE_VTREF = 1,
    ARM_ADI_STAGE_DPIDR = 2,
    ARM_ADI_STAGE_ABORT_CLEAR = 3,
    ARM_ADI_STAGE_DP_SELECT = 4,
    ARM_ADI_STAGE_POWER_REQUEST = 5,
    ARM_ADI_STAGE_POWER_ACK = 6,
    ARM_ADI_STAGE_AP_IDR = 7,
    ARM_ADI_STAGE_AP_CSW = 8,
    ARM_ADI_STAGE_HALT = 9,
    ARM_ADI_STAGE_IDENTIFY = 10,
    ARM_ADI_STAGE_SRAM_TEST = 11,
    ARM_ADI_STAGE_RESET = 12,
    ARM_ADI_STAGE_LOADER_UPLOAD = 13,
    ARM_ADI_STAGE_LOADER_EXECUTE = 14,
    ARM_ADI_STAGE_FLASH_READ = 15,
} arm_adi_stage_t;

esp_err_t arm_adi_init(void);
datlink_status_t arm_adi_connect(bool under_reset, arm_adi_info_t *info);
datlink_status_t arm_adi_hardware_reset(void);
void arm_adi_disconnect(void);
datlink_status_t arm_adi_read_dp(uint8_t address, uint32_t *value);
datlink_status_t arm_adi_write_dp(uint8_t address, uint32_t value);
datlink_status_t arm_adi_read_ap(uint8_t address, uint32_t *value);
datlink_status_t arm_adi_write_ap(uint8_t address, uint32_t value);
datlink_status_t arm_adi_mem_read32(uint32_t address, uint32_t *value);
datlink_status_t arm_adi_mem_write32(uint32_t address, uint32_t value);
datlink_status_t arm_adi_mem_read(uint32_t address, void *data, size_t length);
datlink_status_t arm_adi_mem_write(uint32_t address, const void *data, size_t length);

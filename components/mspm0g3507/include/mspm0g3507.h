#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "arm_adi.h"
#include "datlink_common.h"
#include "datlink_protocol.h"

#define MSPM0G3507_FLASH_BASE       0x00000000U
#define MSPM0G3507_FLASH_SIZE       0x00020000U
#define MSPM0G3507_FLASH_SECTOR_SIZE 1024U
#define MSPM0G3507_SRAM_BASE        0x20200000U
#define MSPM0G3507_SRAM_SIZE        0x00008000U
#define MSPM0G3507_FACTORY_BASE     0x41C40000U

typedef struct {
    arm_adi_info_t adi;
    uint32_t cpuid;
    uint32_t factory_trace_id;
    uint32_t factory_device_id;
    uint32_t factory_user_id;
    uint32_t factory_sramflash;
} mspm0g3507_info_t;

datlink_status_t mspm0g3507_identify(mspm0g3507_info_t *info);
datlink_status_t mspm0g3507_sram_self_test(void);
bool mspm0g3507_range_is_main(uint32_t address, uint32_t length);
datlink_status_t mspm0g3507_validate_manifest(const datlink_image_manifest_t *manifest);

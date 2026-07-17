#include "mspm0g3507.h"

#include <string.h>

#include "cortexm_debug.h"

#define FACTORY_TRACEID  (MSPM0G3507_FACTORY_BASE + 0x00U)
#define FACTORY_DEVICEID (MSPM0G3507_FACTORY_BASE + 0x04U)
#define FACTORY_USERID   (MSPM0G3507_FACTORY_BASE + 0x08U)
#define FACTORY_SRAMFLASH (MSPM0G3507_FACTORY_BASE + 0x18U)
#define FACTORY_MAIN_KB_MASK 0x00000FFFU
#define FACTORY_SRAM_KB_MASK 0x03FF0000U
#define FACTORY_SRAM_KB_SHIFT 16U

bool mspm0g3507_range_is_main(uint32_t address, uint32_t length)
{
    if (length == 0U || address >= MSPM0G3507_FLASH_SIZE) return false;
    return length <= MSPM0G3507_FLASH_SIZE - address;
}

datlink_status_t mspm0g3507_validate_manifest(const datlink_image_manifest_t *manifest)
{
    if (manifest == NULL || manifest->target != DATLINK_TARGET_MSPM0G3507 ||
        manifest->segment_count == 0U ||
        manifest->segment_count > DATLINK_IMAGE_MAX_SEGMENTS) {
        return DATLINK_ERR_ARGUMENT;
    }
    for (uint16_t i = 0; i < manifest->segment_count; ++i) {
        const datlink_image_segment_t *segment = &manifest->segments[i];
        if (!mspm0g3507_range_is_main(segment->address, segment->length)) {
            return DATLINK_ERR_RANGE;
        }
    }
    return DATLINK_OK;
}

datlink_status_t mspm0g3507_identify(mspm0g3507_info_t *info)
{
    if (info == NULL) return DATLINK_ERR_ARGUMENT;
    memset(info, 0, sizeof(*info));
    datlink_status_t status = cortexm_read_cpuid(&info->cpuid);
    if (status == DATLINK_OK) status = arm_adi_mem_read32(FACTORY_TRACEID, &info->factory_trace_id);
    if (status == DATLINK_OK) status = arm_adi_mem_read32(FACTORY_DEVICEID, &info->factory_device_id);
    if (status == DATLINK_OK) status = arm_adi_mem_read32(FACTORY_USERID, &info->factory_user_id);
    if (status == DATLINK_OK) status = arm_adi_mem_read32(FACTORY_SRAMFLASH, &info->factory_sramflash);
    if (status != DATLINK_OK) return status;

    /* ARM implementer 0x41 and Cortex-M0+ part number 0xC60. */
    if ((info->cpuid & 0xFF000000U) != 0x41000000U ||
        (info->cpuid & 0x0000FFF0U) != 0x0000C600U ||
        (info->factory_device_id & 1U) == 0U) {
        return DATLINK_ERR_TARGET_ID;
    }
    const uint32_t main_kb = info->factory_sramflash & FACTORY_MAIN_KB_MASK;
    const uint32_t sram_kb = (info->factory_sramflash & FACTORY_SRAM_KB_MASK) >> FACTORY_SRAM_KB_SHIFT;
    if (main_kb != MSPM0G3507_FLASH_SIZE / 1024U ||
        sram_kb != MSPM0G3507_SRAM_SIZE / 1024U) {
        return DATLINK_ERR_TARGET_ID;
    }
    return DATLINK_OK;
}

datlink_status_t mspm0g3507_sram_self_test(void)
{
    static const uint32_t addresses[] = {0x20202800U, 0x20202FFCU, 0x20207FFCU};
    static const uint32_t patterns[] = {0xA5A55A5AU, 0x01234567U, 0xDEADBEEFU};
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        uint32_t original = 0, actual = 0;
        datlink_status_t status = arm_adi_mem_read32(addresses[i], &original);
        if (status == DATLINK_OK) status = arm_adi_mem_write32(addresses[i], patterns[i]);
        if (status == DATLINK_OK) status = arm_adi_mem_read32(addresses[i], &actual);
        (void)arm_adi_mem_write32(addresses[i], original);
        if (status != DATLINK_OK) return status;
        if (actual != patterns[i]) return DATLINK_ERR_VERIFY;
    }
    return DATLINK_OK;
}

#include "arm_adi.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "swd_phy.h"

#define DP_ABORT 0x0U
#define DP_IDCODE 0x0U
#define DP_CTRL_STAT 0x4U
#define DP_SELECT 0x8U
#define DP_RDBUFF 0xCU

#define AP_CSW 0x00U
#define AP_TAR 0x04U
#define AP_DRW 0x0CU
#define AP_IDR 0xFCU

#define DP_ABORT_CLEAR_ALL 0x1EU
#define CTRLSTAT_POWER_REQ 0x50000000U
#define CTRLSTAT_POWER_ACK 0xA0000000U
#define MEMAP_CSW_32_AUTOINC 0x23000052U

static const char *TAG = "arm_adi";
static uint32_t s_selected = UINT32_MAX;
static bool s_connected;

static datlink_status_t transfer_retry(bool ap, bool read, uint8_t address,
                                       uint32_t *data)
{
    for (unsigned retry = 0; retry < 100U; ++retry) {
        datlink_status_t status = swd_phy_transfer(ap, read, address, data);
        if (status == DATLINK_OK) return status;
        if (status == DATLINK_ERR_SWD_ACK_WAIT) {
            if ((retry % 10U) == 9U) vTaskDelay(1);
            continue;
        }
        if (status == DATLINK_ERR_SWD_ACK_FAULT) {
            uint32_t clear = DP_ABORT_CLEAR_ALL;
            (void)swd_phy_transfer(false, false, DP_ABORT, &clear);
        }
        return status;
    }
    return DATLINK_ERR_TIMEOUT;
}

datlink_status_t arm_adi_read_dp(uint8_t address, uint32_t *value)
{
    return transfer_retry(false, true, address & 0x0CU, value);
}

datlink_status_t arm_adi_write_dp(uint8_t address, uint32_t value)
{
    return transfer_retry(false, false, address & 0x0CU, &value);
}

static datlink_status_t select_ap_bank(uint8_t address)
{
    const uint32_t select = (uint32_t)(address & 0xF0U);
    if (select == s_selected) return DATLINK_OK;
    datlink_status_t status = arm_adi_write_dp(DP_SELECT, select);
    if (status == DATLINK_OK) s_selected = select;
    return status;
}

datlink_status_t arm_adi_read_ap(uint8_t address, uint32_t *value)
{
    datlink_status_t status = select_ap_bank(address);
    if (status != DATLINK_OK) return status;
    uint32_t posted;
    status = transfer_retry(true, true, address & 0x0CU, &posted);
    return status == DATLINK_OK ? arm_adi_read_dp(DP_RDBUFF, value) : status;
}

datlink_status_t arm_adi_write_ap(uint8_t address, uint32_t value)
{
    datlink_status_t status = select_ap_bank(address);
    return status == DATLINK_OK
               ? transfer_retry(true, false, address & 0x0CU, &value)
               : status;
}

esp_err_t arm_adi_init(void)
{
    return swd_phy_init();
}

static datlink_status_t connect_at_speed(uint32_t khz, bool under_reset,
                                         arm_adi_info_t *info)
{
    swd_phy_set_clock_khz(khz);
    if (swd_phy_enable() != ESP_OK) return DATLINK_ERR_TARGET_POWER;
    swd_phy_assert_reset(under_reset);
    vTaskDelay(pdMS_TO_TICKS(2));
    swd_phy_line_reset();
    s_selected = UINT32_MAX;

    uint32_t dpidr = 0;
    datlink_status_t status = arm_adi_read_dp(DP_IDCODE, &dpidr);
    if (status != DATLINK_OK || dpidr == 0U || dpidr == UINT32_MAX) {
        swd_phy_safe_state();
        return status == DATLINK_OK ? DATLINK_ERR_TARGET_ID : status;
    }
    status = arm_adi_write_dp(DP_ABORT, DP_ABORT_CLEAR_ALL);
    if (status != DATLINK_OK) return status;
    status = arm_adi_write_dp(DP_SELECT, 0);
    if (status != DATLINK_OK) return status;
    s_selected = 0;
    status = arm_adi_write_dp(DP_CTRL_STAT, CTRLSTAT_POWER_REQ);
    if (status != DATLINK_OK) return status;
    uint32_t ctrl = 0;
    for (unsigned retry = 0; retry < 100U; ++retry) {
        status = arm_adi_read_dp(DP_CTRL_STAT, &ctrl);
        if (status == DATLINK_OK && (ctrl & CTRLSTAT_POWER_ACK) == CTRLSTAT_POWER_ACK) break;
        vTaskDelay(1);
    }
    if ((ctrl & CTRLSTAT_POWER_ACK) != CTRLSTAT_POWER_ACK) return DATLINK_ERR_TIMEOUT;

    uint32_t ap_idr = 0;
    status = arm_adi_read_ap(AP_IDR, &ap_idr);
    if (status != DATLINK_OK) return status;
    status = arm_adi_write_ap(AP_CSW, MEMAP_CSW_32_AUTOINC);
    if (status != DATLINK_OK) return status;
    s_connected = true;
    if (under_reset) swd_phy_assert_reset(false);
    if (info != NULL) {
        info->dpidr = dpidr;
        info->ap_idr = ap_idr;
        info->clock_khz = khz;
    }
    return DATLINK_OK;
}

datlink_status_t arm_adi_connect(bool under_reset, arm_adi_info_t *info)
{
    const uint32_t speeds[] = {100U, 1000U, 500U, 250U, 100U};
    datlink_status_t last = DATLINK_ERR_LINK;
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); ++i) {
        last = connect_at_speed(speeds[i], under_reset, info);
        if (last == DATLINK_OK) {
            ESP_LOGI(TAG, "connected DPIDR=0x%08" PRIx32 " APIDR=0x%08" PRIx32
                          " at %" PRIu32 "kHz",
                     info != NULL ? info->dpidr : 0,
                     info != NULL ? info->ap_idr : 0, speeds[i]);
            return DATLINK_OK;
        }
    }
    return last;
}

void arm_adi_disconnect(void)
{
    s_connected = false;
    swd_phy_safe_state();
}

datlink_status_t arm_adi_mem_read32(uint32_t address, uint32_t *value)
{
    if (!s_connected || value == NULL || (address & 3U) != 0U) return DATLINK_ERR_ARGUMENT;
    datlink_status_t status = arm_adi_write_ap(AP_CSW, MEMAP_CSW_32_AUTOINC);
    if (status == DATLINK_OK) status = arm_adi_write_ap(AP_TAR, address);
    return status == DATLINK_OK ? arm_adi_read_ap(AP_DRW, value) : status;
}

datlink_status_t arm_adi_mem_write32(uint32_t address, uint32_t value)
{
    if (!s_connected || (address & 3U) != 0U) return DATLINK_ERR_ARGUMENT;
    datlink_status_t status = arm_adi_write_ap(AP_CSW, MEMAP_CSW_32_AUTOINC);
    if (status == DATLINK_OK) status = arm_adi_write_ap(AP_TAR, address);
    return status == DATLINK_OK ? arm_adi_write_ap(AP_DRW, value) : status;
}

datlink_status_t arm_adi_mem_read(uint32_t address, void *data, size_t length)
{
    if (data == NULL || length == 0U) return DATLINK_ERR_ARGUMENT;
    uint8_t *output = data;
    while (length > 0U) {
        const uint32_t aligned = address & ~3U;
        uint32_t word;
        datlink_status_t status = arm_adi_mem_read32(aligned, &word);
        if (status != DATLINK_OK) return status;
        const unsigned start = address & 3U;
        size_t count = 4U - start;
        if (count > length) count = length;
        memcpy(output, ((uint8_t *)&word) + start, count);
        address += (uint32_t)count;
        output += count;
        length -= count;
    }
    return DATLINK_OK;
}

datlink_status_t arm_adi_mem_write(uint32_t address, const void *data, size_t length)
{
    if (data == NULL || length == 0U) return DATLINK_ERR_ARGUMENT;
    const uint8_t *input = data;
    while (length > 0U) {
        const uint32_t aligned = address & ~3U;
        const unsigned start = address & 3U;
        size_t count = 4U - start;
        if (count > length) count = length;
        uint32_t word = 0;
        if (start != 0U || count != 4U) {
            datlink_status_t status = arm_adi_mem_read32(aligned, &word);
            if (status != DATLINK_OK) return status;
        }
        memcpy(((uint8_t *)&word) + start, input, count);
        datlink_status_t status = arm_adi_mem_write32(aligned, word);
        if (status != DATLINK_OK) return status;
        address += (uint32_t)count;
        input += count;
        length -= count;
    }
    return DATLINK_OK;
}

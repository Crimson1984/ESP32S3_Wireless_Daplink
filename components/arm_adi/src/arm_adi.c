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
#define RESET_RELEASE_SETTLE_MS 10U
#define AP_IDR_RECOVERY_RETRIES 100U
#define AP_IDR_RETRY_DELAY_MS 1U

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

static datlink_status_t read_ap_idr_after_reset(uint32_t *value)
{
    datlink_status_t last_status = DATLINK_ERR_TIMEOUT;
    uint32_t last_value = 0U;

    for (unsigned retry = 0; retry < AP_IDR_RECOVERY_RETRIES; ++retry) {
        last_value = 0U;
        last_status = arm_adi_read_ap(AP_IDR, &last_value);
        if (last_status == DATLINK_OK && last_value != 0U &&
            last_value != UINT32_MAX) {
            *value = last_value;
            if (retry != 0U) {
                ESP_LOGI(TAG, "MEM-AP recovered %u ms after reset release",
                         retry * AP_IDR_RETRY_DELAY_MS);
            }
            return DATLINK_OK;
        }

        if (last_status != DATLINK_OK &&
            last_status != DATLINK_ERR_SWD_ACK_WAIT &&
            last_status != DATLINK_ERR_SWD_ACK_FAULT) {
            *value = last_value;
            return last_status;
        }

        /* A target can transiently return WAIT/FAULT while its debug power
         * domain recovers after nRESET is released. Clear sticky DP state and
         * force APBANKSEL to be written again before the next bounded trial. */
        uint32_t clear = DP_ABORT_CLEAR_ALL;
        (void)swd_phy_transfer(false, false, DP_ABORT, &clear);
        s_selected = UINT32_MAX;
        vTaskDelay(pdMS_TO_TICKS(AP_IDR_RETRY_DELAY_MS));
    }

    *value = last_value;
    return last_status == DATLINK_OK ? DATLINK_ERR_TARGET_ID : last_status;
}

static datlink_status_t connect_at_speed(uint32_t khz, bool under_reset,
                                         arm_adi_info_t *info)
{
    s_connected = false;
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->clock_khz = khz;
        info->stage = ARM_ADI_STAGE_VTREF;
    }
    swd_phy_set_clock_khz(khz);
    if (swd_phy_enable() != ESP_OK) return DATLINK_ERR_TARGET_POWER;
    swd_phy_assert_reset(under_reset);
    vTaskDelay(pdMS_TO_TICKS(2));
    swd_phy_line_reset();
    s_selected = UINT32_MAX;

    uint32_t dpidr = 0;
    if (info != NULL) info->stage = ARM_ADI_STAGE_DPIDR;
    datlink_status_t status = arm_adi_read_dp(DP_IDCODE, &dpidr);
    if (info != NULL) info->dpidr = dpidr;
    if (status != DATLINK_OK || dpidr == 0U || dpidr == UINT32_MAX) {
        swd_phy_safe_state();
        return status == DATLINK_OK ? DATLINK_ERR_TARGET_ID : status;
    }
    if (info != NULL) info->stage = ARM_ADI_STAGE_ABORT_CLEAR;
    status = arm_adi_write_dp(DP_ABORT, DP_ABORT_CLEAR_ALL);
    if (status != DATLINK_OK) return status;
    if (info != NULL) info->stage = ARM_ADI_STAGE_DP_SELECT;
    status = arm_adi_write_dp(DP_SELECT, 0);
    if (status != DATLINK_OK) return status;
    s_selected = 0;
    if (info != NULL) info->stage = ARM_ADI_STAGE_POWER_REQUEST;
    status = arm_adi_write_dp(DP_CTRL_STAT, CTRLSTAT_POWER_REQ);
    if (status != DATLINK_OK) return status;
    uint32_t ctrl = 0;
    if (info != NULL) info->stage = ARM_ADI_STAGE_POWER_ACK;
    for (unsigned retry = 0; retry < 100U; ++retry) {
        status = arm_adi_read_dp(DP_CTRL_STAT, &ctrl);
        if (status == DATLINK_OK && (ctrl & CTRLSTAT_POWER_ACK) == CTRLSTAT_POWER_ACK) break;
        vTaskDelay(1);
    }
    if ((ctrl & CTRLSTAT_POWER_ACK) != CTRLSTAT_POWER_ACK) return DATLINK_ERR_TIMEOUT;

    /* MSPM0 exposes the SW-DP while nRESET is asserted, but its MEM-AP IDR
     * reads as zero until the target reset is released.  Complete DP power-up
     * under reset, then release before selecting and validating the AP. */
    if (under_reset) {
        swd_phy_assert_reset(false);
        vTaskDelay(pdMS_TO_TICKS(RESET_RELEASE_SETTLE_MS));
    }

    uint32_t ap_idr = 0;
    if (info != NULL) info->stage = ARM_ADI_STAGE_AP_IDR;
    status = under_reset ? read_ap_idr_after_reset(&ap_idr)
                         : arm_adi_read_ap(AP_IDR, &ap_idr);
    if (info != NULL) info->ap_idr = ap_idr;
    if (status != DATLINK_OK) return status;
    if (ap_idr == 0U || ap_idr == UINT32_MAX) return DATLINK_ERR_TARGET_ID;
    if (info != NULL) info->stage = ARM_ADI_STAGE_AP_CSW;
    status = arm_adi_write_ap(AP_CSW, MEMAP_CSW_32_AUTOINC);
    if (status != DATLINK_OK) return status;
    s_connected = true;
    if (info != NULL) {
        info->dpidr = dpidr;
        info->ap_idr = ap_idr;
        info->clock_khz = khz;
        info->stage = ARM_ADI_STAGE_NONE;
    }
    return DATLINK_OK;
}

datlink_status_t arm_adi_connect(bool under_reset, arm_adi_info_t *info)
{
    arm_adi_info_t initial = {0};
    datlink_status_t status = connect_at_speed(100U, under_reset, &initial);
    if (status != DATLINK_OK) {
        if (info != NULL) *info = initial;
        return status;
    }

    /* Establish the target safely at 100 kHz first, then reconnect without
     * another hardware reset at the fastest validated rate.  A failed trial
     * is recovered by a complete lower-speed line reset and DP/AP init. */
    static const uint32_t promote_speeds[] = {1000U, 500U, 250U};
    for (size_t i = 0; i < sizeof(promote_speeds) / sizeof(promote_speeds[0]); ++i) {
        arm_adi_info_t candidate = {0};
        status = connect_at_speed(promote_speeds[i], false, &candidate);
        if (status == DATLINK_OK) {
            if (info != NULL) *info = candidate;
            ESP_LOGI(TAG, "connected DPIDR=0x%08" PRIx32 " APIDR=0x%08" PRIx32
                          " at %" PRIu32 "kHz",
                     candidate.dpidr, candidate.ap_idr, candidate.clock_khz);
            return DATLINK_OK;
        }
        ESP_LOGW(TAG, "SWD promotion to %" PRIu32 "kHz failed at stage %" PRIu32
                      ": %s",
                 promote_speeds[i], candidate.stage, datlink_status_name(status));
    }

    arm_adi_info_t fallback = {0};
    status = connect_at_speed(100U, false, &fallback);
    if (info != NULL) *info = fallback;
    if (status == DATLINK_OK) {
        ESP_LOGW(TAG, "using validated 100kHz SWD fallback");
    }
    return status;
}

datlink_status_t arm_adi_hardware_reset(void)
{
    if (!s_connected || !swd_phy_target_present()) {
        return DATLINK_ERR_TARGET_POWER;
    }
    swd_phy_assert_reset(true);
    vTaskDelay(pdMS_TO_TICKS(5));
    swd_phy_assert_reset(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    return DATLINK_OK;
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

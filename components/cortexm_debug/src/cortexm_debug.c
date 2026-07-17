#include "cortexm_debug.h"

#include "arm_adi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DHCSR  0xE000EDF0U
#define DCRSR  0xE000EDF4U
#define DCRDR  0xE000EDF8U
#define AIRCR  0xE000ED0CU
#define CPUID  0xE000ED00U

#define DBGKEY       0xA05F0000U
#define C_DEBUGEN    (1U << 0)
#define C_HALT       (1U << 1)
#define S_REGRDY     (1U << 16)
#define S_HALT       (1U << 17)
#define REGWnR       (1U << 16)
#define VECTKEY      0x05FA0000U
#define SYSRESETREQ  (1U << 2)

static datlink_status_t wait_dhcsr(uint32_t mask, bool set, uint32_t timeout_ms)
{
    const uint64_t deadline = datlink_now_ms() + timeout_ms;
    do {
        uint32_t value = 0;
        datlink_status_t status = arm_adi_mem_read32(DHCSR, &value);
        if (status != DATLINK_OK) return status;
        if (((value & mask) != 0U) == set) return DATLINK_OK;
        vTaskDelay(1);
    } while (datlink_now_ms() < deadline);
    return DATLINK_ERR_TIMEOUT;
}

datlink_status_t cortexm_halt(uint32_t timeout_ms)
{
    datlink_status_t status = arm_adi_mem_write32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT);
    return status == DATLINK_OK ? wait_dhcsr(S_HALT, true, timeout_ms) : status;
}

datlink_status_t cortexm_run(void)
{
    return arm_adi_mem_write32(DHCSR, DBGKEY | C_DEBUGEN);
}

datlink_status_t cortexm_wait_halted(uint32_t timeout_ms)
{
    return wait_dhcsr(S_HALT, true, timeout_ms);
}

datlink_status_t cortexm_read_register(uint8_t reg, uint32_t *value)
{
    if (value == NULL || reg > CORTEXM_REG_MSP) return DATLINK_ERR_ARGUMENT;
    datlink_status_t status = arm_adi_mem_write32(DCRSR, reg);
    if (status == DATLINK_OK) status = wait_dhcsr(S_REGRDY, true, 100U);
    return status == DATLINK_OK ? arm_adi_mem_read32(DCRDR, value) : status;
}

datlink_status_t cortexm_write_register(uint8_t reg, uint32_t value)
{
    if (reg > CORTEXM_REG_MSP) return DATLINK_ERR_ARGUMENT;
    datlink_status_t status = arm_adi_mem_write32(DCRDR, value);
    if (status == DATLINK_OK) status = arm_adi_mem_write32(DCRSR, REGWnR | reg);
    return status == DATLINK_OK ? wait_dhcsr(S_REGRDY, true, 100U) : status;
}

datlink_status_t cortexm_system_reset(bool halt_after_reset)
{
    datlink_status_t status = arm_adi_mem_write32(AIRCR, VECTKEY | SYSRESETREQ);
    if (status != DATLINK_OK) return status;
    vTaskDelay(pdMS_TO_TICKS(20));
    return halt_after_reset ? cortexm_halt(500U) : DATLINK_OK;
}

datlink_status_t cortexm_read_cpuid(uint32_t *cpuid)
{
    return cpuid == NULL ? DATLINK_ERR_ARGUMENT : arm_adi_mem_read32(CPUID, cpuid);
}

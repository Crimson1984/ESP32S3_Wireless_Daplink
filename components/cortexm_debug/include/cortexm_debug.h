#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "datlink_common.h"

#define CORTEXM_REG_R0  0U
#define CORTEXM_REG_SP  13U
#define CORTEXM_REG_LR  14U
#define CORTEXM_REG_PC  15U
#define CORTEXM_REG_XPSR 16U
#define CORTEXM_REG_MSP 17U

datlink_status_t cortexm_halt(uint32_t timeout_ms);
datlink_status_t cortexm_run(void);
datlink_status_t cortexm_wait_halted(uint32_t timeout_ms);
datlink_status_t cortexm_read_register(uint8_t reg, uint32_t *value);
datlink_status_t cortexm_write_register(uint8_t reg, uint32_t value);
datlink_status_t cortexm_system_reset(bool halt_after_reset);
datlink_status_t cortexm_read_cpuid(uint32_t *cpuid);

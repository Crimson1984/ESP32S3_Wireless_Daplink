#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "datlink_protocol.h"
#include "esp_err.h"

typedef enum {
    PROGRAMMER_PHASE_IDLE = 0,
    PROGRAMMER_PHASE_CONNECT = 1,
    PROGRAMMER_PHASE_IDENTIFY = 2,
    PROGRAMMER_PHASE_LOADER = 3,
    PROGRAMMER_PHASE_ERASE = 4,
    PROGRAMMER_PHASE_PROGRAM = 5,
    PROGRAMMER_PHASE_VERIFY = 6,
    PROGRAMMER_PHASE_RESET = 7,
    PROGRAMMER_PHASE_DONE = 8,
    PROGRAMMER_PHASE_FAILED = 9,
    PROGRAMMER_PHASE_ABORTED = 10,
} programmer_phase_t;

typedef datlink_target_info_t programmer_target_info_t;

typedef void (*programmer_progress_cb_t)(const datlink_progress_t *progress,
                                         void *context);

esp_err_t programmer_init(programmer_progress_cb_t callback, void *context);
esp_err_t programmer_start(uint32_t operation_id);
void programmer_abort(void);
esp_err_t programmer_get_progress(datlink_progress_t *progress);
esp_err_t programmer_read_target_info(programmer_target_info_t *info);
esp_err_t programmer_reset_target(void);

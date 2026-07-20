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

typedef struct {
    uint32_t stage;
    uint32_t dpidr;
    uint32_t ap_idr;
    uint32_t swd_clock_khz;
} programmer_target_diagnostic_t;

typedef void (*programmer_progress_cb_t)(const datlink_progress_t *progress,
                                         void *context);

typedef esp_err_t (*programmer_backup_data_cb_t)(
    uint32_t operation_id, uint32_t offset, const uint8_t *data, size_t length,
    void *context);
typedef void (*programmer_backup_done_cb_t)(
    uint32_t operation_id, datlink_status_t status, uint32_t total_length,
    const uint8_t sha256[DATLINK_SHA256_LEN],
    const programmer_target_diagnostic_t *diagnostic, void *context);

esp_err_t programmer_init(programmer_progress_cb_t callback, void *context);
esp_err_t programmer_start(uint32_t operation_id);
void programmer_abort(void);
esp_err_t programmer_get_progress(datlink_progress_t *progress);
datlink_status_t programmer_read_target_info(
    programmer_target_info_t *info, programmer_target_diagnostic_t *diagnostic);
datlink_status_t programmer_test_loader(
    programmer_target_info_t *info, programmer_target_diagnostic_t *diagnostic);
esp_err_t programmer_backup_start(uint32_t operation_id,
                                  programmer_backup_data_cb_t data_callback,
                                  programmer_backup_done_cb_t done_callback,
                                  void *context);
esp_err_t programmer_reset_target(void);

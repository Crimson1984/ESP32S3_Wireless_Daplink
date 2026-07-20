#include "programmer.h"

#include <string.h>
#include <stdatomic.h>

#include "arm_adi.h"
#include "cortexm_debug.h"
#include "datlink_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mspm0_loader.h"
#include "mspm0g3507.h"
#include "psa/crypto.h"

#define PROGRAMMER_TASK_STACK 8192U
#define LOADER_TIMEOUT_MS 3000U

typedef enum {
    PROGRAMMER_TASK_NONE = 0,
    PROGRAMMER_TASK_FLASH,
    PROGRAMMER_TASK_BACKUP,
} programmer_task_kind_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static programmer_progress_cb_t s_callback;
static void *s_callback_context;
static datlink_progress_t s_progress;
static uint32_t s_operation_id;
static atomic_bool s_abort;
static programmer_task_kind_t s_task_kind;
static programmer_backup_data_cb_t s_backup_data_callback;
static programmer_backup_done_cb_t s_backup_done_callback;
static void *s_backup_context;

static esp_err_t status_to_esp(datlink_status_t status)
{
    if (status == DATLINK_OK) return ESP_OK;
    if (status == DATLINK_ERR_ARGUMENT) return ESP_ERR_INVALID_ARG;
    if (status == DATLINK_ERR_STATE) return ESP_ERR_INVALID_STATE;
    if (status == DATLINK_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
    return ESP_FAIL;
}

static void report(programmer_phase_t phase, datlink_status_t status,
                   uint32_t completed, uint32_t total, uint32_t detail)
{
    datlink_progress_t copy = {
        .status = status, .phase = phase, .completed = completed,
        .total = total, .detail = detail,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_progress = copy;
    xSemaphoreGive(s_lock);
    if (s_callback != NULL) s_callback(&copy, s_callback_context);
}

static datlink_status_t connect_target(mspm0g3507_info_t *target,
                                       programmer_target_diagnostic_t *diagnostic)
{
    memset(target, 0, sizeof(*target));
    if (diagnostic != NULL) memset(diagnostic, 0, sizeof(*diagnostic));
    report(PROGRAMMER_PHASE_CONNECT, DATLINK_OK, 0, 0, 0);
    datlink_status_t status = arm_adi_connect(true, &target->adi);
    if (diagnostic != NULL) {
        diagnostic->stage = target->adi.stage;
        diagnostic->dpidr = target->adi.dpidr;
        diagnostic->ap_idr = target->adi.ap_idr;
        diagnostic->swd_clock_khz = target->adi.clock_khz;
    }
    if (status == DATLINK_OK) {
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_HALT;
        status = cortexm_halt(500U);
    }
    if (status == DATLINK_OK) {
        report(PROGRAMMER_PHASE_IDENTIFY, DATLINK_OK, 0, 0, 0);
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_IDENTIFY;
        status = mspm0g3507_identify(target);
    }
    if (status == DATLINK_OK) {
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_SRAM_TEST;
        status = mspm0g3507_sram_self_test();
    }
    if (status == DATLINK_OK && diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_NONE;
    return status;
}

static datlink_status_t erase_sectors(const datlink_image_manifest_t *manifest)
{
    bool sectors[MSPM0G3507_FLASH_SIZE / MSPM0G3507_FLASH_SECTOR_SIZE] = {false};
    uint32_t total = 0;
    for (uint16_t i = 0; i < manifest->segment_count; ++i) {
        const datlink_image_segment_t *segment = &manifest->segments[i];
        uint32_t first = segment->address / MSPM0G3507_FLASH_SECTOR_SIZE;
        uint32_t last = (segment->address + segment->length - 1U) / MSPM0G3507_FLASH_SECTOR_SIZE;
        for (uint32_t sector = first; sector <= last; ++sector) {
            if (!sectors[sector]) { sectors[sector] = true; ++total; }
        }
    }
    uint32_t completed = 0;
    for (uint32_t sector = 0; sector < sizeof(sectors); ++sector) {
        if (!sectors[sector]) continue;
        if (atomic_load(&s_abort)) return DATLINK_ERR_ABORTED;
        mspm0_loader_mailbox_t mailbox = {
            .command = MSPM0_LOADER_CMD_ERASE_SECTOR,
            .address = sector * MSPM0G3507_FLASH_SECTOR_SIZE,
            .length = MSPM0G3507_FLASH_SECTOR_SIZE,
        };
        datlink_status_t status = mspm0_loader_execute(&mailbox, NULL, 0, LOADER_TIMEOUT_MS);
        if (status != DATLINK_OK) return status;
        report(PROGRAMMER_PHASE_ERASE, DATLINK_OK, ++completed, total, mailbox.address);
    }
    return DATLINK_OK;
}

static datlink_status_t program_segments(const datlink_image_manifest_t *manifest)
{
    uint8_t buffer[1024];
    uint32_t completed = 0;
    for (uint16_t i = 0; i < manifest->segment_count; ++i) {
        const datlink_image_segment_t *segment = &manifest->segments[i];
        uint32_t offset = 0;
        while (offset < segment->length) {
            if (atomic_load(&s_abort)) return DATLINK_ERR_ABORTED;
            uint32_t count = segment->length - offset;
            if (count > sizeof(buffer)) count = sizeof(buffer);
            uint32_t padded = (count + 7U) & ~7U;
            memset(buffer, 0xFF, padded);
            if (datlink_storage_read(segment->data_offset + offset, buffer, count) != ESP_OK) {
                return DATLINK_ERR_STORAGE;
            }
            mspm0_loader_mailbox_t mailbox = {
                .command = MSPM0_LOADER_CMD_PROGRAM_64,
                .address = segment->address + offset,
                .length = padded,
                .expected_crc = datlink_crc32c(0, buffer, padded),
            };
            datlink_status_t status = mspm0_loader_execute(&mailbox, buffer, padded,
                                                            LOADER_TIMEOUT_MS);
            if (status != DATLINK_OK) return status;
            offset += count;
            completed += count;
            report(PROGRAMMER_PHASE_PROGRAM, DATLINK_OK, completed,
                   manifest->total_length, mailbox.address);
        }
    }
    return DATLINK_OK;
}

static datlink_status_t verify_segments(const datlink_image_manifest_t *manifest)
{
    uint8_t expected[256], actual[256], digest[32];
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) return DATLINK_ERR_VERIFY;
    uint32_t completed = 0;
    datlink_status_t result = DATLINK_OK;
    for (uint16_t i = 0; i < manifest->segment_count && result == DATLINK_OK; ++i) {
        const datlink_image_segment_t *segment = &manifest->segments[i];
        uint32_t offset = 0;
        uint32_t crc = 0;
        while (offset < segment->length) {
            if (atomic_load(&s_abort)) { result = DATLINK_ERR_ABORTED; break; }
            size_t count = segment->length - offset;
            if (count > sizeof(actual)) count = sizeof(actual);
            if (datlink_storage_read(segment->data_offset + offset, expected, count) != ESP_OK) {
                result = DATLINK_ERR_STORAGE; break;
            }
            result = arm_adi_mem_read(segment->address + offset, actual, count);
            if (result != DATLINK_OK) break;
            if (memcmp(expected, actual, count) != 0) { result = DATLINK_ERR_VERIFY; break; }
            crc = datlink_crc32c(crc, actual, count);
            if (psa_hash_update(&sha, actual, count) != PSA_SUCCESS) {
                result = DATLINK_ERR_VERIFY; break;
            }
            offset += (uint32_t)count;
            completed += (uint32_t)count;
            report(PROGRAMMER_PHASE_VERIFY, DATLINK_OK, completed,
                   manifest->total_length, segment->address + offset);
        }
        if (result == DATLINK_OK && crc != segment->crc32c) result = DATLINK_ERR_CRC;
    }
    size_t digest_length = 0;
    if (result == DATLINK_OK &&
        (psa_hash_finish(&sha, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
         digest_length != sizeof(digest))) result = DATLINK_ERR_VERIFY;
    if (result != DATLINK_OK) (void)psa_hash_abort(&sha);
    if (result == DATLINK_OK && memcmp(digest, manifest->sha256, sizeof(digest)) != 0) {
        result = DATLINK_ERR_VERIFY;
    }
    return result;
}

static void programmer_task(void *arg)
{
    (void)arg;
    const datlink_image_manifest_t manifest = *datlink_storage_manifest();
    datlink_status_t status = mspm0g3507_validate_manifest(&manifest);
    mspm0g3507_info_t target;
    if (status == DATLINK_OK) status = connect_target(&target, NULL);
    if (status == DATLINK_OK) {
        report(PROGRAMMER_PHASE_LOADER, DATLINK_OK, 0, 0, 0);
        status = mspm0_loader_upload();
    }
    if (status == DATLINK_OK) {
        mspm0_loader_mailbox_t mailbox = {.command = MSPM0_LOADER_CMD_PROBE};
        status = mspm0_loader_execute(&mailbox, NULL, 0, LOADER_TIMEOUT_MS);
    }
    if (status == DATLINK_OK) status = erase_sectors(&manifest);
    if (status == DATLINK_OK) status = program_segments(&manifest);
    if (status == DATLINK_OK) status = verify_segments(&manifest);
    if (status == DATLINK_OK) {
        report(PROGRAMMER_PHASE_RESET, DATLINK_OK, manifest.total_length,
               manifest.total_length, 0);
        status = cortexm_system_reset(false);
    }
    if (status == DATLINK_OK) {
        report(PROGRAMMER_PHASE_DONE, status, manifest.total_length,
               manifest.total_length, s_operation_id);
    } else if (status == DATLINK_ERR_ABORTED) {
        report(PROGRAMMER_PHASE_ABORTED, status, s_progress.completed,
               manifest.total_length, s_operation_id);
    } else {
        /* A failure leaves the core halted; disconnect only tri-states the external pins. */
        report(PROGRAMMER_PHASE_FAILED, status, s_progress.completed,
               manifest.total_length, s_operation_id);
    }
    arm_adi_disconnect();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    s_task_kind = PROGRAMMER_TASK_NONE;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

static void backup_task(void *arg)
{
    (void)arg;
    uint8_t data[DATLINK_BACKUP_DATA_BYTES];
    uint8_t digest[DATLINK_SHA256_LEN] = {0};
    programmer_target_diagnostic_t diagnostic = {0};
    mspm0g3507_info_t target;
    uint32_t completed = 0;
    bool connected = false;

    datlink_status_t status = connect_target(&target, &diagnostic);
    if (status == DATLINK_OK) connected = true;

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    bool hash_started = false;
    if (status == DATLINK_OK) {
        if (psa_crypto_init() != PSA_SUCCESS ||
            psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            status = DATLINK_ERR_VERIFY;
        } else {
            hash_started = true;
            diagnostic.stage = ARM_ADI_STAGE_FLASH_READ;
        }
    }

    while (status == DATLINK_OK && completed < MSPM0G3507_FLASH_SIZE) {
        if (atomic_load(&s_abort)) {
            status = DATLINK_ERR_ABORTED;
            break;
        }
        size_t count = MSPM0G3507_FLASH_SIZE - completed;
        if (count > sizeof(data)) count = sizeof(data);
        status = arm_adi_mem_read(MSPM0G3507_FLASH_BASE + completed, data, count);
        if (status != DATLINK_OK) break;
        if (psa_hash_update(&sha, data, count) != PSA_SUCCESS) {
            status = DATLINK_ERR_VERIFY;
            break;
        }
        if (s_backup_data_callback == NULL ||
            s_backup_data_callback(s_operation_id, completed, data, count,
                                   s_backup_context) != ESP_OK) {
            status = DATLINK_ERR_LINK;
            break;
        }
        completed += (uint32_t)count;
    }

    if (status == DATLINK_OK) {
        size_t digest_length = 0;
        if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
            digest_length != sizeof(digest)) {
            status = DATLINK_ERR_VERIFY;
        }
        hash_started = false;
    }
    if (hash_started) (void)psa_hash_abort(&sha);

    if (connected) {
        diagnostic.stage = ARM_ADI_STAGE_RESET;
        const datlink_status_t reset_status = cortexm_system_reset(false);
        if (status == DATLINK_OK && reset_status != DATLINK_OK) status = reset_status;
    }
    /* A partially failed connect may still have driven SWD/nRESET. Always
     * release the external pins even when no valid DP/AP session was formed. */
    arm_adi_disconnect();
    if (status == DATLINK_OK) diagnostic.stage = ARM_ADI_STAGE_NONE;

    if (s_backup_done_callback != NULL) {
        s_backup_done_callback(s_operation_id, status, completed, digest,
                               &diagnostic, s_backup_context);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    s_task_kind = PROGRAMMER_TASK_NONE;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t programmer_init(programmer_progress_cb_t callback, void *context)
{
    if (s_lock != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_callback = callback;
    s_callback_context = context;
    s_progress = (datlink_progress_t){
        .status = DATLINK_OK,
        .phase = PROGRAMMER_PHASE_IDLE,
    };
    return arm_adi_init();
}

esp_err_t programmer_start(uint32_t operation_id)
{
    if (!datlink_storage_ready() || operation_id == 0U) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task != NULL) {
        bool same = s_task_kind == PROGRAMMER_TASK_FLASH &&
                    s_operation_id == operation_id;
        xSemaphoreGive(s_lock);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if ((s_progress.phase == PROGRAMMER_PHASE_DONE ||
         s_progress.phase == PROGRAMMER_PHASE_FAILED) && s_operation_id == operation_id) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_operation_id = operation_id;
    s_task_kind = PROGRAMMER_TASK_FLASH;
    atomic_store(&s_abort, false);
    BaseType_t created = xTaskCreatePinnedToCore(programmer_task, "programmer",
                                                 PROGRAMMER_TASK_STACK, NULL, 8,
                                                 &s_task, 1);
    xSemaphoreGive(s_lock);
    if (created != pdPASS) s_task_kind = PROGRAMMER_TASK_NONE;
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void programmer_abort(void) { atomic_store(&s_abort, true); }

esp_err_t programmer_get_progress(datlink_progress_t *progress)
{
    if (progress == NULL || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *progress = s_progress;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

datlink_status_t programmer_read_target_info(
    programmer_target_info_t *info, programmer_target_diagnostic_t *diagnostic)
{
    if (info == NULL) return DATLINK_ERR_ARGUMENT;
    if (s_task != NULL) return DATLINK_ERR_STATE;
    mspm0g3507_info_t target;
    datlink_status_t status = connect_target(&target, diagnostic);
    if (status == DATLINK_OK) {
        *info = (programmer_target_info_t){
            .dpidr = target.adi.dpidr, .ap_idr = target.adi.ap_idr,
            .cpuid = target.cpuid, .factory_device_id = target.factory_device_id,
            .factory_user_id = target.factory_user_id,
            .factory_sramflash = target.factory_sramflash,
            .swd_clock_khz = target.adi.clock_khz,
        };
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_RESET;
        status = cortexm_system_reset(false);
    }
    arm_adi_disconnect();
    return status;
}

datlink_status_t programmer_test_loader(
    programmer_target_info_t *info, programmer_target_diagnostic_t *diagnostic)
{
    if (info == NULL) return DATLINK_ERR_ARGUMENT;
    if (s_task != NULL) return DATLINK_ERR_STATE;

    mspm0g3507_info_t target;
    datlink_status_t status = connect_target(&target, diagnostic);
    if (status == DATLINK_OK) {
        *info = (programmer_target_info_t){
            .dpidr = target.adi.dpidr, .ap_idr = target.adi.ap_idr,
            .cpuid = target.cpuid, .factory_device_id = target.factory_device_id,
            .factory_user_id = target.factory_user_id,
            .factory_sramflash = target.factory_sramflash,
            .swd_clock_khz = target.adi.clock_khz,
        };
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_LOADER_UPLOAD;
        status = mspm0_loader_upload();
    }
    if (status == DATLINK_OK) {
        mspm0_loader_mailbox_t mailbox = {.command = MSPM0_LOADER_CMD_PROBE};
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_LOADER_EXECUTE;
        status = mspm0_loader_execute(&mailbox, NULL, 0, LOADER_TIMEOUT_MS);
    }
    if (status == DATLINK_OK) {
        if (diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_RESET;
        status = cortexm_system_reset(false);
    }
    if (status == DATLINK_OK && diagnostic != NULL) diagnostic->stage = ARM_ADI_STAGE_NONE;
    arm_adi_disconnect();
    return status;
}

esp_err_t programmer_backup_start(uint32_t operation_id,
                                  programmer_backup_data_cb_t data_callback,
                                  programmer_backup_done_cb_t done_callback,
                                  void *context)
{
    if (operation_id == 0U || data_callback == NULL || done_callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_operation_id = operation_id;
    s_backup_data_callback = data_callback;
    s_backup_done_callback = done_callback;
    s_backup_context = context;
    s_task_kind = PROGRAMMER_TASK_BACKUP;
    atomic_store(&s_abort, false);
    const BaseType_t created = xTaskCreatePinnedToCore(
        backup_task, "target_backup", PROGRAMMER_TASK_STACK, NULL, 8, &s_task, 1);
    if (created != pdPASS) s_task_kind = PROGRAMMER_TASK_NONE;
    xSemaphoreGive(s_lock);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t programmer_reset_target(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    arm_adi_info_t adi;
    datlink_status_t status = arm_adi_connect(true, &adi);
    if (status == DATLINK_OK) status = cortexm_system_reset(false);
    arm_adi_disconnect();
    return status_to_esp(status);
}

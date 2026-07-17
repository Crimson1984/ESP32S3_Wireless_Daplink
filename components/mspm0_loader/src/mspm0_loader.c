#include "mspm0_loader.h"

#include "arm_adi.h"
#include "cortexm_debug.h"
#include "mspm0_loader_blob.h"

datlink_status_t mspm0_loader_upload(void)
{
    if (g_mspm0_loader_blob_size == 0U || g_mspm0_loader_blob_size > 8192U ||
        g_mspm0_loader_entry < MSPM0_LOADER_CODE_ADDRESS ||
        g_mspm0_loader_entry >= MSPM0_LOADER_MAILBOX_ADDRESS) {
        return DATLINK_ERR_LOADER;
    }
    datlink_status_t status = arm_adi_mem_write(MSPM0_LOADER_CODE_ADDRESS,
                                                g_mspm0_loader_blob,
                                                g_mspm0_loader_blob_size);
    if (status != DATLINK_OK) return status;
    uint8_t verify[128];
    size_t offset = 0;
    while (offset < g_mspm0_loader_blob_size) {
        size_t count = g_mspm0_loader_blob_size - offset;
        if (count > sizeof(verify)) count = sizeof(verify);
        status = arm_adi_mem_read(MSPM0_LOADER_CODE_ADDRESS + (uint32_t)offset,
                                  verify, count);
        if (status != DATLINK_OK) return status;
        for (size_t i = 0; i < count; ++i) {
            if (verify[i] != g_mspm0_loader_blob[offset + i]) return DATLINK_ERR_VERIFY;
        }
        offset += count;
    }
    return DATLINK_OK;
}

datlink_status_t mspm0_loader_execute(mspm0_loader_mailbox_t *mailbox,
                                      const void *buffer, size_t buffer_length,
                                      uint32_t timeout_ms)
{
    if (mailbox == NULL || buffer_length > 1024U ||
        (buffer_length != 0U && buffer == NULL)) return DATLINK_ERR_ARGUMENT;
    mailbox->magic = MSPM0_LOADER_MAILBOX_MAGIC;
    mailbox->version = MSPM0_LOADER_VERSION;
    mailbox->buffer_address = MSPM0_LOADER_BUFFER_ADDRESS;
    mailbox->status = MSPM0_LOADER_STATUS_IDLE;
    mailbox->result_code = 0U;
    mailbox->actual_crc = 0U;

    datlink_status_t status = DATLINK_OK;
    if (buffer_length != 0U) {
        status = arm_adi_mem_write(MSPM0_LOADER_BUFFER_ADDRESS, buffer, buffer_length);
    }
    if (status == DATLINK_OK) {
        status = arm_adi_mem_write(MSPM0_LOADER_MAILBOX_ADDRESS, mailbox, sizeof(*mailbox));
    }
    if (status == DATLINK_OK) status = cortexm_write_register(CORTEXM_REG_MSP, MSPM0_LOADER_INITIAL_MSP);
    if (status == DATLINK_OK) status = cortexm_write_register(CORTEXM_REG_R0, MSPM0_LOADER_MAILBOX_ADDRESS);
    if (status == DATLINK_OK) status = cortexm_write_register(CORTEXM_REG_PC, g_mspm0_loader_entry | 1U);
    if (status == DATLINK_OK) status = cortexm_run();
    if (status == DATLINK_OK) status = cortexm_wait_halted(timeout_ms);
    if (status == DATLINK_OK) {
        status = arm_adi_mem_read(MSPM0_LOADER_MAILBOX_ADDRESS, mailbox, sizeof(*mailbox));
    }
    if (status != DATLINK_OK) return status;
    if (mailbox->magic != MSPM0_LOADER_MAILBOX_MAGIC ||
        mailbox->version != MSPM0_LOADER_VERSION ||
        mailbox->status != MSPM0_LOADER_STATUS_DONE || mailbox->result_code != 0U) {
        return DATLINK_ERR_LOADER;
    }
    return DATLINK_OK;
}

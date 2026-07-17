#pragma once

#include <stddef.h>
#include <stdint.h>

#include "datlink_common.h"

#define MSPM0_LOADER_CODE_ADDRESS    0x20200000U
#define MSPM0_LOADER_MAILBOX_ADDRESS 0x20202000U
#define MSPM0_LOADER_BUFFER_ADDRESS  0x20202400U
#define MSPM0_LOADER_INITIAL_MSP     0x20208000U
#define MSPM0_LOADER_MAILBOX_MAGIC   0x304C504DU
#define MSPM0_LOADER_VERSION         1U

typedef enum {
    MSPM0_LOADER_CMD_PROBE = 1,
    MSPM0_LOADER_CMD_ERASE_SECTOR = 2,
    MSPM0_LOADER_CMD_PROGRAM_64 = 3,
    MSPM0_LOADER_CMD_CRC32 = 4,
} mspm0_loader_command_t;

typedef enum {
    MSPM0_LOADER_STATUS_IDLE = 0,
    MSPM0_LOADER_STATUS_BUSY = 1,
    MSPM0_LOADER_STATUS_DONE = 2,
    MSPM0_LOADER_STATUS_ERROR = 3,
} mspm0_loader_status_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command;
    uint32_t address;
    uint32_t length;
    uint32_t buffer_address;
    uint32_t expected_crc;
    uint32_t status;
    uint32_t result_code;
    uint32_t actual_crc;
} mspm0_loader_mailbox_t;

datlink_status_t mspm0_loader_upload(void);
datlink_status_t mspm0_loader_execute(mspm0_loader_mailbox_t *mailbox,
                                      const void *buffer, size_t buffer_length,
                                      uint32_t timeout_ms);

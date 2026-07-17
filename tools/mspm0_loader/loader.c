/*
 * MSPM0G3507 SRAM flash loader.
 * Built from TI MSPM0 SDK DriverLib under its BSD-3-Clause license.
 */
#include <stdint.h>

#include "ti/devices/msp/msp.h"
#include "ti/driverlib/dl_flashctl.h"

#define MAILBOX_MAGIC 0x304C504DU
#define MAILBOX_VERSION 1U
#define FLASH_SIZE 0x20000U
#define SECTOR_SIZE 1024U

enum { CMD_PROBE = 1, CMD_ERASE_SECTOR = 2, CMD_PROGRAM_64 = 3, CMD_CRC32 = 4 };
enum { STATUS_IDLE = 0, STATUS_BUSY = 1, STATUS_DONE = 2, STATUS_ERROR = 3 };
enum { RESULT_OK = 0, RESULT_ARGUMENT = 1, RESULT_RANGE = 2,
       RESULT_FLASH = 3, RESULT_CRC = 4 };

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
} loader_mailbox_t;

static uint32_t crc32c(uint32_t crc, const uint8_t *data, uint32_t length)
{
    crc = ~crc;
    while (length-- != 0U) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static int range_is_main(uint32_t address, uint32_t length)
{
    return length != 0U && address < FLASH_SIZE && length <= FLASH_SIZE - address;
}

static uint32_t erase_sector(uint32_t address)
{
    if ((address & (SECTOR_SIZE - 1U)) != 0U ||
        !range_is_main(address, SECTOR_SIZE)) return RESULT_RANGE;
    DL_FlashCTL_unprotectSector(FLASHCTL, address, DL_FLASHCTL_REGION_SELECT_MAIN);
    return DL_FlashCTL_eraseMemoryFromRAM(FLASHCTL, address,
               DL_FLASHCTL_COMMAND_SIZE_SECTOR) == DL_FLASHCTL_COMMAND_STATUS_PASSED
               ? RESULT_OK : RESULT_FLASH;
}

static uint32_t program_words(uint32_t address, const uint8_t *buffer, uint32_t length)
{
    if ((address & 7U) != 0U || (length & 7U) != 0U || length > 1024U ||
        !range_is_main(address, length)) return RESULT_RANGE;
    for (uint32_t offset = 0; offset < length; offset += 8U) {
        uint32_t words[2];
        words[0] = (uint32_t)buffer[offset] |
                   ((uint32_t)buffer[offset + 1U] << 8) |
                   ((uint32_t)buffer[offset + 2U] << 16) |
                   ((uint32_t)buffer[offset + 3U] << 24);
        words[1] = (uint32_t)buffer[offset + 4U] |
                   ((uint32_t)buffer[offset + 5U] << 8) |
                   ((uint32_t)buffer[offset + 6U] << 16) |
                   ((uint32_t)buffer[offset + 7U] << 24);
        DL_FlashCTL_unprotectSector(FLASHCTL, address + offset,
                                    DL_FLASHCTL_REGION_SELECT_MAIN);
        if (DL_FlashCTL_programMemoryFromRAM64WithECCGenerated(
                FLASHCTL, address + offset, words) != DL_FLASHCTL_COMMAND_STATUS_PASSED) {
            return RESULT_FLASH;
        }
    }
    return RESULT_OK;
}

__attribute__((used, noreturn)) void loader_entry(loader_mailbox_t *mailbox)
{
    uint32_t result = RESULT_ARGUMENT;
    if (mailbox != 0 && mailbox->magic == MAILBOX_MAGIC &&
        mailbox->version == MAILBOX_VERSION) {
        mailbox->status = STATUS_BUSY;
        mailbox->actual_crc = 0U;
        switch (mailbox->command) {
        case CMD_PROBE:
            result = RESULT_OK;
            break;
        case CMD_ERASE_SECTOR:
            result = erase_sector(mailbox->address);
            break;
        case CMD_PROGRAM_64: {
            const uint8_t *buffer = (const uint8_t *)(uintptr_t)mailbox->buffer_address;
            uint32_t crc = crc32c(0, buffer, mailbox->length);
            mailbox->actual_crc = crc;
            result = crc == mailbox->expected_crc
                         ? program_words(mailbox->address, buffer, mailbox->length)
                         : RESULT_CRC;
            break;
        }
        case CMD_CRC32:
            if (range_is_main(mailbox->address, mailbox->length)) {
                mailbox->actual_crc = crc32c(0,
                    (const uint8_t *)(uintptr_t)mailbox->address, mailbox->length);
                result = RESULT_OK;
            } else {
                result = RESULT_RANGE;
            }
            break;
        default:
            result = RESULT_ARGUMENT;
            break;
        }
    }
    if (mailbox != 0) {
        mailbox->result_code = result;
        mailbox->status = result == RESULT_OK ? STATUS_DONE : STATUS_ERROR;
    }
    __asm volatile ("dsb");
    __asm volatile ("bkpt #0");
    for (;;) __asm volatile ("wfi");
}

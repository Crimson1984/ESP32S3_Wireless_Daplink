#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATLINK_PROTOCOL_VERSION 1U
#define DATLINK_SHA256_LEN 32U

typedef enum {
    DATLINK_ROLE_GATEWAY = 1,
    DATLINK_ROLE_PROBE = 2,
} datlink_role_t;

typedef enum {
    DATLINK_OK = 0,
    DATLINK_ERR_ARGUMENT = -1,
    DATLINK_ERR_STATE = -2,
    DATLINK_ERR_CRC = -3,
    DATLINK_ERR_RANGE = -4,
    DATLINK_ERR_TIMEOUT = -5,
    DATLINK_ERR_LINK = -6,
    DATLINK_ERR_STORAGE = -7,
    DATLINK_ERR_TARGET_POWER = -8,
    DATLINK_ERR_SWD_ACK_WAIT = -9,
    DATLINK_ERR_SWD_ACK_FAULT = -10,
    DATLINK_ERR_SWD_PARITY = -11,
    DATLINK_ERR_TARGET_ID = -12,
    DATLINK_ERR_TARGET_LOCKED = -13,
    DATLINK_ERR_LOADER = -14,
    DATLINK_ERR_VERIFY = -15,
    DATLINK_ERR_ABORTED = -16,
} datlink_status_t;

uint16_t datlink_crc16_ccitt(const void *data, size_t length);
uint32_t datlink_crc32c(uint32_t seed, const void *data, size_t length);
esp_err_t datlink_sha256(const void *data, size_t length,
                         uint8_t output[DATLINK_SHA256_LEN]);
uint64_t datlink_now_ms(void);
const char *datlink_status_name(datlink_status_t status);

#ifdef __cplusplus
}
#endif


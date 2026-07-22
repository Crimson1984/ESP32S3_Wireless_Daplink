#include "datlink_common.h"

#include "esp_timer.h"
#include "psa/crypto.h"

uint16_t datlink_crc16_ccitt(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)bytes[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint32_t datlink_crc32c(uint32_t seed, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = ~seed;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

esp_err_t datlink_sha256(const void *data, size_t length,
                         uint8_t output[DATLINK_SHA256_LEN])
{
    if ((data == NULL && length != 0U) || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t written = 0;
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    return psa_hash_compute(PSA_ALG_SHA_256, data, length, output,
                            DATLINK_SHA256_LEN, &written) == PSA_SUCCESS &&
                   written == DATLINK_SHA256_LEN
               ? ESP_OK : ESP_FAIL;
}

uint64_t datlink_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

const char *datlink_status_name(datlink_status_t status)
{
    switch (status) {
    case DATLINK_OK: return "ok";
    case DATLINK_ERR_ARGUMENT: return "argument";
    case DATLINK_ERR_STATE: return "state";
    case DATLINK_ERR_CRC: return "crc";
    case DATLINK_ERR_RANGE: return "range";
    case DATLINK_ERR_TIMEOUT: return "timeout";
    case DATLINK_ERR_LINK: return "link";
    case DATLINK_ERR_STORAGE: return "storage";
    case DATLINK_ERR_TARGET_POWER: return "target_power";
    case DATLINK_ERR_SWD_ACK_WAIT: return "swd_wait";
    case DATLINK_ERR_SWD_ACK_FAULT: return "swd_fault";
    case DATLINK_ERR_SWD_PARITY: return "swd_parity";
    case DATLINK_ERR_TARGET_ID: return "target_id";
    case DATLINK_ERR_TARGET_LOCKED: return "target_locked";
    case DATLINK_ERR_LOADER: return "loader";
    case DATLINK_ERR_VERIFY: return "verify";
    case DATLINK_ERR_ABORTED: return "aborted";
    case DATLINK_ERR_VERSION: return "protocol_version";
    default: return "unknown";
    }
}

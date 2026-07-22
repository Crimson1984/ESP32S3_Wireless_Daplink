#include "datlink_storage.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#define META_MAGIC 0x4D494C44U

typedef struct {
    uint32_t magic;
    uint32_t ready;
    datlink_image_manifest_t manifest;
} stored_meta_t;

static const char *TAG = "storage";
static const esp_partition_t *s_image;
static const esp_partition_t *s_meta;
static datlink_image_manifest_t s_manifest;
static uint32_t s_written;
static bool s_ready;

static size_t erase_size(size_t length)
{
    return (length + 4095U) & ~4095U;
}

esp_err_t datlink_storage_init(void)
{
    s_image = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "target_image");
    s_meta = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "target_meta");
    if (s_image == NULL || s_meta == NULL) {
        ESP_LOGE(TAG, "required target partitions were not found");
        return ESP_ERR_NOT_FOUND;
    }
    s_written = 0;
    s_ready = false;
    memset(&s_manifest, 0, sizeof(s_manifest));
    ESP_LOGI(TAG, "image partition size=%" PRIu32, s_image->size);
    return ESP_OK;
}

esp_err_t datlink_storage_begin(const datlink_image_manifest_t *manifest)
{
    ESP_RETURN_ON_ERROR(datlink_manifest_validate(manifest), TAG, "invalid manifest");
    if (manifest->total_length > s_image->size) return ESP_ERR_INVALID_SIZE;
    s_ready = false;
    s_written = 0;
    s_manifest = *manifest;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_meta, 0, 4096), TAG, "erase metadata");
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_image, 0,
                                                  erase_size(manifest->total_length)),
                        TAG, "erase image");
    return ESP_OK;
}

esp_err_t datlink_storage_write(uint32_t offset, const void *data, size_t length)
{
    if (data == NULL || length == 0U || offset != s_written ||
        offset > s_manifest.total_length || length > s_manifest.total_length - offset) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_partition_write(s_image, offset, data, length), TAG,
                        "write target image");
    s_written += (uint32_t)length;
    return ESP_OK;
}

esp_err_t datlink_storage_read(uint32_t offset, void *data, size_t length)
{
    if (data == NULL || offset > s_manifest.total_length ||
        length > s_manifest.total_length - offset) return ESP_ERR_INVALID_ARG;
    return esp_partition_read(s_image, offset, data, length);
}

esp_err_t datlink_storage_finalize(void)
{
    if (s_written != s_manifest.total_length) return ESP_ERR_INVALID_SIZE;

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_FAIL;
    uint8_t buffer[1024];
    for (uint32_t offset = 0; offset < s_manifest.total_length;) {
        const size_t length = (s_manifest.total_length - offset) > sizeof(buffer)
                                  ? sizeof(buffer)
                                  : s_manifest.total_length - offset;
        ESP_RETURN_ON_ERROR(esp_partition_read(s_image, offset, buffer, length), TAG,
                            "read image for hash");
        if (psa_hash_update(&sha, buffer, length) != PSA_SUCCESS) {
            (void)psa_hash_abort(&sha);
            return ESP_FAIL;
        }
        offset += (uint32_t)length;
    }
    uint8_t digest[DATLINK_SHA256_LEN];
    size_t digest_length = 0;
    if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
        digest_length != sizeof(digest)) return ESP_FAIL;
    if (memcmp(digest, s_manifest.sha256, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "image SHA-256 mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    const stored_meta_t meta = {
        .magic = META_MAGIC,
        .ready = 1,
        .manifest = s_manifest,
    };
    ESP_RETURN_ON_ERROR(esp_partition_write(s_meta, 0, &meta, sizeof(meta)), TAG,
                        "write image metadata");
    s_ready = true;
    ESP_LOGI(TAG, "image ready, bytes=%" PRIu32, s_written);
    return ESP_OK;
}

bool datlink_storage_ready(void) { return s_ready; }
uint32_t datlink_storage_bytes_written(void) { return s_written; }
const datlink_image_manifest_t *datlink_storage_manifest(void) { return &s_manifest; }
void datlink_storage_invalidate(void)
{
    s_ready = false;
    s_written = 0U;
    memset(&s_manifest, 0, sizeof(s_manifest));
}

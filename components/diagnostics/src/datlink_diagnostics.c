#include "datlink_diagnostics.h"

#include <inttypes.h>

#include "datlink_security.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "diagnostics";

static int led_level(bool active)
{
#if CONFIG_DATLINK_STATUS_LED_ACTIVE_HIGH
    return active ? 1 : 0;
#else
    return active ? 0 : 1;
#endif
}

esp_err_t datlink_diagnostics_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DATLINK_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "status LED config");
    gpio_set_level(CONFIG_DATLINK_STATUS_LED_GPIO, led_level(false));

    uint32_t flash_size = 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &flash_size), TAG, "flash size");
    ESP_LOGI(TAG, "ESP-IDF %s flash=%" PRIu32 "MB psram=%uKB",
             esp_get_idf_version(), flash_size / (1024U * 1024U),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024U));
    return ESP_OK;
}

void datlink_diagnostics_set_activity(bool active)
{
    gpio_set_level(CONFIG_DATLINK_STATUS_LED_GPIO, led_level(active));
}

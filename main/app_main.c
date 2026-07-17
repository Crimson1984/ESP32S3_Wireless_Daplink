#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "datlink_diagnostics.h"
#include "datlink_security.h"
#include "datlink_storage.h"
#include "datlink_transport.h"
#if CONFIG_DATLINK_ROLE_GATEWAY
#include "gateway_app.h"
#else
#include "probe_app.h"
#endif

static const char *TAG = "datlink";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(datlink_diagnostics_init());
    ESP_ERROR_CHECK(datlink_security_init());
    ESP_ERROR_CHECK(datlink_storage_init());
    ESP_ERROR_CHECK(datlink_transport_init());

#if CONFIG_DATLINK_ROLE_GATEWAY
    ESP_LOGI(TAG, "starting gateway role");
    ESP_ERROR_CHECK(gateway_app_start());
#elif CONFIG_DATLINK_ROLE_PROBE
    ESP_LOGI(TAG, "starting probe role");
    ESP_ERROR_CHECK(probe_app_start());
#else
#error "A DATLINK firmware role must be selected"
#endif
}

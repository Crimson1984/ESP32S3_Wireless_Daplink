#include "datlink_security.h"

#include <string.h>

#include "datlink_secrets.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "security";
static uint8_t s_actual_mac[6];

datlink_role_t datlink_security_role(void)
{
#if CONFIG_DATLINK_ROLE_GATEWAY
    return DATLINK_ROLE_GATEWAY;
#else
    return DATLINK_ROLE_PROBE;
#endif
}

const uint8_t *datlink_security_local_mac(void)
{
#if CONFIG_DATLINK_ROLE_GATEWAY
    return DATLINK_GENERATED_GATEWAY_MAC;
#else
    return DATLINK_GENERATED_PROBE_MAC;
#endif
}

const uint8_t *datlink_security_peer_mac(void)
{
#if CONFIG_DATLINK_ROLE_GATEWAY
    return DATLINK_GENERATED_PROBE_MAC;
#else
    return DATLINK_GENERATED_GATEWAY_MAC;
#endif
}

const uint8_t *datlink_security_pmk(void) { return DATLINK_GENERATED_PMK; }
const uint8_t *datlink_security_lmk(void) { return DATLINK_GENERATED_LMK; }

esp_err_t datlink_security_init(void)
{
    ESP_RETURN_ON_ERROR(esp_read_mac(s_actual_mac, ESP_MAC_WIFI_STA), TAG,
                        "read station MAC");
    if (memcmp(s_actual_mac, datlink_security_local_mac(), sizeof(s_actual_mac)) != 0) {
        ESP_LOGE(TAG, "firmware role MAC mismatch; actual=" MACSTR " expected=" MACSTR,
                 MAC2STR(s_actual_mac), MAC2STR(datlink_security_local_mac()));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "role=%s local=" MACSTR " peer=" MACSTR,
             datlink_security_role() == DATLINK_ROLE_GATEWAY ? "gateway" : "probe",
             MAC2STR(s_actual_mac), MAC2STR(datlink_security_peer_mac()));
    return ESP_OK;
}

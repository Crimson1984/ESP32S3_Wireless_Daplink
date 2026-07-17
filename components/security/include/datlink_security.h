#pragma once

#include <stdint.h>

#include "datlink_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t datlink_security_init(void);
datlink_role_t datlink_security_role(void);
const uint8_t *datlink_security_local_mac(void);
const uint8_t *datlink_security_peer_mac(void);
const uint8_t *datlink_security_pmk(void);
const uint8_t *datlink_security_lmk(void);

#ifdef __cplusplus
}
#endif


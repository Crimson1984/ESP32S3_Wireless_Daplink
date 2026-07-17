#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t datlink_diagnostics_init(void);
void datlink_diagnostics_set_activity(bool active);


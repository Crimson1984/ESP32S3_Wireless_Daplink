#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "datlink_common.h"
#include "esp_err.h"

typedef enum {
    SWD_ACK_OK = 0x1,
    SWD_ACK_WAIT = 0x2,
    SWD_ACK_FAULT = 0x4,
} swd_ack_t;

esp_err_t swd_phy_init(void);
void swd_phy_safe_state(void);
bool swd_phy_target_present(void);
esp_err_t swd_phy_enable(void);
void swd_phy_set_clock_khz(uint32_t khz);
uint32_t swd_phy_clock_khz(void);
void swd_phy_assert_reset(bool asserted);
void swd_phy_line_reset(void);
datlink_status_t swd_phy_transfer(bool ap, bool read, uint8_t address,
                                  uint32_t *data);


#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "datlink_protocol.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*datlink_transport_handler_t)(
    const datlink_wire_frame_t *frame, void *context);

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_retries;
    uint32_t rx_frames;
    uint32_t rx_duplicates;
    uint32_t rx_crc_errors;
    uint32_t dropped_frames;
} datlink_transport_stats_t;

esp_err_t datlink_transport_init(void);
void datlink_transport_set_handler(datlink_transport_handler_t handler,
                                   void *context);
esp_err_t datlink_transport_send(uint8_t type, uint16_t flags,
                                 const void *payload, size_t length,
                                 TickType_t timeout);
esp_err_t datlink_transport_flush(TickType_t timeout);
bool datlink_transport_link_up(void);
uint32_t datlink_transport_session_id(void);
void datlink_transport_get_stats(datlink_transport_stats_t *stats);

#ifdef __cplusplus
}
#endif

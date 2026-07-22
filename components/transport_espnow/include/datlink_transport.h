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

typedef enum {
    DATLINK_RX_ASYNC = 0,
    DATLINK_RX_DEFER = 1,
} datlink_rx_disposition_t;

typedef struct {
    uint32_t session_id;
    uint32_t sequence;
    uint8_t type;
    uint32_t generation;
} datlink_rx_token_t;

typedef datlink_rx_disposition_t (*datlink_transport_handler_t)(
    const datlink_wire_frame_t *frame, const datlink_rx_token_t *token,
    void *context);

typedef enum {
    DATLINK_TRANSPORT_EVENT_PEER_EPOCH_RESET = 1,
    DATLINK_TRANSPORT_EVENT_LOCAL_EPOCH_RESET = 2,
} datlink_transport_event_t;

typedef void (*datlink_transport_event_handler_t)(
    datlink_transport_event_t event, void *context);

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_retries;
    uint32_t rx_frames;
    uint32_t rx_duplicates;
    uint32_t rx_crc_errors;
    uint32_t dropped_frames;
    uint32_t application_defers;
    uint32_t application_rejects;
    uint32_t recovery_count;
} datlink_transport_stats_t;

typedef struct {
    bool up;
    bool recovering;
    uint32_t local_session;
    uint32_t peer_session;
    uint32_t next_tx_sequence;
    uint32_t rx_base;
    uint16_t tx_pending;
    uint16_t rx_pending;
    uint8_t head_state;
    uint32_t head_age_ms;
    int32_t last_error;
    uint32_t recovery_count;
} datlink_transport_status_t;

esp_err_t datlink_transport_init(void);
void datlink_transport_set_handler(datlink_transport_handler_t handler,
                                   void *context);
void datlink_transport_set_event_handler(
    datlink_transport_event_handler_t handler, void *context);
esp_err_t datlink_transport_complete_rx(const datlink_rx_token_t *token,
                                        datlink_status_t status,
                                        uint32_t detail);
esp_err_t datlink_transport_send(uint8_t type, uint16_t flags,
                                 const void *payload, size_t length,
                                 TickType_t timeout);
esp_err_t datlink_transport_flush(TickType_t timeout);
bool datlink_transport_link_up(void);
uint32_t datlink_transport_session_id(void);
void datlink_transport_get_stats(datlink_transport_stats_t *stats);
void datlink_transport_get_status(datlink_transport_status_t *status);
esp_err_t datlink_transport_recover(void);

#ifdef __cplusplus
}
#endif

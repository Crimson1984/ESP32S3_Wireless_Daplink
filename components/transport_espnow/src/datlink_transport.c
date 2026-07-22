#include "datlink_transport.h"

#include <inttypes.h>
#include <string.h>

#include "datlink_diagnostics.h"
#include "datlink_security.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TX_WINDOW 4U
#define RX_WINDOW 4U
#define RTO_MS 50U
#define MAX_RETRIES 8U
#define RECOVERY_RTO_MS 500U
#define HEARTBEAT_MS 500U
#define LINK_TIMEOUT_MS 3000U
#define DEFER_RETRY_MS 20U
#define DEFER_TIMEOUT_MS 2000U
#define GAP_TIMEOUT_MS 3000U
#define RESYNC_RETRY_MS 500U
#define RESYNC_MAX_RETRIES 6U
#define TX_QUEUE_LENGTH 32U
#define ACK_QUEUE_LENGTH 12U
#define RAW_RX_QUEUE_LENGTH 16U

typedef struct {
    bool reliable;
    uint8_t type;
    uint16_t flags;
    uint16_t length;
    uint8_t payload[DATLINK_WIRE_PAYLOAD_MAX];
} tx_item_t;

typedef struct {
    bool used;
    bool sent;
    uint8_t retries;
    uint32_t sequence;
    uint64_t last_sent_ms;
    datlink_wire_frame_t frame;
} tx_slot_t;

typedef struct {
    uint8_t mac[6];
    uint16_t length;
    uint8_t data[DATLINK_WIRE_FRAME_MAX];
} raw_rx_t;

typedef enum {
    RX_SLOT_EMPTY = 0,
    RX_SLOT_RECEIVED = 1,
    RX_SLOT_IN_PROGRESS = 2,
    RX_SLOT_ERROR_PENDING = 3,
    RX_SLOT_COMMITTED = 4,
} rx_slot_state_t;

typedef struct {
    rx_slot_state_t state;
    uint32_t sequence;
    uint32_t generation;
    uint64_t first_seen_ms;
    uint64_t state_since_ms;
    datlink_status_t error_status;
    uint32_t error_detail;
    datlink_wire_frame_t frame;
} rx_slot_t;

static const char *TAG = "espnow_transport";
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_ack_queue;
static QueueHandle_t s_raw_rx_queue;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static tx_slot_t s_tx_slots[TX_WINDOW];
static rx_slot_t s_rx_slots[RX_WINDOW];
static uint32_t s_session_id;
static uint32_t s_peer_session;
static uint32_t s_next_tx_seq = 1U;
static uint32_t s_rx_base;
static uint32_t s_rx_generation = 1U;
static uint64_t s_last_rx_ms;
static uint64_t s_last_tx_ms;
static uint64_t s_rx_progress_ms;
static bool s_link_up;
static bool s_recovering;
static uint32_t s_resync_nonce;
static uint32_t s_resync_reason;
static uint32_t s_resync_expected;
static uint64_t s_resync_last_ms;
static uint8_t s_resync_retries;
static int32_t s_last_error;
static datlink_transport_handler_t s_handler;
static void *s_handler_context;
static datlink_transport_event_handler_t s_event_handler;
static void *s_event_context;
static datlink_transport_stats_t s_stats;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t new_session_id(void)
{
    uint32_t value = esp_random() ^ (uint32_t)datlink_now_ms() ^ s_session_id;
    return value == 0U ? 1U : value;
}

static uint32_t rx_bitmap_locked(void)
{
    uint32_t bitmap = 0U;
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        const rx_slot_t *slot = &s_rx_slots[i];
        if (slot->state == RX_SLOT_COMMITTED && slot->sequence > s_rx_base &&
            slot->sequence <= s_rx_base + 32U) {
            bitmap |= 1UL << (slot->sequence - s_rx_base - 1U);
        }
    }
    return bitmap;
}

static void fill_ack_fields(datlink_wire_frame_t *frame)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    frame->ack_base = s_rx_base;
    frame->ack_bitmap = rx_bitmap_locked();
    xSemaphoreGive(s_lock);
}

static esp_err_t queue_control(uint8_t type, const void *payload, size_t length)
{
    if (length > DATLINK_WIRE_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;
    tx_item_t item = {.reliable = false, .type = type, .length = (uint16_t)length};
    if (payload != NULL && length != 0U) memcpy(item.payload, payload, length);
    return xQueueSend(s_ack_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t queue_reliable(uint8_t type, uint16_t flags,
                                const void *payload, size_t length,
                                TickType_t timeout)
{
    if ((payload == NULL && length != 0U) || length > DATLINK_WIRE_PAYLOAD_MAX ||
        type < DATLINK_MSG_IMAGE_BEGIN) {
        return ESP_ERR_INVALID_ARG;
    }
    tx_item_t item = {
        .reliable = true,
        .type = type,
        .flags = flags,
        .length = (uint16_t)length,
    };
    if (length != 0U) memcpy(item.payload, payload, length);
    return xQueueSend(s_tx_queue, &item, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void process_ack(uint32_t ack_base, uint32_t ack_bitmap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < TX_WINDOW; ++i) {
        if (!s_tx_slots[i].used) continue;
        const uint32_t seq = s_tx_slots[i].sequence;
        bool acked = seq <= ack_base;
        if (!acked && seq > ack_base && seq <= ack_base + 32U) {
            acked = (ack_bitmap & (1UL << (seq - ack_base - 1U))) != 0U;
        }
        if (acked) memset(&s_tx_slots[i], 0, sizeof(s_tx_slots[i]));
    }
    xSemaphoreGive(s_lock);
}

static void reset_rx_session_locked(uint32_t peer_session)
{
    memset(s_rx_slots, 0, sizeof(s_rx_slots));
    s_rx_base = 0U;
    s_peer_session = peer_session;
    if (++s_rx_generation == 0U) s_rx_generation = 1U;
    s_rx_progress_ms = datlink_now_ms();
}

static bool reply_nonce_matches(const datlink_wire_frame_t *frame)
{
    return frame->type == DATLINK_MSG_RESYNC_REPLY &&
           frame->payload_length == DATLINK_RESYNC_WIRE_LEN &&
           get_u32(frame->payload) == s_resync_nonce && s_recovering;
}

static bool accept_session(const datlink_wire_frame_t *frame)
{
    bool accepted = false;
    bool became_up = false;
    bool epoch_reset = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_peer_session == frame->session_id) {
        accepted = true;
    } else if ((frame->type == DATLINK_MSG_HELLO &&
                (s_peer_session == 0U || !s_link_up)) ||
               reply_nonce_matches(frame)) {
        reset_rx_session_locked(frame->session_id);
        accepted = true;
        epoch_reset = true;
    }
    if (accepted) {
        became_up = !s_link_up;
        s_last_rx_ms = datlink_now_ms();
        s_link_up = true;
    }
    xSemaphoreGive(s_lock);
    if (became_up) ESP_LOGI(TAG, "link up, peer session=%" PRIu32, frame->session_id);
    if (epoch_reset && s_event_handler != NULL) {
        s_event_handler(DATLINK_TRANSPORT_EVENT_PEER_EPOCH_RESET, s_event_context);
    }
    return accepted;
}

static rx_slot_t *find_rx_slot_locked(uint32_t sequence)
{
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].state != RX_SLOT_EMPTY &&
            s_rx_slots[i].sequence == sequence) return &s_rx_slots[i];
    }
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].state == RX_SLOT_EMPTY) return &s_rx_slots[i];
    }
    return NULL;
}

static uint32_t message_timeout_ms(uint8_t type)
{
    switch (type) {
    case DATLINK_MSG_IMAGE_BEGIN:
    case DATLINK_MSG_IMAGE_DATA:
    case DATLINK_MSG_IMAGE_END: return 5000U;
    case DATLINK_MSG_TARGET_INFO:
    case DATLINK_MSG_TARGET_RESET: return 10000U;
    case DATLINK_MSG_LOADER_TEST: return 15000U;
    case DATLINK_MSG_PROGRAM_START:
    case DATLINK_MSG_PROGRAM_ABORT:
    case DATLINK_MSG_TARGET_BACKUP_START: return 2000U;
    default: return 2000U;
    }
}

static void advance_committed(void)
{
    bool advanced;
    do {
        advanced = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < RX_WINDOW; ++i) {
            rx_slot_t *slot = &s_rx_slots[i];
            if (slot->state == RX_SLOT_COMMITTED &&
                slot->sequence == s_rx_base + 1U) {
                s_rx_base = slot->sequence;
                memset(slot, 0, sizeof(*slot));
                s_rx_progress_ms = datlink_now_ms();
                advanced = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    } while (advanced);
}

static bool queue_head_error(void)
{
    datlink_wire_frame_t origin = {0};
    datlink_status_t status = DATLINK_OK;
    uint32_t detail = 0U;
    unsigned index = 0U;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].state == RX_SLOT_ERROR_PENDING &&
            s_rx_slots[i].sequence == s_rx_base + 1U) {
            origin = s_rx_slots[i].frame;
            status = s_rx_slots[i].error_status;
            detail = s_rx_slots[i].error_detail;
            index = i;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!found) return false;

    if (origin.type != DATLINK_MSG_COMMAND_ERROR) {
        uint8_t payload[DATLINK_COMMAND_ERROR_WIRE_LEN] = {0};
        put_u32(payload + 0U, origin.session_id);
        put_u32(payload + 4U, origin.sequence);
        payload[8] = origin.type;
        put_u32(payload + 12U, (uint32_t)status);
        put_u32(payload + 16U, detail);
        if (queue_reliable(DATLINK_MSG_COMMAND_ERROR, 0, payload, sizeof(payload), 0) != ESP_OK) {
            return false;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx_slots[index].state == RX_SLOT_ERROR_PENDING &&
        s_rx_slots[index].sequence == origin.sequence) {
        s_rx_slots[index].state = RX_SLOT_COMMITTED;
        ++s_stats.application_rejects;
        s_last_error = status;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "application reject seq=%" PRIu32 " type=%u status=%d detail=0x%08" PRIx32,
             origin.sequence, origin.type, (int)status, detail);
    return true;
}

static void deliver_ordered(void)
{
    for (;;) {
        datlink_wire_frame_t frame = {0};
        datlink_rx_token_t token = {0};
        unsigned index = 0U;
        bool found = false;
        const uint64_t now = datlink_now_ms();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < RX_WINDOW; ++i) {
            rx_slot_t *slot = &s_rx_slots[i];
            if (slot->state == RX_SLOT_RECEIVED &&
                slot->sequence == s_rx_base + 1U) {
                frame = slot->frame;
                token.session_id = frame.session_id;
                token.sequence = frame.sequence;
                token.type = frame.type;
                token.generation = slot->generation;
                slot->state = RX_SLOT_IN_PROGRESS;
                slot->state_since_ms = now;
                index = i;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!found) return;

        const datlink_rx_disposition_t disposition =
            s_handler != NULL ? s_handler(&frame, &token, s_handler_context)
                              : DATLINK_RX_DEFER;
        if (disposition == DATLINK_RX_ASYNC) return;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        rx_slot_t *slot = &s_rx_slots[index];
        if (slot->state == RX_SLOT_IN_PROGRESS &&
            slot->sequence == token.sequence && slot->generation == token.generation) {
            slot->state = RX_SLOT_RECEIVED;
            slot->state_since_ms = now;
            ++s_stats.application_defers;
            if (now - slot->first_seen_ms >= DEFER_TIMEOUT_MS) {
                slot->state = RX_SLOT_ERROR_PENDING;
                slot->error_status = DATLINK_ERR_STATE;
                slot->error_detail = ESP_ERR_TIMEOUT;
                ESP_LOGW(TAG, "application defer timeout seq=%" PRIu32, token.sequence);
            }
        }
        xSemaphoreGive(s_lock);
        return;
    }
}

static void service_rx(void)
{
    const uint64_t now = datlink_now_ms();
    uint32_t before_base;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    before_base = s_rx_base;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        rx_slot_t *slot = &s_rx_slots[i];
        if (slot->state == RX_SLOT_IN_PROGRESS &&
            now - slot->state_since_ms >= message_timeout_ms(slot->frame.type)) {
            slot->state = RX_SLOT_ERROR_PENDING;
            slot->error_status = DATLINK_ERR_TIMEOUT;
            slot->error_detail = slot->frame.type;
            ESP_LOGE(TAG, "worker timeout seq=%" PRIu32 " type=%u",
                     slot->sequence, slot->frame.type);
        }
    }
    xSemaphoreGive(s_lock);

    while (queue_head_error()) advance_committed();
    advance_committed();
    deliver_ordered();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool advanced = s_rx_base != before_base;
    xSemaphoreGive(s_lock);
    if (advanced) (void)queue_control(DATLINK_MSG_ACK, NULL, 0);
}

static void rotate_local_epoch(bool notify)
{
    xQueueReset(s_tx_queue);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_tx_slots, 0, sizeof(s_tx_slots));
    s_session_id = new_session_id();
    s_next_tx_seq = 1U;
    ++s_stats.recovery_count;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "new local session epoch=%" PRIu32, s_session_id);
    if (notify && s_event_handler != NULL) {
        s_event_handler(DATLINK_TRANSPORT_EVENT_LOCAL_EPOCH_RESET, s_event_context);
    }
}

static void send_resync_request(void)
{
    uint8_t payload[DATLINK_RESYNC_WIRE_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    put_u32(payload + 0U, s_resync_nonce);
    put_u32(payload + 4U, s_peer_session);
    put_u32(payload + 8U, s_resync_expected);
    put_u32(payload + 12U, s_resync_reason);
    s_resync_last_ms = datlink_now_ms();
    if (s_resync_retries < UINT8_MAX) ++s_resync_retries;
    xSemaphoreGive(s_lock);
    (void)queue_control(DATLINK_MSG_RESYNC_REQUEST, payload, sizeof(payload));
}

static void begin_resync(uint32_t reason, uint32_t expected)
{
    bool start = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_recovering) {
        s_recovering = true;
        s_resync_nonce = esp_random();
        if (s_resync_nonce == 0U) s_resync_nonce = 1U;
        s_resync_reason = reason;
        s_resync_expected = expected;
        s_resync_retries = 0U;
        start = true;
    }
    xSemaphoreGive(s_lock);
    if (start) {
        ESP_LOGW(TAG, "ordered-head stall; resync expected=%" PRIu32 " reason=%" PRIu32,
                 expected, reason);
        send_resync_request();
    }
}

static void handle_resync_request(const datlink_wire_frame_t *frame)
{
    if (frame->payload_length != DATLINK_RESYNC_WIRE_LEN) return;
    const uint32_t nonce = get_u32(frame->payload + 0U);
    const uint32_t target = get_u32(frame->payload + 4U);
    const uint32_t expected = get_u32(frame->payload + 8U);
    const uint32_t reason = get_u32(frame->payload + 12U);
    if (target != s_session_id) return;

    bool retained = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < TX_WINDOW; ++i) {
        if (s_tx_slots[i].used && s_tx_slots[i].sequence == expected) {
            s_tx_slots[i].sent = false;
            s_tx_slots[i].retries = 0U;
            retained = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    uint32_t action = DATLINK_RESYNC_RETRANSMIT;
    if (!retained || reason == DATLINK_RESYNC_REASON_MANUAL) {
        if (reason == DATLINK_RESYNC_REASON_MANUAL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            reset_rx_session_locked(0U);
            s_link_up = false;
            xSemaphoreGive(s_lock);
        }
        rotate_local_epoch(true);
        action = DATLINK_RESYNC_NEW_EPOCH;
    } else {
        ESP_LOGW(TAG, "resync retransmit seq=%" PRIu32, expected);
    }

    uint8_t reply[DATLINK_RESYNC_WIRE_LEN];
    put_u32(reply + 0U, nonce);
    put_u32(reply + 4U, action);
    put_u32(reply + 8U, s_session_id);
    put_u32(reply + 12U, s_next_tx_seq);
    (void)queue_control(DATLINK_MSG_RESYNC_REPLY, reply, sizeof(reply));
}

static void handle_resync_reply(const datlink_wire_frame_t *frame)
{
    if (frame->payload_length != DATLINK_RESYNC_WIRE_LEN) return;
    const uint32_t nonce = get_u32(frame->payload + 0U);
    const uint32_t action = get_u32(frame->payload + 4U);
    bool manual = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_recovering || nonce != s_resync_nonce) {
        xSemaphoreGive(s_lock);
        return;
    }
    manual = s_resync_reason == DATLINK_RESYNC_REASON_MANUAL;
    s_recovering = false;
    s_resync_retries = 0U;
    ++s_stats.recovery_count;
    xSemaphoreGive(s_lock);
    if (action == DATLINK_RESYNC_NEW_EPOCH && manual) rotate_local_epoch(true);
    ESP_LOGI(TAG, "resync complete action=%" PRIu32, action);
}

static void recovery_tick(void)
{
    const uint64_t now = datlink_now_ms();
    bool retry = false;
    bool exhausted = false;
    bool has_later = false;
    bool has_expected = false;
    uint32_t expected;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    expected = s_rx_base + 1U;
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].state == RX_SLOT_EMPTY) continue;
        if (s_rx_slots[i].sequence == expected) has_expected = true;
        if (s_rx_slots[i].sequence > expected) has_later = true;
    }
    if (s_recovering && now - s_resync_last_ms >= RESYNC_RETRY_MS) {
        if (s_resync_retries < RESYNC_MAX_RETRIES) retry = true;
        else {
            exhausted = true;
            s_recovering = false;
            s_link_up = false;
            s_last_error = DATLINK_ERR_LINK;
        }
    }
    /* A later frame is a gap only when the expected sequence is genuinely
     * absent. The expected slot may legitimately remain IN_PROGRESS while a
     * storage or SWD worker commits it; treating that as loss rotates the
     * sender epoch in the middle of an otherwise healthy image pipeline. */
    const bool start = !s_recovering && !has_expected && has_later &&
                       now - s_rx_progress_ms >= GAP_TIMEOUT_MS;
    xSemaphoreGive(s_lock);
    if (retry) send_resync_request();
    if (exhausted) ESP_LOGE(TAG, "resync timed out; waiting for a fresh HELLO");
    if (start) begin_resync(DATLINK_RESYNC_REASON_GAP, expected);
}

static void rx_task(void *argument)
{
    (void)argument;
    s_rx_task = xTaskGetCurrentTaskHandle();
    raw_rx_t raw;
    for (;;) {
        const bool got_raw = xQueueReceive(s_raw_rx_queue, &raw,
                                            pdMS_TO_TICKS(DEFER_RETRY_MS)) == pdTRUE;
        if (got_raw) {
            datlink_wire_frame_t frame;
            esp_err_t err = datlink_wire_decode(raw.data, raw.length, &frame);
            if (err != ESP_OK) {
                ++s_stats.rx_crc_errors;
                if (raw.length >= 3U && raw.data[0] == (uint8_t)DATLINK_WIRE_MAGIC &&
                    raw.data[1] == (uint8_t)(DATLINK_WIRE_MAGIC >> 8) &&
                    raw.data[2] != DATLINK_PROTOCOL_VERSION) {
                    s_last_error = DATLINK_ERR_VERSION;
                    ESP_LOGE(TAG, "protocol version mismatch: peer=%u local=%u",
                             raw.data[2], DATLINK_PROTOCOL_VERSION);
                }
            } else {
                ++s_stats.rx_frames;
                if (!accept_session(&frame)) {
                    ++s_stats.dropped_frames;
                } else {
                    process_ack(frame.ack_base, frame.ack_bitmap);
                    if (frame.type == DATLINK_MSG_RESYNC_REQUEST) {
                        handle_resync_request(&frame);
                    } else if (frame.type == DATLINK_MSG_RESYNC_REPLY) {
                        handle_resync_reply(&frame);
                    } else if (frame.type == DATLINK_MSG_ACK ||
                               frame.type == DATLINK_MSG_HEARTBEAT ||
                               frame.type == DATLINK_MSG_HELLO || frame.sequence == 0U) {
                        if (frame.type != DATLINK_MSG_ACK) {
                            (void)queue_control(DATLINK_MSG_ACK, NULL, 0);
                        }
                    } else {
                        bool out_of_window = false;
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        if (frame.sequence <= s_rx_base) {
                            ++s_stats.rx_duplicates;
                        } else if (frame.sequence > s_rx_base + RX_WINDOW) {
                            ++s_stats.dropped_frames;
                            out_of_window = true;
                        } else {
                            rx_slot_t *slot = find_rx_slot_locked(frame.sequence);
                            if (slot == NULL) {
                                ++s_stats.dropped_frames;
                            } else if (slot->state != RX_SLOT_EMPTY) {
                                ++s_stats.rx_duplicates;
                            } else {
                                slot->state = RX_SLOT_RECEIVED;
                                slot->sequence = frame.sequence;
                                slot->generation = s_rx_generation;
                                slot->first_seen_ms = datlink_now_ms();
                                slot->state_since_ms = slot->first_seen_ms;
                                slot->frame = frame;
                            }
                        }
                        const uint32_t expected = s_rx_base + 1U;
                        xSemaphoreGive(s_lock);
                        if (out_of_window) begin_resync(DATLINK_RESYNC_REASON_GAP, expected);
                        (void)queue_control(DATLINK_MSG_ACK, NULL, 0);
                    }
                }
            }
        }
        service_rx();
        recovery_tick();
    }
}

static esp_err_t physical_send(datlink_wire_frame_t *frame)
{
    uint8_t encoded[DATLINK_WIRE_FRAME_MAX];
    fill_ack_fields(frame);
    const size_t length = datlink_wire_encode(frame, encoded, sizeof(encoded));
    if (length == 0U) return ESP_ERR_INVALID_SIZE;
    while (ulTaskNotifyTake(pdTRUE, 0) != 0U) {}
    esp_err_t err = esp_now_send(datlink_security_peer_mac(), encoded, length);
    if (err != ESP_OK) return err;
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0U) return ESP_ERR_TIMEOUT;
    ++s_stats.tx_frames;
    s_last_tx_ms = datlink_now_ms();
    return ESP_OK;
}

static bool allocate_slot(const tx_item_t *item)
{
    bool allocated = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < TX_WINDOW; ++i) {
        if (s_tx_slots[i].used) continue;
        tx_slot_t *slot = &s_tx_slots[i];
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->sequence = s_next_tx_seq++;
        slot->frame.type = item->type;
        slot->frame.flags = item->flags;
        slot->frame.session_id = s_session_id;
        slot->frame.sequence = slot->sequence;
        slot->frame.payload_length = item->length;
        memcpy(slot->frame.payload, item->payload, item->length);
        allocated = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return allocated;
}

static unsigned active_slots(void)
{
    unsigned count = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < TX_WINDOW; ++i) count += s_tx_slots[i].used ? 1U : 0U;
    xSemaphoreGive(s_lock);
    return count;
}

static void tx_task(void *argument)
{
    (void)argument;
    s_tx_task = xTaskGetCurrentTaskHandle();
    uint64_t last_heartbeat = 0U;
    for (;;) {
        tx_item_t item;
        if (xQueueReceive(s_ack_queue, &item, 0) == pdTRUE) {
            datlink_wire_frame_t frame = {
                .type = item.type,
                .session_id = s_session_id,
                .sequence = 0U,
                .payload_length = item.length,
            };
            memcpy(frame.payload, item.payload, item.length);
            (void)physical_send(&frame);
            continue;
        }
        while (active_slots() < TX_WINDOW &&
               xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
            if (!allocate_slot(&item)) {
                (void)xQueueSendToFront(s_tx_queue, &item, 0);
                break;
            }
        }

        const uint64_t now = datlink_now_ms();
        bool did_send = false;
        for (unsigned i = 0; i < TX_WINDOW; ++i) {
            datlink_wire_frame_t frame;
            bool should_send = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            tx_slot_t *slot = &s_tx_slots[i];
            const uint64_t interval = slot->retries >= MAX_RETRIES
                                          ? RECOVERY_RTO_MS : RTO_MS;
            if (slot->used && (!slot->sent || now - slot->last_sent_ms >= interval)) {
                if (slot->retries == MAX_RETRIES) {
                    ESP_LOGE(TAG, "radio retry recovery seq=%" PRIu32, slot->sequence);
                    s_link_up = false;
                }
                if (slot->retries < UINT8_MAX) ++slot->retries;
                frame = slot->frame;
                should_send = true;
                if (slot->sent) ++s_stats.tx_retries;
                slot->sent = true;
                slot->last_sent_ms = now;
            }
            xSemaphoreGive(s_lock);
            if (should_send) {
                (void)physical_send(&frame);
                did_send = true;
                break;
            }
        }
        if (!did_send && now - last_heartbeat >= HEARTBEAT_MS) {
            datlink_wire_frame_t hello = {
                .type = DATLINK_MSG_HELLO,
                .session_id = s_session_id,
            };
            (void)physical_send(&hello);
            last_heartbeat = now;
        }
        bool down = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_link_up && now - s_last_rx_ms > LINK_TIMEOUT_MS) {
            s_link_up = false;
            down = true;
        }
        xSemaphoreGive(s_lock);
        if (down) ESP_LOGW(TAG, "link timeout");
        datlink_diagnostics_set_activity(active_slots() != 0U);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    (void)status;
    if (s_tx_task != NULL) xTaskNotifyGive(s_tx_task);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (info == NULL || data == NULL || length <= 0 ||
        length > (int)DATLINK_WIRE_FRAME_MAX ||
        memcmp(info->src_addr, datlink_security_peer_mac(), 6) != 0) return;
    raw_rx_t raw = {.length = (uint16_t)length};
    memcpy(raw.mac, info->src_addr, 6);
    memcpy(raw.data, data, length);
    if (xQueueSend(s_raw_rx_queue, &raw, 0) != pdTRUE) ++s_stats.dropped_frames;
}

static esp_err_t wifi_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(CONFIG_DATLINK_WIFI_CHANNEL,
                                             WIFI_SECOND_CHAN_NONE),
                        TAG, "wifi channel");
    return esp_wifi_set_ps(WIFI_PS_NONE);
}

esp_err_t datlink_transport_init(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(tx_item_t));
    s_ack_queue = xQueueCreate(ACK_QUEUE_LENGTH, sizeof(tx_item_t));
    s_raw_rx_queue = xQueueCreate(RAW_RX_QUEUE_LENGTH, sizeof(raw_rx_t));
    s_lock = xSemaphoreCreateMutex();
    if (s_tx_queue == NULL || s_ack_queue == NULL || s_raw_rx_queue == NULL ||
        s_lock == NULL) return ESP_ERR_NO_MEM;

    uint32_t boot_id = 0U;
    nvs_handle_t nvs;
    if (nvs_open("datlink", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_get_u32(nvs, "boot_id", &boot_id);
        if (++boot_id == 0U) boot_id = 1U;
        if (nvs_set_u32(nvs, "boot_id", boot_id) == ESP_OK) (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_session_id = esp_random() ^ (boot_id * 0x9E3779B9U);
    if (s_session_id == 0U) s_session_id = 1U;
    s_rx_progress_ms = datlink_now_ms();

    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "wifi setup");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(send_cb), TAG, "send callback");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(recv_cb), TAG, "receive callback");
    ESP_RETURN_ON_ERROR(esp_now_set_pmk(datlink_security_pmk()), TAG, "set PMK");
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, datlink_security_peer_mac(), 6);
    memcpy(peer.lmk, datlink_security_lmk(), ESP_NOW_KEY_LEN);
    peer.channel = CONFIG_DATLINK_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add encrypted peer");
    if (xTaskCreatePinnedToCore(rx_task, "datlink_rx", 7168, NULL, 8, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(tx_task, "datlink_tx", 6144, NULL, 8, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "protocol=v%u session=%" PRIu32 " channel=%d encrypted peer=" MACSTR,
             DATLINK_PROTOCOL_VERSION, s_session_id, CONFIG_DATLINK_WIFI_CHANNEL,
             MAC2STR(datlink_security_peer_mac()));
    return ESP_OK;
}

void datlink_transport_set_handler(datlink_transport_handler_t handler, void *context)
{
    s_handler = handler;
    s_handler_context = context;
}

void datlink_transport_set_event_handler(datlink_transport_event_handler_t handler,
                                         void *context)
{
    s_event_handler = handler;
    s_event_context = context;
}

esp_err_t datlink_transport_complete_rx(const datlink_rx_token_t *token,
                                        datlink_status_t status, uint32_t detail)
{
    if (token == NULL || token->sequence == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        rx_slot_t *slot = &s_rx_slots[i];
        if (slot->state == RX_SLOT_IN_PROGRESS &&
            slot->sequence == token->sequence &&
            slot->generation == token->generation &&
            slot->frame.session_id == token->session_id &&
            slot->frame.type == token->type) {
            slot->state = status == DATLINK_OK ? RX_SLOT_COMMITTED
                                               : RX_SLOT_ERROR_PENDING;
            slot->error_status = status;
            slot->error_detail = detail;
            slot->state_since_ms = datlink_now_ms();
            result = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (result == ESP_OK && s_rx_task != NULL) xTaskNotifyGive(s_rx_task);
    return result;
}

esp_err_t datlink_transport_send(uint8_t type, uint16_t flags,
                                 const void *payload, size_t length,
                                 TickType_t timeout)
{
    return queue_reliable(type, flags, payload, length, timeout);
}

esp_err_t datlink_transport_flush(TickType_t timeout)
{
    const TickType_t start = xTaskGetTickCount();
    while (uxQueueMessagesWaiting(s_tx_queue) != 0U || active_slots() != 0U) {
        if (xTaskGetTickCount() - start >= timeout) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

bool datlink_transport_link_up(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool up = s_link_up;
    xSemaphoreGive(s_lock);
    return up;
}

uint32_t datlink_transport_session_id(void) { return s_session_id; }

void datlink_transport_get_stats(datlink_transport_stats_t *stats)
{
    if (stats != NULL) *stats = s_stats;
}

void datlink_transport_get_status(datlink_transport_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    const uint64_t now = datlink_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->up = s_link_up;
    status->recovering = s_recovering;
    status->local_session = s_session_id;
    status->peer_session = s_peer_session;
    status->next_tx_sequence = s_next_tx_seq;
    status->rx_base = s_rx_base;
    status->last_error = s_last_error;
    status->recovery_count = s_stats.recovery_count;
    for (unsigned i = 0; i < TX_WINDOW; ++i) status->tx_pending += s_tx_slots[i].used;
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        const rx_slot_t *slot = &s_rx_slots[i];
        if (slot->state != RX_SLOT_EMPTY) ++status->rx_pending;
        if (slot->sequence == s_rx_base + 1U) {
            status->head_state = (uint8_t)slot->state;
            status->head_age_ms = (uint32_t)(now - slot->first_seen_ms);
        }
    }
    xSemaphoreGive(s_lock);
    status->tx_pending += (uint16_t)uxQueueMessagesWaiting(s_tx_queue);
}

esp_err_t datlink_transport_recover(void)
{
    begin_resync(DATLINK_RESYNC_REASON_MANUAL, s_rx_base + 1U);
    return ESP_OK;
}

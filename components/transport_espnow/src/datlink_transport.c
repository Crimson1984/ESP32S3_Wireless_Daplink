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
#define HEARTBEAT_MS 500U
#define LINK_TIMEOUT_MS 3000U
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

typedef struct {
    bool used;
    uint32_t sequence;
    datlink_wire_frame_t frame;
} rx_slot_t;

static const char *TAG = "espnow_transport";
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_ack_queue;
static QueueHandle_t s_raw_rx_queue;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_tx_task;
static tx_slot_t s_tx_slots[TX_WINDOW];
static rx_slot_t s_rx_slots[RX_WINDOW];
static uint32_t s_session_id;
static uint32_t s_peer_session;
static uint32_t s_next_tx_seq = 1;
static uint32_t s_rx_base;
static uint64_t s_last_rx_ms;
static uint64_t s_last_tx_ms;
static bool s_link_up;
static datlink_transport_handler_t s_handler;
static void *s_handler_context;
static datlink_transport_stats_t s_stats;

static uint32_t rx_bitmap_locked(void)
{
    uint32_t bitmap = 0;
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].used && s_rx_slots[i].sequence > s_rx_base &&
            s_rx_slots[i].sequence <= s_rx_base + 32U) {
            bitmap |= 1UL << (s_rx_slots[i].sequence - s_rx_base - 1U);
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

static esp_err_t queue_unreliable(uint8_t type)
{
    const tx_item_t item = {.reliable = false, .type = type};
    return xQueueSend(s_ack_queue, &item, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
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
    s_rx_base = 0;
    s_peer_session = peer_session;
}

static bool accept_session(const datlink_wire_frame_t *frame)
{
    bool accepted = false;
    bool became_up = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_peer_session == frame->session_id) {
        accepted = true;
    } else if (frame->type == DATLINK_MSG_HELLO &&
               (s_peer_session == 0U || !s_link_up)) {
        reset_rx_session_locked(frame->session_id);
        accepted = true;
    }
    if (accepted) {
        became_up = !s_link_up;
        s_last_rx_ms = datlink_now_ms();
        s_link_up = true;
    }
    xSemaphoreGive(s_lock);
    if (became_up) {
        ESP_LOGI(TAG, "link up, peer session=%" PRIu32, frame->session_id);
    }
    return accepted;
}

static bool link_is_up(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool up = s_link_up;
    xSemaphoreGive(s_lock);
    return up;
}

static rx_slot_t *find_rx_slot_locked(uint32_t sequence)
{
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (s_rx_slots[i].used && s_rx_slots[i].sequence == sequence) {
            return &s_rx_slots[i];
        }
    }
    for (unsigned i = 0; i < RX_WINDOW; ++i) {
        if (!s_rx_slots[i].used) return &s_rx_slots[i];
    }
    return NULL;
}

static void deliver_ordered(void)
{
    for (;;) {
        datlink_wire_frame_t frame;
        bool found = false;
        unsigned slot_index = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < RX_WINDOW; ++i) {
            if (s_rx_slots[i].used && s_rx_slots[i].sequence == s_rx_base + 1U) {
                frame = s_rx_slots[i].frame;
                slot_index = i;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!found) break;

        const esp_err_t result = s_handler != NULL
                                     ? s_handler(&frame, s_handler_context)
                                     : ESP_ERR_INVALID_STATE;
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "application rejected seq=%" PRIu32 ": %s",
                     frame.sequence, esp_err_to_name(result));
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_rx_slots[slot_index].used &&
            s_rx_slots[slot_index].sequence == frame.sequence) {
            memset(&s_rx_slots[slot_index], 0, sizeof(s_rx_slots[slot_index]));
            s_rx_base = frame.sequence;
        }
        xSemaphoreGive(s_lock);
    }
}

static void rx_task(void *argument)
{
    (void)argument;
    raw_rx_t raw;
    for (;;) {
        if (xQueueReceive(s_raw_rx_queue, &raw, portMAX_DELAY) != pdTRUE) continue;
        datlink_wire_frame_t frame;
        esp_err_t err = datlink_wire_decode(raw.data, raw.length, &frame);
        if (err != ESP_OK) {
            ++s_stats.rx_crc_errors;
            continue;
        }
        ++s_stats.rx_frames;
        if (!accept_session(&frame)) {
            ++s_stats.dropped_frames;
            continue;
        }
        process_ack(frame.ack_base, frame.ack_bitmap);

        if (frame.type == DATLINK_MSG_ACK || frame.type == DATLINK_MSG_HEARTBEAT ||
            frame.type == DATLINK_MSG_HELLO || frame.sequence == 0U) {
            if (frame.type != DATLINK_MSG_ACK) (void)queue_unreliable(DATLINK_MSG_ACK);
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (frame.sequence <= s_rx_base) {
            ++s_stats.rx_duplicates;
            xSemaphoreGive(s_lock);
            (void)queue_unreliable(DATLINK_MSG_ACK);
            continue;
        }
        if (frame.sequence > s_rx_base + RX_WINDOW) {
            ++s_stats.dropped_frames;
            xSemaphoreGive(s_lock);
            (void)queue_unreliable(DATLINK_MSG_ACK);
            continue;
        }
        rx_slot_t *slot = find_rx_slot_locked(frame.sequence);
        if (slot == NULL) {
            ++s_stats.dropped_frames;
            xSemaphoreGive(s_lock);
            continue;
        }
        if (slot->used) {
            ++s_stats.rx_duplicates;
        } else {
            slot->used = true;
            slot->sequence = frame.sequence;
            slot->frame = frame;
        }
        xSemaphoreGive(s_lock);
        deliver_ordered();
        (void)queue_unreliable(DATLINK_MSG_ACK);
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
    unsigned count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < TX_WINDOW; ++i) count += s_tx_slots[i].used ? 1U : 0U;
    xSemaphoreGive(s_lock);
    return count;
}

static void tx_task(void *argument)
{
    (void)argument;
    s_tx_task = xTaskGetCurrentTaskHandle();
    uint64_t last_heartbeat = 0;
    for (;;) {
        tx_item_t item;
        if (xQueueReceive(s_ack_queue, &item, 0) == pdTRUE) {
            datlink_wire_frame_t frame = {
                .type = item.type,
                .session_id = s_session_id,
                .sequence = 0,
            };
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
            if (slot->used && (!slot->sent || now - slot->last_sent_ms >= RTO_MS)) {
                if (slot->retries >= MAX_RETRIES) {
                    ESP_LOGE(TAG, "sequence %" PRIu32 " exceeded retries", slot->sequence);
                    memset(slot, 0, sizeof(*slot));
                    s_link_up = false;
                } else {
                    frame = slot->frame;
                    should_send = true;
                    if (slot->sent) ++s_stats.tx_retries;
                    slot->sent = true;
                    slot->last_sent_ms = now;
                    ++slot->retries;
                }
            }
            xSemaphoreGive(s_lock);
            if (should_send) {
                (void)physical_send(&frame);
                did_send = true;
                break;
            }
        }

        if (!did_send && now - last_heartbeat >= HEARTBEAT_MS) {
            datlink_wire_frame_t heartbeat = {
                /* HELLO is idempotent for the established session and also
                 * heals an asymmetric startup where the first HELLO was lost. */
                .type = DATLINK_MSG_HELLO,
                .session_id = s_session_id,
            };
            (void)physical_send(&heartbeat);
            last_heartbeat = now;
        }
        bool became_down = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_link_up && now - s_last_rx_ms > LINK_TIMEOUT_MS) {
            s_link_up = false;
            became_down = true;
        }
        xSemaphoreGive(s_lock);
        if (became_down) ESP_LOGW(TAG, "link timeout");
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
        memcmp(info->src_addr, datlink_security_peer_mac(), 6) != 0) {
        return;
    }
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
    uint32_t boot_id = 0;
    nvs_handle_t nvs;
    if (nvs_open("datlink", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_get_u32(nvs, "boot_id", &boot_id);
        ++boot_id;
        if (boot_id == 0U) boot_id = 1U;
        if (nvs_set_u32(nvs, "boot_id", boot_id) == ESP_OK) (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_session_id = esp_random() ^ (boot_id * 0x9E3779B9U);
    if (s_session_id == 0U) s_session_id = 1U;

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

    if (xTaskCreatePinnedToCore(rx_task, "datlink_rx", 6144, NULL, 8, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(tx_task, "datlink_tx", 6144, NULL, 8, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "session=%" PRIu32 " channel=%d encrypted peer=" MACSTR,
             s_session_id, CONFIG_DATLINK_WIFI_CHANNEL,
             MAC2STR(datlink_security_peer_mac()));
    return ESP_OK;
}

void datlink_transport_set_handler(datlink_transport_handler_t handler, void *context)
{
    s_handler = handler;
    s_handler_context = context;
}

esp_err_t datlink_transport_send(uint8_t type, uint16_t flags,
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
    memcpy(item.payload, payload, length);
    return xQueueSend(s_tx_queue, &item, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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

bool datlink_transport_link_up(void) { return link_is_up(); }
uint32_t datlink_transport_session_id(void) { return s_session_id; }

void datlink_transport_get_stats(datlink_transport_stats_t *stats)
{
    if (stats != NULL) *stats = s_stats;
}

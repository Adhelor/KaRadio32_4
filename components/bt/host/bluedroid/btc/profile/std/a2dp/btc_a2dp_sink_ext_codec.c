#include "common/bt_target.h"
#include <string.h>
#include "common/bt_trace.h"
#include "osi/allocator.h"
#include "osi/fixed_queue.h"
#include "osi/thread.h"
#include "btc/btc_manage.h"
#include "esp_a2dp_api.h"

#if (BTC_AV_SINK_INCLUDED == TRUE) && defined(CONFIG_BT_A2DP_AAC_DECODER) && CONFIG_BT_A2DP_AAC_DECODER

#define EXT_CODEC_QUEUE_SIZE 14
#define EXT_CODEC_QUEUE_IDX 1

static fixed_queue_t *s_packet_queue;
static struct osi_event *s_data_event;
static osi_thread_t *s_btc_thread;
static esp_a2d_sink_data_cb_t s_data_callback;
static bool s_active;
static bool s_rx_flush = true;

static void btc_a2dp_sink_ext_codec_flush(void)
{
    if (s_packet_queue == NULL)
    {
        return;
    }
    while (!fixed_queue_is_empty(s_packet_queue))
    {
        osi_free(fixed_queue_dequeue(s_packet_queue, 0));
    }
}

static void btc_a2dp_sink_ext_codec_process(BT_HDR *packet)
{
    uint8_t *payload = (uint8_t *)(packet + 1) + packet->offset;
    size_t payload_length = packet->len;

    if (s_rx_flush || s_data_callback == NULL || payload_length == 0)
    {
        osi_free(packet);
        return;
    }

    uint8_t *framed_packet = osi_malloc(payload_length + sizeof(uint16_t));
    if (framed_packet == NULL)
    {
        osi_free(packet);
        return;
    }
    framed_packet[0] = (uint8_t)(payload_length >> 8);
    framed_packet[1] = (uint8_t)payload_length;
    memcpy(framed_packet + sizeof(uint16_t), payload, payload_length);
    s_data_callback(framed_packet, payload_length + sizeof(uint16_t));
    osi_free(framed_packet);
    osi_free(packet);
}

static void btc_a2dp_sink_ext_codec_data_ready(void *context)
{
    (void)context;
    if (!s_active)
    {
        return;
    }
    if (s_rx_flush)
    {
        btc_a2dp_sink_ext_codec_flush();
        return;
    }

    int count = fixed_queue_length(s_packet_queue);
    while (count-- > 0)
    {
        BT_HDR *packet = fixed_queue_dequeue(s_packet_queue, 0);
        if (packet == NULL)
        {
            break;
        }
        btc_a2dp_sink_ext_codec_process(packet);
    }

    if (!fixed_queue_is_empty(s_packet_queue))
    {
        osi_thread_post_event(s_data_event, OSI_THREAD_MAX_TIMEOUT);
    }
}

bool btc_a2dp_sink_ext_codec_startup(void)
{
    if (s_active)
    {
        return true;
    }

    s_btc_thread = btc_get_current_thread();
    s_packet_queue = fixed_queue_new(EXT_CODEC_QUEUE_SIZE);
    s_data_event = osi_event_create(btc_a2dp_sink_ext_codec_data_ready, NULL);
    if (s_packet_queue == NULL || s_data_event == NULL)
    {
        if (s_data_event != NULL)
        {
            osi_event_delete(s_data_event);
            s_data_event = NULL;
        }
        if (s_packet_queue != NULL)
        {
            fixed_queue_free(s_packet_queue, osi_free_func);
            s_packet_queue = NULL;
        }
        s_btc_thread = NULL;
        return false;
    }

    osi_event_bind(s_data_event, s_btc_thread, EXT_CODEC_QUEUE_IDX);
    s_rx_flush = true;
    s_active = true;
    return true;
}

void btc_a2dp_sink_ext_codec_shutdown(void)
{
    if (!s_active)
    {
        return;
    }
    s_active = false;
    btc_a2dp_sink_ext_codec_flush();
    fixed_queue_free(s_packet_queue, osi_free_func);
    osi_event_delete(s_data_event);
    s_packet_queue = NULL;
    s_data_event = NULL;
    s_btc_thread = NULL;
}

void btc_a2dp_sink_ext_codec_set_data_callback(esp_a2d_sink_data_cb_t callback)
{
    s_data_callback = callback;
}

void btc_a2dp_sink_ext_codec_set_rx_flush(bool flush)
{
    s_rx_flush = flush;
    if (flush)
    {
        btc_a2dp_sink_ext_codec_flush();
    }
}

void btc_a2dp_sink_ext_codec_enqueue(BT_HDR *packet)
{
    if (packet == NULL)
    {
        return;
    }
    if (!s_active || s_rx_flush || s_packet_queue == NULL)
    {
        return;
    }

    size_t packet_size = sizeof(BT_HDR) + packet->offset + packet->len;
    BT_HDR *copy = osi_malloc(packet_size);
    if (copy == NULL)
    {
        return;
    }
    memcpy(copy, packet, packet_size);

    if (fixed_queue_length(s_packet_queue) >= EXT_CODEC_QUEUE_SIZE)
    {
        osi_free(fixed_queue_dequeue(s_packet_queue, 0));
    }
    fixed_queue_enqueue(s_packet_queue, copy, FIXED_QUEUE_MAX_TIMEOUT);
    osi_thread_post_event(s_data_event, OSI_THREAD_MAX_TIMEOUT);
}

#else

bool btc_a2dp_sink_ext_codec_startup(void) { return true; }
void btc_a2dp_sink_ext_codec_shutdown(void) {}
void btc_a2dp_sink_ext_codec_set_data_callback(esp_a2d_sink_data_cb_t callback) { (void)callback; }
void btc_a2dp_sink_ext_codec_set_rx_flush(bool flush) { (void)flush; }
void btc_a2dp_sink_ext_codec_enqueue(BT_HDR *packet) { osi_free(packet); }

#endif
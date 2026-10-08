#include "common/bt_target.h"
#include <string.h>
#include "common/bt_trace.h"
#include "osi/allocator.h"
#include "osi/fixed_queue.h"
#include "osi/thread.h"
#include "btc/btc_manage.h"
#include "esp_a2dp_api.h"
#include "esp_timer.h"

#if (BTC_AV_SINK_INCLUDED == TRUE) && \
    ((defined(CONFIG_BT_A2DP_AAC_DECODER) && CONFIG_BT_A2DP_AAC_DECODER) || \
    (defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER) || \
     (defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER))

#define EXT_CODEC_QUEUE_SIZE 96
#define EXT_CODEC_QUEUE_IDX 1

static fixed_queue_t *s_packet_queue;
static struct osi_event *s_data_event;
static osi_thread_t *s_btc_thread;
static esp_a2d_sink_data_cb_t s_data_callback;
static bool s_active;
static bool s_rx_flush = true;
static int64_t s_stats_start_us;
static uint32_t s_rx_packets;
static uint32_t s_rx_bytes;
static uint32_t s_delivered_packets;
static uint32_t s_delivered_bytes;
static uint32_t s_sequence_gaps;
static uint32_t s_overflow_drops;
static uint32_t s_flush_drops;
static uint32_t s_alloc_failures;
static uint32_t s_max_queue_depth;
static uint16_t s_last_sequence;
static bool s_have_last_sequence;

static void btc_a2dp_sink_ext_codec_log_stats(int64_t now_us)
{
    if (s_stats_start_us == 0)
    {
        s_stats_start_us = now_us;
    }
    if (now_us - s_stats_start_us < 5000000)
    {
        return;
    }

    APPL_TRACE_EVENT("A2DP codec 5s: rx=%u/%uB delivered=%u/%uB seq_gaps=%u overflow=%u rx_flush=%u alloc_fail=%u max_q=%u",
                     (unsigned)s_rx_packets, (unsigned)s_rx_bytes,
                     (unsigned)s_delivered_packets, (unsigned)s_delivered_bytes,
                     (unsigned)s_sequence_gaps, (unsigned)s_overflow_drops,
                     (unsigned)s_flush_drops, (unsigned)s_alloc_failures,
                     (unsigned)s_max_queue_depth);
    s_rx_packets = 0;
    s_rx_bytes = 0;
    s_delivered_packets = 0;
    s_delivered_bytes = 0;
    s_sequence_gaps = 0;
    s_overflow_drops = 0;
    s_flush_drops = 0;
    s_alloc_failures = 0;
    s_max_queue_depth = 0;
    s_stats_start_us = now_us;
}

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
    s_delivered_packets++;
    s_delivered_bytes += payload_length;
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
        if (s_packet_queue != NULL)
        {
            s_flush_drops += (uint32_t)fixed_queue_length(s_packet_queue);
        }
        btc_a2dp_sink_ext_codec_flush();
    }
    s_have_last_sequence = false;
}

void btc_a2dp_sink_ext_codec_enqueue(BT_HDR *packet)
{
    if (packet == NULL)
    {
        return;
    }
    if (!s_active || s_packet_queue == NULL)
    {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    s_rx_packets++;
    s_rx_bytes += packet->len;
    if (s_rx_flush)
    {
        s_flush_drops++;
        btc_a2dp_sink_ext_codec_log_stats(now_us);
        return;
    }

    uint16_t sequence = (uint16_t)packet->layer_specific;
    if (s_have_last_sequence)
    {
        uint16_t delta = (uint16_t)(sequence - s_last_sequence);
        if (delta > 1 && delta < 0x8000)
        {
            s_sequence_gaps += delta - 1;
        }
    }
    s_last_sequence = sequence;
    s_have_last_sequence = true;

    size_t packet_size = sizeof(BT_HDR) + packet->offset + packet->len;
    BT_HDR *copy = osi_malloc(packet_size);
    if (copy == NULL)
    {
        s_alloc_failures++;
        btc_a2dp_sink_ext_codec_log_stats(now_us);
        return;
    }
    memcpy(copy, packet, packet_size);

    if (fixed_queue_length(s_packet_queue) >= EXT_CODEC_QUEUE_SIZE)
    {
        osi_free(fixed_queue_dequeue(s_packet_queue, 0));
        s_overflow_drops++;
        if ((s_overflow_drops & (s_overflow_drops - 1)) == 0)
        {
            APPL_TRACE_EVENT("A2DP codec queue overflow drops=%u seq=0x%x len=%u",
                             (unsigned)s_overflow_drops, packet->layer_specific, packet->len);
        }
    }
    fixed_queue_enqueue(s_packet_queue, copy, FIXED_QUEUE_MAX_TIMEOUT);
    uint32_t queue_depth = (uint32_t)fixed_queue_length(s_packet_queue);
    if (queue_depth > s_max_queue_depth)
    {
        s_max_queue_depth = queue_depth;
    }
    osi_thread_post_event(s_data_event, OSI_THREAD_MAX_TIMEOUT);
    btc_a2dp_sink_ext_codec_log_stats(now_us);
}

#else

bool btc_a2dp_sink_ext_codec_startup(void) { return true; }
void btc_a2dp_sink_ext_codec_shutdown(void) {}
void btc_a2dp_sink_ext_codec_set_data_callback(esp_a2d_sink_data_cb_t callback) { (void)callback; }
void btc_a2dp_sink_ext_codec_set_rx_flush(bool flush) { (void)flush; }
void btc_a2dp_sink_ext_codec_enqueue(BT_HDR *packet) { osi_free(packet); }

#endif
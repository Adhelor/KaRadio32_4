// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/******************************************************************************
 **
 **  Name:          btc_a2dp_sink.c
 **
 ******************************************************************************/
#include "common/bt_target.h"
#include "common/bt_trace.h"
#include <string.h>
#include <stdint.h>
#include "common/bt_defs.h"
#include "osi/allocator.h"
#include "osi/mutex.h"
#include "osi/thread.h"
#include "osi/fixed_queue.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "stack/a2dp_vendor_ldac_constants.h"
#include "esp_log.h"
#include "bta/bta_av_api.h"
#include "bta/bta_av_ci.h"
#include "btc_av_co.h"
#include "btc_a2dp.h"
#include "btc_a2dp_control.h"
#include "btc_a2dp_sink.h"
#include "btc/btc_manage.h"
#include "btc_av.h"
#include "btc/btc_util.h"
#include "esp_a2dp_api.h"
#include "oi_codec_sbc.h"
#include "oi_status.h"
#include "osi/future.h"
#include <assert.h>

#if (BTC_AV_SINK_INCLUDED == TRUE)

/*****************************************************************************
 **  Constants
 *****************************************************************************/

/* BTC media cmd event definition : BTC_MEDIA_TASK_CMD */
enum
{
    BTC_MEDIA_TASK_SINK_INIT,
    BTC_MEDIA_TASK_SINK_CLEAN_UP,
    BTC_MEDIA_FLUSH_AA_RX,
    BTC_MEDIA_AUDIO_SINK_CFG_UPDATE,
    BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK,
};

enum
{
    BTC_A2DP_SINK_STATE_OFF = 0,
    BTC_A2DP_SINK_STATE_ON = 1,
    BTC_A2DP_SINK_STATE_SHUTTING_DOWN = 2
};

/*
 * CONGESTION COMPENSATION CTRL ::
 *
 * Thus setting controls how many buffers we will hold in media task
 * during temp link congestion. Together with the stack buffer queues
 * it controls much temporary a2dp link congestion we can
 * compensate for. It however also depends on the default run level of sinks
 * jitterbuffers. Depending on type of sink this would vary.
 * Ideally the (SRC) max tx buffer capacity should equal the sinks
 * jitterbuffer runlevel including any intermediate buffers on the way
 * towards the sinks codec.
 */

/* fixme -- define this in pcm time instead of buffer count */

/* The typical runlevel of the tx queue size is ~1 buffer
   but due to link flow control or thread preemption in lower
   layers we might need to temporarily buffer up data */

/* 18 frames is equivalent to 6.89*18*2.9 ~= 360 ms @ 44.1 khz, 20 ms mediatick */
#define MAX_OUTPUT_A2DP_SNK_FRAME_QUEUE_SZ (25)

#define BTC_A2DP_SNK_DATA_QUEUE_IDX (1)

typedef struct
{
    uint32_t sig;
    void *param;
} a2dp_sink_task_evt_t;

typedef struct
{
    UINT16 num_frames_to_be_processed;
    UINT16 len;
    UINT16 offset;
    UINT16 layer_specific;
} tBT_SBC_HDR;


typedef struct
{
    BOOLEAN rx_flush; /* discards any incoming data when true */
    UINT8 channel_count;
    struct osi_event *data_ready_event;
    fixed_queue_t *RxSbcQ;
    UINT32 sample_rate;
} tBTC_A2DP_SINK_CB;

typedef struct
{
    uint16_t expected_seq_num;
    bool seq_num_recount;
} a2dp_sink_media_pkt_seq_num_t;

typedef struct
{
    tBTC_A2DP_SINK_CB btc_aa_snk_cb;
    osi_thread_t *btc_aa_snk_task_hdl;
    OI_CODEC_SBC_DECODER_CONTEXT context;
    OI_UINT32 contextData[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
    OI_INT16 pcmData[15 * SBC_MAX_SAMPLES_PER_FRAME * SBC_MAX_CHANNELS];
    a2dp_sink_media_pkt_seq_num_t media_pkt_seq_num;
} a2dp_sink_local_param_t;

#if CONFIG_BT_SPEAKER_MODE
/* Forward declaration to fix implicit declaration error */
extern uint8_t bta_av_co_get_current_codec_id(void);
extern void bt_app_set_pcm_format(uint32_t sample_rate, uint8_t num_channels, uint8_t bit_depth);
extern void bt_app_set_format_params(uint32_t sample_rate, uint8_t num_channels);

#endif

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1)
extern void *aptx_init(int hd_mode); // hd_mode: 0 = standard, 1 = HD
extern void aptx_finish(void *ctx);

// Standardowy aptX (16-bit wyjście PCM)
extern int aptx_decode16(void *ctx, const unsigned char *input, size_t input_size,
                         short *output, size_t *output_size);

// aptX HD (32-bit wyjście PCM - idealne pod Twój zaktualizowany renderer!)
extern int aptx_decode32(void *ctx, const unsigned char *input, size_t input_size,
                         int *output, size_t *output_size);
#endif

#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
extern bool a2dp_ldac_decoder_decode_packet(void *p_buf, unsigned char *buf, size_t buf_len);
#endif
static void btc_a2dp_sink_thread_init(UNUSED_ATTR void *context);
static void btc_a2dp_sink_thread_cleanup(UNUSED_ATTR void *context);
static void btc_a2dp_sink_flush_q(fixed_queue_t *p_q);
static void btc_a2dp_sink_rx_flush(void);
static int btc_a2dp_sink_get_track_frequency(UINT8 frequency);
static int btc_a2dp_sink_get_track_channel_count(UINT8 channeltype);
/* Handle incoming media packets A2DP SINK streaming*/
static void btc_a2dp_sink_handle_inc_media(tBT_SBC_HDR *p_msg);
static void btc_a2dp_sink_handle_decoder_reset(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg);
static void btc_a2dp_sink_handle_clear_track(void);
static BOOLEAN btc_a2dp_sink_clear_track(void);

static void btc_a2dp_sink_data_ready(void *context);

static int btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_OFF;
static esp_a2d_sink_data_cb_t bt_aa_snk_data_cb = NULL;
#if A2D_DYNAMIC_MEMORY == FALSE
static a2dp_sink_local_param_t a2dp_sink_local_param;
#else
static a2dp_sink_local_param_t *a2dp_sink_local_param_ptr;
#define a2dp_sink_local_param (*a2dp_sink_local_param_ptr)
#endif /// A2D_DYNAMIC_MEMORY == FALSE

void btc_a2dp_sink_reg_data_cb(esp_a2d_sink_data_cb_t callback)
{
    // todo: critical section protection
    bt_aa_snk_data_cb = callback;
}

static inline void btc_a2d_data_cb_to_app(const uint8_t *data, uint32_t len)
{
    // todo: critical section protection
    if (bt_aa_snk_data_cb)
    {
        bt_aa_snk_data_cb(data, len);
    }
}

/*****************************************************************************
 **  Misc helper functions
 *****************************************************************************/
static inline void btc_a2d_cb_to_app(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    esp_a2d_cb_t btc_aa_cb = (esp_a2d_cb_t)btc_profile_cb_get(BTC_PID_A2DP);
    if (btc_aa_cb)
    {
        btc_aa_cb(event, param);
    }
}

/*****************************************************************************
 **  BTC ADAPTATION
 *****************************************************************************/

static bool btc_a2dp_sink_ctrl(uint32_t sig, void *param)
{
    switch (sig)
    {
    case BTC_MEDIA_TASK_SINK_INIT:
        btc_a2dp_sink_thread_init(NULL);
        break;
    case BTC_MEDIA_TASK_SINK_CLEAN_UP:
        btc_a2dp_sink_thread_cleanup(NULL);
        break;
    case BTC_MEDIA_AUDIO_SINK_CFG_UPDATE:
        btc_a2dp_sink_handle_decoder_reset(param);
        break;
    case BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK:
        btc_a2dp_sink_handle_clear_track();
        break;
    case BTC_MEDIA_FLUSH_AA_RX:
        btc_a2dp_sink_rx_flush();
        break;
    default:
        APPL_TRACE_WARNING("media task unhandled evt: 0x%x\n", sig);
    }

    if (param != NULL)
    {
        osi_free(param);
    }

    return true;
}

bool btc_a2dp_sink_startup(void)
{
    if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_OFF)
    {
        APPL_TRACE_ERROR("warning : media task already running");
        return false;
    }

#if A2D_DYNAMIC_MEMORY == TRUE
    if ((a2dp_sink_local_param_ptr = (a2dp_sink_local_param_t *)osi_malloc(sizeof(a2dp_sink_local_param_t))) == NULL)
    {
        APPL_TRACE_ERROR("%s malloc failed!", __func__);
        return false;
    }
    memset((void *)a2dp_sink_local_param_ptr, 0, sizeof(a2dp_sink_local_param_t));
#endif

    APPL_TRACE_EVENT("## A2DP SINK START MEDIA THREAD ##");

    a2dp_sink_local_param.btc_aa_snk_task_hdl = btc_get_current_thread();

    if (btc_a2dp_sink_ctrl(BTC_MEDIA_TASK_SINK_INIT, NULL) == false)
    {
        goto error_exit;
    }

    APPL_TRACE_EVENT("## A2DP SINK MEDIA THREAD STARTED ##\n");

    return true;

error_exit:;
    APPL_TRACE_ERROR("%s unable to start up media thread\n", __func__);
    a2dp_sink_local_param.btc_aa_snk_task_hdl = NULL;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_sink_local_param_ptr);
    a2dp_sink_local_param_ptr = NULL;
#endif

    return false;
}

void btc_a2dp_sink_shutdown(void)
{
    APPL_TRACE_EVENT("## A2DP SINK STOP MEDIA THREAD ##\n");

    // Exit thread
    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_SHUTTING_DOWN;

    btc_a2dp_sink_ctrl(BTC_MEDIA_TASK_SINK_CLEAN_UP, NULL);

    a2dp_sink_local_param.btc_aa_snk_task_hdl = NULL;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_sink_local_param_ptr);
    a2dp_sink_local_param_ptr = NULL;
#endif
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_on_idle
**
*******************************************************************************/

void btc_a2dp_sink_on_idle(void)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    btc_a2dp_sink_clear_track();

    APPL_TRACE_DEBUG("Stopped BT track");
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_on_stopped
**
*******************************************************************************/

void btc_a2dp_sink_on_stopped(tBTA_AV_SUSPEND *p_av)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    btc_a2dp_control_set_datachnl_stat(FALSE);
}

/*****************************************************************************
**
** Function        btc_a2dp_on_suspended
**
*******************************************************************************/

void btc_a2dp_sink_on_suspended(tBTA_AV_SUSPEND *p_av)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    return;
}

#if 0 /* only used by disabled custom codec routing below */
static void btc_a2dp_sink_data_post(void)
{
    osi_thread_post(a2dp_sink_local_param.btc_aa_snk_task_hdl, btc_a2dp_sink_data_ready, NULL,
                    BTC_A2DP_SNK_DATA_QUEUE_IDX, OSI_THREAD_MAX_TIMEOUT);
}
#endif

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_clear_track
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
static BOOLEAN btc_a2dp_sink_clear_track(void)
{
    return btc_a2dp_sink_ctrl(BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK, NULL);
}

/* when true media task discards any rx frames */
void btc_a2dp_sink_set_rx_flush(BOOLEAN enable)
{
    APPL_TRACE_EVENT("## DROP RX %d ##\n", enable);
    if (enable == FALSE)
    {
        a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num = 0x1;
        a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount = true;
    }
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = enable;
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_reset_decoder
**
** Description
**
** Returns
**
*******************************************************************************/

void btc_a2dp_sink_reset_decoder(UINT8 *p_av)
{
    APPL_TRACE_EVENT("btc reset decoder");
    APPL_TRACE_DEBUG("btc reset decoder p_codec_info[%x:%x:%x:%x:%x:%x]\n",
                     p_av[1], p_av[2], p_av[3],
                     p_av[4], p_av[5], p_av[6]);

    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf;
    if (NULL == (p_buf = osi_malloc(sizeof(tBTC_MEDIA_SINK_CFG_UPDATE))))
    {
        APPL_TRACE_ERROR("btc reset decoder No Buffer ");
        return;
    }

    memcpy(p_buf->codec_info, p_av, AVDT_CODEC_SIZE);
    btc_a2dp_sink_ctrl(BTC_MEDIA_AUDIO_SINK_CFG_UPDATE, p_buf);
}

static void btc_a2dp_sink_data_ready(UNUSED_ATTR void *context)
{
    tBT_SBC_HDR *p_msg;
    int nb_of_msgs_to_process = 0;
    if (fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ))
    {
        APPL_TRACE_DEBUG("  QUE  EMPTY ");
    }
    else
    {
        if (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush == TRUE)
        {
            btc_a2dp_sink_flush_q(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
            return;
        }
        nb_of_msgs_to_process = fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
        APPL_TRACE_DEBUG("nb:%d", nb_of_msgs_to_process);
        while (nb_of_msgs_to_process > 0)
        {
            if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_ON)
            {
                
                return;
            }
            p_msg = (tBT_SBC_HDR *)fixed_queue_dequeue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, 0);
            if (p_msg == NULL)
            {
                APPL_TRACE_ERROR("Insufficient data in que ");
                break;
            }
            btc_a2dp_sink_handle_inc_media(p_msg);
            osi_free(p_msg);
            nb_of_msgs_to_process--;
        }
        APPL_TRACE_DEBUG(" Process Frames - ");

        if (!fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ))
        {
            osi_thread_post_event(a2dp_sink_local_param.btc_aa_snk_cb.data_ready_event, OSI_THREAD_MAX_TIMEOUT);
        }
    }
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_decoder_reset
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_decoder_reset(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg)
{
    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf = p_msg;
    tA2D_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;
    OI_STATUS status;
    UINT32 freq_multiple = 48 * 20; /* frequency multiple for 20ms of data , initialize with 48K*/
    UINT32 num_blocks = 16;
    UINT32 num_subbands = 8;

    APPL_TRACE_EVENT("%s p_codec_info[%x:%x:%x:%x:%x:%x]\n", __FUNCTION__,
                     p_buf->codec_info[1], p_buf->codec_info[2], p_buf->codec_info[3],
                     p_buf->codec_info[4], p_buf->codec_info[5], p_buf->codec_info[6]);

    a2d_status = A2D_ParsSbcInfo(&sbc_cie, p_buf->codec_info, FALSE);
    if (a2d_status != A2D_SUCCESS)
    {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d\n", a2d_status);
        return;
    }

    a2dp_sink_local_param.btc_aa_snk_cb.sample_rate = btc_a2dp_sink_get_track_frequency(sbc_cie.samp_freq);
    a2dp_sink_local_param.btc_aa_snk_cb.channel_count = btc_a2dp_sink_get_track_channel_count(sbc_cie.ch_mode);
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = FALSE;
    status = OI_CODEC_SBC_DecoderReset(&a2dp_sink_local_param.context, a2dp_sink_local_param.contextData,
                                       sizeof(a2dp_sink_local_param.contextData), a2dp_sink_local_param.btc_aa_snk_cb.channel_count,
                                       a2dp_sink_local_param.btc_aa_snk_cb.channel_count, FALSE, FALSE);
    if (!OI_SUCCESS(status))
        APPL_TRACE_ERROR("OI_CODEC_SBC_DecoderReset failed with error code %d\n", status);

    btc_a2dp_control_set_datachnl_stat(TRUE);

    switch (sbc_cie.samp_freq)
    {
    case A2D_SBC_IE_SAMP_FREQ_16:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (16000)\n", sbc_cie.samp_freq);
        freq_multiple = 16 * 20;
        break;
    case A2D_SBC_IE_SAMP_FREQ_32:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (32000)\n", sbc_cie.samp_freq);
        freq_multiple = 32 * 20;
        break;
    case A2D_SBC_IE_SAMP_FREQ_44:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (44100)\n", sbc_cie.samp_freq);
        freq_multiple = 441 * 2;
        break;
    case A2D_SBC_IE_SAMP_FREQ_48:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (48000)\n", sbc_cie.samp_freq);
        freq_multiple = 48 * 20;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Frequency ");
        break;
    }

    switch (sbc_cie.ch_mode)
    {
    case A2D_SBC_IE_CH_MD_MONO:
        APPL_TRACE_DEBUG("\tch_mode:%d (Mono)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_DUAL:
        APPL_TRACE_DEBUG("\tch_mode:%d (DUAL)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_STEREO:
        APPL_TRACE_DEBUG("\tch_mode:%d (STEREO)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_JOINT:
        APPL_TRACE_DEBUG("\tch_mode:%d (JOINT)\n", sbc_cie.ch_mode);
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Mode ");
        break;
    }

    switch (sbc_cie.block_len)
    {
    case A2D_SBC_IE_BLOCKS_4:
        APPL_TRACE_DEBUG("\tblock_len:%d (4)\n", sbc_cie.block_len);
        num_blocks = 4;
        break;
    case A2D_SBC_IE_BLOCKS_8:
        APPL_TRACE_DEBUG("\tblock_len:%d (8)\n", sbc_cie.block_len);
        num_blocks = 8;
        break;
    case A2D_SBC_IE_BLOCKS_12:
        APPL_TRACE_DEBUG("\tblock_len:%d (12)\n", sbc_cie.block_len);
        num_blocks = 12;
        break;
    case A2D_SBC_IE_BLOCKS_16:
        APPL_TRACE_DEBUG("\tblock_len:%d (16)\n", sbc_cie.block_len);
        num_blocks = 16;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown BlockLen ");
        break;
    }

    switch (sbc_cie.num_subbands)
    {
    case A2D_SBC_IE_SUBBAND_4:
        APPL_TRACE_DEBUG("\tnum_subbands:%d (4)\n", sbc_cie.num_subbands);
        num_subbands = 4;
        break;
    case A2D_SBC_IE_SUBBAND_8:
        APPL_TRACE_DEBUG("\tnum_subbands:%d (8)\n", sbc_cie.num_subbands);
        num_subbands = 8;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown SubBands ");
        break;
    }

    switch (sbc_cie.alloc_mthd)
    {
    case A2D_SBC_IE_ALLOC_MD_S:
        APPL_TRACE_DEBUG("\talloc_mthd:%d (SNR)\n", sbc_cie.alloc_mthd);
        break;
    case A2D_SBC_IE_ALLOC_MD_L:
        APPL_TRACE_DEBUG("\talloc_mthd:%d (Loudness)\n", sbc_cie.alloc_mthd);
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Allocation Method");
        break;
    }

    APPL_TRACE_EVENT("\tBit pool Min:%d Max:%d\n", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

    int frames_to_process = ((freq_multiple) / (num_blocks * num_subbands)) + 1;
    APPL_TRACE_EVENT(" Frames to be processed in 20 ms %d\n", frames_to_process);
    UNUSED(frames_to_process);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_inc_media
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/

/* Custom AAC/aptX/LDAC media routing was moved out while restoring the stock SBC path.
 * Keep the previous implementation in repository history for later reintroduction
 * behind a codec-specific boundary.
 */
static void btc_a2dp_sink_handle_inc_media(tBT_SBC_HDR *p_msg)
{
    UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset + 1);
    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = a2dp_sink_local_param.pcmData;
    OI_STATUS status;
    int num_sbc_frames = p_msg->num_frames_to_be_processed;
    UINT32 sbc_frame_len = p_msg->len - 1;
    availPcmBytes = sizeof(a2dp_sink_local_param.pcmData);

    if (btc_av_get_peer_sep() == AVDT_TSEP_SNK || a2dp_sink_local_param.btc_aa_snk_cb.rx_flush)
        return;

    if (!btc_a2dp_control_get_datachnl_stat())
        return;

    if (p_msg->layer_specific != a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num)
    {
        if (!a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount ||
            a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num != 0x1)
        {
            APPL_TRACE_WARNING("Sequence numbers error, recv:0x%x, expect:0x%x, recount:0x%x",
                               p_msg->layer_specific,
                               a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num,
                               a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount);
        }
    }
    a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num = p_msg->layer_specific + 1;
    a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount = false;

    for (count = 0; count < num_sbc_frames && sbc_frame_len != 0; count++)
    {
        pcmBytes = availPcmBytes;
        status = OI_CODEC_SBC_DecodeFrame(&a2dp_sink_local_param.context,
                                          (const OI_BYTE **)&sbc_start_frame,
                                          (OI_UINT32 *)&sbc_frame_len,
                                          (OI_INT16 *)pcmDataPointer,
                                          (OI_UINT32 *)&pcmBytes);
        if (!OI_SUCCESS(status))
        {
            APPL_TRACE_ERROR("Decoding failure: %d\n", status);
            break;
        }
        availPcmBytes -= pcmBytes;
        pcmDataPointer += pcmBytes / 2;
        p_msg->offset += (p_msg->len - 1) - sbc_frame_len;
        p_msg->len = sbc_frame_len + 1;
    }

    btc_a2d_data_cb_to_app((uint8_t *)a2dp_sink_local_param.pcmData,
                           sizeof(a2dp_sink_local_param.pcmData) - availPcmBytes);
}


#if 0
/* Custom AAC/aptX/LDAC routing retained for reference; stock SBC is active. */
static void btc_a2dp_sink_handle_inc_media_custom_codec_routing(tBT_SBC_HDR *p_msg)
{
    /* Force dynamic routing based EXCLUSIVELY on the active negotiated codec ID */
    uint8_t current_id = bta_av_co_get_current_codec_id();
    tA2D_CODEC_TYPE active_codec = A2D_MEDIA_CT_SBC;

    if (current_id == 2)
    {
        active_codec = A2D_MEDIA_CT_M24;
    }
    else if (current_id == 255)
    {
        active_codec = A2D_MEDIA_CT_NON_A2DP;
    }
    else
    {
        active_codec = A2D_MEDIA_CT_SBC;
    }

    {
        static unsigned debug_route_calls = 0;
        if (debug_route_calls < 8)
        {
            extern uint8_t g_winning_codec_index;
            APPL_TRACE_ERROR("### DEBUG route current_id=%u active_codec=%u winning_index=%u offset=%u len=%u",
                             current_id, active_codec, g_winning_codec_index,
                             p_msg->offset, p_msg->len);
            debug_route_calls++;
        }
    }

#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
    if (active_codec == A2D_MEDIA_CT_M24)
    {
        int num_aac_frames = 1;
        uint8_t *aac_raw_ptr = tBT_SBC_DATA(p_msg) + p_msg->offset;
        uint32_t aac_raw_len = p_msg->len;
        if (aac_raw_len <= 1)
        {
            aac_raw_len = p_msg->layer_specific;
        }
        static bool log_once = false;
        if (!log_once)
        {
            ESP_LOGW("AAC_DEBUG", "p_msg->len: %d, layer_specific: %d, use len: %d",
                     p_msg->len, p_msg->layer_specific, aac_raw_len);
            log_once = true;
        }

        if (num_aac_frames > 0 && aac_raw_len > 1)
        {
            // Strip the 1-byte A2DP AAC control header
            // aac_raw_ptr += 1;
            // aac_raw_len -= 1;

            // Extract dynamic capabilities from selected indices
            uint8_t aac_freq_high = bta_av_co_cb.codec_cfg.info[3];
            uint8_t aac_freq_low = bta_av_co_cb.codec_cfg.info[4];

            uint8_t adts_freq_idx = 4; // Default fallback to 44.1kHz
            uint8_t adts_chan_cfg = 2; // Default fallback to Stereo

            // Direct mapping based on btc_av_co_multi_sink_caps bit layout
            if (aac_freq_high & 0x01)
            {
                adts_freq_idx = 4; // 44100 Hz
            }
            else if (aac_freq_low & 0x80)
            {
                adts_freq_idx = 3; // 48000 Hz
            }
            else if (aac_freq_high & 0x02)
            {
                adts_freq_idx = 5; // 32000 Hz
            }

            // Strictly validate channel mode mask (0x0C) from your fork layout
            if ((aac_freq_low & 0x0C) == 0x04)
            {
                adts_chan_cfg = 2; // Stereo
            }
            else if ((aac_freq_low & 0x0C) == 0x08)
            {
                adts_chan_cfg = 1; // Mono
            }

            const uint32_t freq_table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050};
            uint32_t target_hz = freq_table[adts_freq_idx];

            // Inter-component interface sync
            bt_app_set_format_params(target_hz, adts_chan_cfg);

uint8_t *latm_packet = (uint8_t *)malloc(aac_raw_len);
            if (latm_packet != NULL)
            {
                // FIX: Copy flat from index 0 to provide a clean, unshifted LATM container for FDK-AAC
                memcpy(latm_packet, aac_raw_ptr, aac_raw_len);
                btc_a2d_data_cb_to_app(latm_packet, aac_raw_len);
                free(latm_packet);
            }
            btc_a2dp_sink_data_post();
        }
        return;
    }
#endif
    // ============================================================================
    // TRACK 2: PREMIUM CODECS ROUTING (LDAC / aptX)
    // ============================================================================
#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1) || (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
    if (active_codec == A2D_MEDIA_CT_NON_A2DP) // FIX: Zmieniono z "else if" na czysty "if"
    {
        extern uint8_t g_winning_codec_index;

        /* Forward declaration of your genuine application data callback from bt_app_av.c */
        extern void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1)
        if (g_winning_codec_index == 4 || g_winning_codec_index == 5)
        {
            static void *aptx_context = NULL;
            static int current_hd_mode = -1;

            // 1. Identify mode based on winning SEID index (4 = Standard 16-bit, 5 = HD 32-bit)
            int target_hd_mode = (g_winning_codec_index == 5) ? 1 : 0;

            // 2. Dynamic codec re-initialization if stream mode toggled
            if (aptx_context == NULL || current_hd_mode != target_hd_mode)
            {
                if (aptx_context != NULL)
                {
                    aptx_finish(aptx_context);
                }
                current_hd_mode = target_hd_mode;
                aptx_context = aptx_init(current_hd_mode);
            }

            // 3. Extract safe raw data pointers from tBT_SBC_HDR header
            unsigned char *src = tBT_SBC_DATA(p_msg) + p_msg->offset;
            size_t src_size = p_msg->len;

            unsigned char *dst = (unsigned char *)a2dp_sink_local_param.pcmData;
            size_t avail_dst_space = sizeof(a2dp_sink_local_param.pcmData);
            size_t total_pcm_bytes_generated = 0;

            // Setup format descriptor for KaRadio SPI RAM FIFO and Renderer
            // Setup format parameters using baseline native types
            uint32_t target_sample_rate = 48000; // aptX standard natively targets 48kHz
            uint8_t target_channels = 2;
            uint8_t target_bit_depth = 16; // Baseline fallback

            // 4. Execute the targeted bit-depth decoding loop
            while (src_size > 0 && avail_dst_space > 0 && aptx_context != NULL)
            {
                size_t pcm_bytes_written_this_loop = 0;
                int bytes_consumed = 0;

                if (current_hd_mode == 1) // aptX HD -> Outputs native 32-bit aligned PCM
                {
                    target_bit_depth = 32; // Set renderer trigger to PATH A
                    bytes_consumed = aptx_decode32(aptx_context, src, src_size,
                                                   (int *)dst, &pcm_bytes_written_this_loop);
                }
                else // Standard aptX -> Outputs native 16-bit PCM
                {
                    target_bit_depth = 16; // Set renderer trigger to PATH B (requires padding)
                    bytes_consumed = aptx_decode16(aptx_context, src, src_size,
                                                   (short *)dst, &pcm_bytes_written_this_loop);
                }

                if (bytes_consumed <= 0)
                {
                    APPL_TRACE_ERROR("aptX decoder sync error: %d", bytes_consumed);
                    break;
                }

                src += bytes_consumed;
                src_size -= bytes_consumed;
                dst += pcm_bytes_written_this_loop;
                avail_dst_space -= pcm_bytes_written_this_loop;
                total_pcm_bytes_generated += pcm_bytes_written_this_loop;
            }

            // 5. Pipe generated PCM buffer structure straight into FIFO
            if (total_pcm_bytes_generated > 0)
            {
                bt_app_set_pcm_format(target_sample_rate, target_channels, target_bit_depth);

                // Forward data to your genuine callback that handles spiRamFifoWrite
                extern void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
                bt_app_a2d_data_cb((const uint8_t *)a2dp_sink_local_param.pcmData, total_pcm_bytes_generated);
            }
        }
#endif

#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
        if (g_winning_codec_index == 6 || g_winning_codec_index == 7)
        {
        static unsigned ldac_debug_packets = 0;
            // Forward declarations for the automatic Lazy Init sequence
            extern void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
            extern bool a2dp_ldac_decoder_init(void (*decode_callback)(const uint8_t *data, uint32_t len));

            static bool ldac_initialized = false;

            // Trigger allocation and internal setup ONLY when a genuine LDAC stream hits the receiver
            if (!ldac_initialized)
            {
                if (a2dp_ldac_decoder_init(bt_app_a2d_data_cb))
                {
                    ESP_LOGI("BTC_A2DP_SINK", "LDAC decoder lazy-init complete.");
                    ldac_initialized = true;
                }
                else
                {
                    APPL_TRACE_ERROR("LDAC lazy-init failed!");
                    btc_a2dp_sink_data_post();
                    return;
                }
            }

            // ============================================================================
            // FIX: OUT-OF-BAND PARAMETER INJECTION VIA UNIFIED FUNCTIONAL SETTER
            // ============================================================================
            // LDAC decoder outputs interleaved signed 16-bit PCM.
            bt_app_set_pcm_format(96000, 2, 16);
            // ============================================================================

            // Execute autonomous wrapper decoding straight to the application data pipeline
            if (p_msg->len <= A2DP_LDAC_MPL_HDR_LEN)
            {
                APPL_TRACE_WARNING("LDAC packet is shorter than its media header");
                return;
            }

            if (ldac_debug_packets < 8 && p_msg->len >= 8)
            {
                const uint8_t *raw = tBT_SBC_DATA(p_msg) + p_msg->offset;
                ESP_LOGE("LDAC_DBG", "### DEBUG raw offset=%u len=%u seq=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                         p_msg->offset, p_msg->len, p_msg->layer_specific,
                         raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
            }
            p_msg->offset += A2DP_LDAC_MPL_HDR_LEN;
            p_msg->len -= A2DP_LDAC_MPL_HDR_LEN;
            if (ldac_debug_packets < 8 && p_msg->len >= 8)
            {
                const uint8_t *payload = tBT_SBC_DATA(p_msg) + p_msg->offset;
                ESP_LOGE("LDAC_DBG", "### DEBUG decoder offset=%u len=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                         p_msg->offset, p_msg->len,
                         payload[0], payload[1], payload[2], payload[3],
                         payload[4], payload[5], payload[6], payload[7]);
                ldac_debug_packets++;
            }
            bool decode_ok = a2dp_ldac_decoder_decode_packet((BT_HDR *)p_msg,
                                                             (unsigned char *)a2dp_sink_local_param.pcmData,
                                                             sizeof(a2dp_sink_local_param.pcmData));
            if (decode_ok)
            {
                // btc_a2dp_sink_data_ready() reposts itself when queued packets remain.
            }
        }
#endif
        return;
    }
#endif

    // TRACK 3 fallback (legacy, unused: stock SBC handling lives in btc_a2dp_sink_handle_inc_media above)
    UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset + 1);
    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = a2dp_sink_local_param.pcmData;
    OI_STATUS status;
    int num_sbc_frames = p_msg->num_frames_to_be_processed;
    UINT32 sbc_frame_len = p_msg->len - 1;
    availPcmBytes = sizeof(a2dp_sink_local_param.pcmData);

    if (btc_av_get_peer_sep() == AVDT_TSEP_SNK || (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush))
    {
        return;
    }

    if (!btc_a2dp_control_get_datachnl_stat())
    {
        return;
    }

    if (p_msg->layer_specific != a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num)
    {
        if (!a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount ||
            a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num != 0x1)
        {
            APPL_TRACE_WARNING("Sequence numbers error, recv:0x%x, expect:0x%x, recount:0x%x",
                               p_msg->layer_specific, a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num,
                               a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount);
        }
    }
    a2dp_sink_local_param.media_pkt_seq_num.expected_seq_num = p_msg->layer_specific + 1;
    a2dp_sink_local_param.media_pkt_seq_num.seq_num_recount = false;

    for (count = 0; count < num_sbc_frames && sbc_frame_len != 0; count++)
    {
        pcmBytes = availPcmBytes;
        status = OI_CODEC_SBC_DecodeFrame(&a2dp_sink_local_param.context, (const OI_BYTE **)&sbc_start_frame,
                                          (OI_UINT32 *)&sbc_frame_len,
                                          (OI_INT16 *)pcmDataPointer,
                                          (OI_UINT32 *)&pcmBytes);
        if (!OI_SUCCESS(status))
        {
            break;
        }
        availPcmBytes -= pcmBytes;
        pcmDataPointer += pcmBytes / 2;
        p_msg->offset += (p_msg->len - 1) - sbc_frame_len;
        p_msg->len = sbc_frame_len + 1;
    }

    btc_a2d_data_cb_to_app((uint8_t *)a2dp_sink_local_param.pcmData, (sizeof(a2dp_sink_local_param.pcmData) - availPcmBytes));
}
#endif

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_rx_flush_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btc_a2dp_sink_rx_flush_req(void)
{
    if (fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ) == TRUE)
    { /*  Que is already empty */
        return TRUE;
    }

    return btc_a2dp_sink_ctrl(BTC_MEDIA_FLUSH_AA_RX, NULL);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_rx_flush
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_rx_flush(void)
{
    /* Flush all enqueued SBC  buffers (encoded) */
    APPL_TRACE_DEBUG("btc_a2dp_sink_rx_flush");

    btc_a2dp_sink_flush_q(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
}

static int btc_a2dp_sink_get_track_frequency(UINT8 frequency)
{
    int freq = 48000;
    switch (frequency)
    {
    case A2D_SBC_IE_SAMP_FREQ_16:
        freq = 16000;
        break;
    case A2D_SBC_IE_SAMP_FREQ_32:
        freq = 32000;
        break;
    case A2D_SBC_IE_SAMP_FREQ_44:
        freq = 44100;
        break;
    case A2D_SBC_IE_SAMP_FREQ_48:
        freq = 48000;
        break;
    }
    return freq;
}

static int btc_a2dp_sink_get_track_channel_count(UINT8 channeltype)
{
    int count = 1;
    switch (channeltype)
    {
    case A2D_SBC_IE_CH_MD_MONO:
        count = 1;
        break;
    case A2D_SBC_IE_CH_MD_DUAL:
    case A2D_SBC_IE_CH_MD_STEREO:
    case A2D_SBC_IE_CH_MD_JOINT:
        count = 2;
        break;
    }
    return count;
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_enque_buf
 **
 ** Description      This function is called by the av_co to fill A2DP Sink Queue
 **
 **
 ** Returns          size of the queue
 *******************************************************************************/
UINT8 btc_a2dp_sink_enque_buf(BT_HDR *p_pkt)
{
    tBT_SBC_HDR *p_msg;

    if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_ON)
    {
        return 0;
    }

    if (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush == TRUE)
    {
        return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
    }
    
    if (fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ) >= MAX_OUTPUT_A2DP_SNK_FRAME_QUEUE_SZ)
    {
        APPL_TRACE_WARNING("Pkt dropped\n");
        return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
    }

    if ((p_msg = (tBT_SBC_HDR *)osi_malloc(sizeof(tBT_SBC_HDR) + p_pkt->offset + p_pkt->len)) != NULL)
    {
        memcpy(p_msg, p_pkt, (sizeof(BT_HDR) + p_pkt->offset + p_pkt->len));
        p_msg->num_frames_to_be_processed = (*((UINT8 *)(p_msg + 1) + p_msg->offset)) & 0x0f;

        fixed_queue_enqueue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, p_msg, FIXED_QUEUE_MAX_TIMEOUT);
        osi_thread_post_event(a2dp_sink_local_param.btc_aa_snk_cb.data_ready_event, OSI_THREAD_MAX_TIMEOUT);
    }
    else
    {
        APPL_TRACE_WARNING("btc_a2dp_sink_enque_buf No Buffer left - ");
    }
    return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
}

static void btc_a2dp_sink_handle_clear_track(void)
{
    APPL_TRACE_DEBUG("%s", __FUNCTION__);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_flush_q
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_flush_q(fixed_queue_t *p_q)
{
    while (!fixed_queue_is_empty(p_q))
    {
        osi_free(fixed_queue_dequeue(p_q, 0));
    }
}

static void btc_a2dp_sink_thread_init(UNUSED_ATTR void *context)
{
    APPL_TRACE_EVENT("%s\n", __func__);
    memset(&a2dp_sink_local_param.btc_aa_snk_cb, 0, sizeof(a2dp_sink_local_param.btc_aa_snk_cb));

    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_ON;

    struct osi_event *data_event = osi_event_create(btc_a2dp_sink_data_ready, NULL);
    assert(data_event != NULL);
    osi_event_bind(data_event, a2dp_sink_local_param.btc_aa_snk_task_hdl, BTC_A2DP_SNK_DATA_QUEUE_IDX);
    a2dp_sink_local_param.btc_aa_snk_cb.data_ready_event = data_event;

    a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ = fixed_queue_new(QUEUE_SIZE_MAX);

    btc_a2dp_control_init();
}

static void btc_a2dp_sink_thread_cleanup(UNUSED_ATTR void *context)
{
    btc_a2dp_control_set_datachnl_stat(FALSE);
    /* Clear task flag */
    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_OFF;

    btc_a2dp_control_cleanup();

    fixed_queue_free(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, osi_free_func);

    a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ = NULL;

    osi_event_delete(a2dp_sink_local_param.btc_aa_snk_cb.data_ready_event);
    a2dp_sink_local_param.btc_aa_snk_cb.data_ready_event = NULL;
}

#endif /* BTC_AV_SINK_INCLUDED */

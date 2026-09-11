/******************************************************************************
 *
 *  Copyright (C) 2004-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This is the advanced audio/video call-out function implementation for
 *  BTC.
 *
 ******************************************************************************/
#include "string.h"
#include "common/bt_target.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "bta/bta_sys.h"
#include "bta/bta_av_api.h"
#include "bta/bta_av_co.h"
#include "bta/bta_av_ci.h"
#include "bta/bta_av_sbc.h"
#include "btc_a2dp.h"
#include "btc_a2dp_source.h"
#include "btc_av_co.h"
#include "btc/btc_util.h"
#include "osi/mutex.h"

#if BTC_AV_INCLUDED

/*****************************************************************************
 **  Constants
 *****************************************************************************/
#include "bt_av.h"
#include "stack/a2dp_codec_api.h"

#define FUNC_TRACE() APPL_TRACE_DEBUG("%s", __FUNCTION__);

/* Macro to retrieve the number of elements in a statically allocated array */
#define BTA_AV_CO_NUM_ELEMENTS(__a) (sizeof(__a) / sizeof((__a)[0]))

/* MIN and MAX macros */
#define BTA_AV_CO_MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define BTA_AV_CO_MAX(X, Y) ((X) > (Y) ? (X) : (Y))

/* Macro to convert audio handle to index and vice versa */
#define BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl) (((hndl) & (~BTA_AV_CHNL_MSK)) - 1)
#define BTA_AV_CO_AUDIO_INDX_TO_HNDL(indx) (((indx) + 1) | BTA_AV_CHNL_AUDIO)

/* Offsets to access codec information in SBC codec */
#define BTA_AV_CO_SBC_FREQ_CHAN_OFF 3
#define BTA_AV_CO_SBC_BLOCK_BAND_OFF 4
#define BTA_AV_CO_SBC_MIN_BITPOOL_OFF 5
#define A2D_SBC_IE_MAX_BITPOOL_OFF 6 // Upewniamy się, że to makro też jest jawne
#define BTA_AV_CO_SBC_MAX_BITPOOL_OFF 6

#define BTA_AV_CO_SBC_MAX_BITPOOL 53

/* SCMS-T protect info */
const UINT8 bta_av_co_cp_scmst[BTA_AV_CP_INFO_LEN] = "\x02\x02\x00";

/* SBC SRC codec capabilities */
const tA2D_SBC_CIE bta_av_co_sbc_caps = {
    (A2D_SBC_IE_SAMP_FREQ_44),
    (A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_STEREO | A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_DUAL),
    (A2D_SBC_IE_BLOCKS_16 | A2D_SBC_IE_BLOCKS_12 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_4),
    (A2D_SBC_IE_SUBBAND_4 | A2D_SBC_IE_SUBBAND_8),
    (A2D_SBC_IE_ALLOC_MD_L | A2D_SBC_IE_ALLOC_MD_S),
    BTA_AV_CO_SBC_MAX_BITPOOL,
    A2D_SBC_IE_MIN_BITPOOL};

/* SBC SINK codec capabilities */
const tA2D_SBC_CIE bta_av_co_sbc_sink_caps = {
    (A2D_SBC_IE_SAMP_FREQ_48 | A2D_SBC_IE_SAMP_FREQ_44),
    (A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_STEREO | A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_DUAL),
    (A2D_SBC_IE_BLOCKS_16 | A2D_SBC_IE_BLOCKS_12 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_4),
    (A2D_SBC_IE_SUBBAND_4 | A2D_SBC_IE_SUBBAND_8),
    (A2D_SBC_IE_ALLOC_MD_L | A2D_SBC_IE_ALLOC_MD_S),
    A2D_SBC_IE_MAX_BITPOOL,
    A2D_SBC_IE_MIN_BITPOOL};
static const uint8_t bta_av_co_cp_scms_t[] = {0x02, 0x00};

#define bta_av_co_cp_scmst bta_av_co_cp_scms_t

#ifndef FUNC_TRACE
#define FUNC_TRACE() APPL_TRACE_DEBUG("%s", __func__)
#endif

#include "stack/a2d_api.h"
#include "stack/a2dp_vendor.h"
#include "stack/a2dp_vendor_aptx.h"
#include "stack/a2dp_vendor_aptx_constants.h"
#include "stack/a2dp_vendor_aptx_hd.h"
#include "stack/a2dp_vendor_aptx_hd_constants.h"
#include "stack/a2dp_vendor_aptx_ll.h"
#include "stack/a2dp_vendor_aptx_ll_constants.h"
#include "stack/a2dp_vendor_ldac.h"
#include "stack/a2dp_vendor_ldac_constants.h"

#ifndef A2D_AAC_INFO_LEN
#define A2D_AAC_INFO_LEN 6
#endif
#ifndef A2D_SYSTEM_HEADER_LEN
#define A2D_SYSTEM_HEADER_LEN 3
#endif

/* AVDTP Media Codec Service Category ID */
#define AVDT_CAP_TYPE_CODEC 0x07

/* AAC capabilities definition bitmask overrides */
#define A2D_AAC_IE_OBJ_TYPE_MPEG4_LC 0x40
#define A2D_AAC_IE_SAMP_FREQ_44100 0x01
#define A2D_AAC_IE_SAMP_FREQ_48000 0x80
#define A2D_AAC_IE_CHANNELS_STEREO 0x04
#define A2D_AAC_IE_VBR_SUPPORTED 0x80
#define A2D_AAC_IE_BIT_RATE_HIGH 0x04
#define A2D_AAC_IE_BIT_RATE_MID 0xE2
#define A2D_AAC_IE_BIT_RATE_LOW 0x00

/* Combined multi-codec capabilities array for a single SEP entry */
const uint8_t btc_av_co_multi_sink_caps[] = {
    /* --- Block 1: SBC (Mandatory) --- */
    0x07,
    0x06,
    0x00,
    0x00,
    (A2D_SBC_IE_SAMP_FREQ_48 | A2D_SBC_IE_SAMP_FREQ_44),
    (A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_STEREO | A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_DUAL),
    (A2D_SBC_IE_BLOCKS_16 | A2D_SBC_IE_BLOCKS_12 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_4),
    (A2D_SBC_IE_SUBBAND_4 | A2D_SBC_IE_SUBBAND_8) | (A2D_SBC_IE_ALLOC_MD_L | A2D_SBC_IE_ALLOC_MD_S),

#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
    /* --- Block 2: AAC --- */
    0x07,
    0x08,
    0x00,
    0x02,
    A2D_AAC_IE_OBJ_TYPE_MPEG4_LC,
    A2D_AAC_IE_SAMP_FREQ_44100,
    A2D_AAC_IE_SAMP_FREQ_48000 | A2D_AAC_IE_CHANNELS_STEREO,
    A2D_AAC_IE_VBR_SUPPORTED | A2D_AAC_IE_BIT_RATE_HIGH,
    A2D_AAC_IE_BIT_RATE_MID,
    A2D_AAC_IE_BIT_RATE_LOW,
#endif

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1)
    /* --- Block 3: Qualcomm aptX Standard --- */
    0x07,
    0x09,
    0x00,
    0xFF,
    0xD7,
    0x00,
    0x00,
    0x00, /* Correct Qualcomm Vendor ID (Little Endian) */
    0x00,
    0x02, /* aptX Codec ID */
    (A2DP_APTX_SAMPLERATE_44100 | A2DP_APTX_SAMPLERATE_48000 | A2DP_APTX_CHANNELS_STEREO),
#endif

#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
    /* --- Block 4: Sony LDAC --- */
    0x07,
    0x0A,
    0x00,
    0xFF,
    0x2D,
    0x01,
    0x00,
    0x00, /* Correct Sony Vendor ID (Little Endian) */
    0x00,
    0xAA, /* LDAC Codec ID */
    (A2DP_LDAC_SAMPLING_FREQ_44100 | A2DP_LDAC_SAMPLING_FREQ_48000 | A2DP_LDAC_SAMPLING_FREQ_88200 | A2DP_LDAC_SAMPLING_FREQ_96000 | A2DP_LDAC_CHANNEL_MODE_STEREO),
#endif
};

#if !defined(BTC_AV_SBC_DEFAULT_SAMP_FREQ)
#define BTC_AV_SBC_DEFAULT_SAMP_FREQ A2D_SBC_IE_SAMP_FREQ_44
#endif

/* Default SBC codec configuration */
const tA2D_SBC_CIE btc_av_sbc_default_config = {
    BTC_AV_SBC_DEFAULT_SAMP_FREQ, /* samp_freq */
    A2D_SBC_IE_CH_MD_JOINT,       /* ch_mode */
    A2D_SBC_IE_BLOCKS_16,         /* block_len */
    A2D_SBC_IE_SUBBAND_8,         /* num_subbands */
    A2D_SBC_IE_ALLOC_MD_L,        /* alloc_mthd */
    BTA_AV_CO_SBC_MAX_BITPOOL,    /* max_bitpool */
    A2D_SBC_IE_MIN_BITPOOL        /* min_bitpool */
};

/* Control block instance */
#if AVRC_DYNAMIC_MEMORY == FALSE
tBTA_AV_CO_CB bta_av_co_cb;
#else
tBTA_AV_CO_CB *bta_av_co_cb_ptr;
#endif

/* Global latch to store the negotiated codec configuration index */
uint8_t g_winning_codec_index = 0xFF;

static BOOLEAN bta_av_co_audio_codec_build_config(const UINT8 *p_codec_caps, UINT8 *p_codec_cfg);
static void bta_av_co_audio_peer_reset_config(tBTA_AV_CO_PEER *p_peer);
static BOOLEAN bta_av_co_cp_is_scmst(const UINT8 *p_protectinfo);
static BOOLEAN bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK *p_sink);
static BOOLEAN bta_av_co_audio_peer_supports_codec(tBTA_AV_CO_PEER *p_peer, UINT8 *p_snk_index);
static UINT8 bta_av_co_audio_media_supports_config(UINT8 codec_type, const UINT8 *p_codec_cfg);
static UINT8 bta_av_co_audio_sink_supports_config(UINT8 codec_type, const UINT8 *p_codec_cfg);
static BOOLEAN bta_av_co_audio_peer_src_supports_codec(tBTA_AV_CO_PEER *p_peer, UINT8 *p_src_index);

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_is_active
 **
 ** Description      Get the current configuration of content protection
 **
 ** Returns          TRUE if the current streaming has CP, FALSE otherwise
 **
 *******************************************************************************/
BOOLEAN bta_av_co_cp_is_active(void)
{
    FUNC_TRACE();
    return bta_av_co_cb.cp.active;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_get_flag
 **
 ** Description      Get content protection flag
 **                  BTA_AV_CP_SCMS_COPY_NEVER
 **                  BTA_AV_CP_SCMS_COPY_ONCE
 **                  BTA_AV_CP_SCMS_COPY_FREE
 **
 ** Returns          The current flag value
 **
 *******************************************************************************/
UINT8 bta_av_co_cp_get_flag(void)
{
    FUNC_TRACE();
    return bta_av_co_cb.cp.flag;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_set_flag
 **
 ** Description      Set content protection flag
 **                  BTA_AV_CP_SCMS_COPY_NEVER
 **                  BTA_AV_CP_SCMS_COPY_ONCE
 **                  BTA_AV_CP_SCMS_COPY_FREE
 **
 ** Returns          TRUE if setting the SCMS flag is supported else FALSE
 **
 *******************************************************************************/
BOOLEAN bta_av_co_cp_set_flag(UINT8 cp_flag)
{
    FUNC_TRACE();

#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
#else
    if (cp_flag != BTA_AV_CP_SCMS_COPY_FREE)
    {
        return FALSE;
    }
#endif
    bta_av_co_cb.cp.flag = cp_flag;
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_get_peer
 **
 ** Description      find the peer entry for a given handle
 **
 ** Returns          the control block
 **
 *******************************************************************************/
static tBTA_AV_CO_PEER *bta_av_co_get_peer(tBTA_AV_HNDL hndl)
{
    UINT8 index;
    FUNC_TRACE();

    index = BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl);

    /* Sanity check */
    if (index >= BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers))
    {
        APPL_TRACE_ERROR("bta_av_co_get_peer peer index out of bounds:%d", index);
        return NULL;
    }

    return &bta_av_co_cb.peers[index];
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_init
 **
 ** Description      This callout function is executed by AV when it is
 **                  started by calling BTA_AvRegister().  This function can be
 **                  used by the phone to initialize audio paths or for other
 **                  initialization purposes.
 **
 **
 ** Returns          Stream codec and content protection capabilities info.
 **
 *******************************************************************************/
BOOLEAN bta_av_co_audio_init(UINT8 *p_codec_type, UINT8 *p_codec_info, UINT8 *p_num_protect,
                             UINT8 *p_protect_info, UINT8 tsep)
{
    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_init: %d", tsep);

    /* By default - no content protection info */
    *p_num_protect = 0;
    *p_protect_info = 0;

    /* Reset remote preference through setconfig */
    bta_av_co_cb.codec_cfg_setconfig.id = BTC_AV_CODEC_NONE;

    if (tsep == AVDT_TSEP_SRC)
    {
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
        do
        {
            UINT8 *p = p_protect_info;
            *p_num_protect = 1;
            *p++ = BTA_AV_CP_LOSC;
            UINT16_TO_STREAM(p, BTA_AV_CP_SCMS_T_ID);
        } while (0);
#endif
        /* Set up for SBC codec for SRC */
        *p_codec_type = BTA_AV_CODEC_SBC;
        A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *)&bta_av_co_sbc_caps, p_codec_info);
        return TRUE;
    }
    else if (tsep == AVDT_TSEP_SNK)
    {
        /* Define the primary SEP codec type as SBC (mandatory first) */
        *p_codec_type = BTA_AV_CODEC_SBC;

        /* Directly copy our self-contained unified caps array */
        memcpy(p_codec_info, btc_av_co_multi_sink_caps, sizeof(btc_av_co_multi_sink_caps));

        /* Force only 1 active physical Stream Endpoint (SEP) */
        tBTA_AV_CO_PEER *p_init_peer = &bta_av_co_cb.peers[0]; // bta_av_co_get_peer(BTA_AV_HNDL_MSK);
        if (p_init_peer != NULL)
        {
            p_init_peer->num_sup_snks = 1;
        }

        APPL_TRACE_DEBUG("bta_av_co_audio_init: Registered single multi-codec SEP.");

        /* bta_av_co_cb.sink_sep_registered is cleared in bta_av_co_init(), which runs
         * once per BT stack (re)enable cycle, so exactly one SEP is handed out per cycle. */
        if (!bta_av_co_cb.sink_sep_registered)
        {
            bta_av_co_cb.sink_sep_registered = TRUE;
            return TRUE;
        }
        return FALSE; /* Ensures all execution paths return a value */
    }
    else
    {
        APPL_TRACE_WARNING("invalid SEP type %d", tsep);
        return FALSE;
    }

    return FALSE; /* Catch-all for the very end of the function */
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_disc_res
 **
 ** Description      This callout function is executed by AV to report the
 **                  number of stream end points (SEP) were found during the
 **                  AVDT stream discovery process.
 **
 **
 ** Returns          void.
 **
 *******************************************************************************/
void bta_av_co_audio_disc_res(tBTA_AV_HNDL hndl, UINT8 num_seps, UINT8 num_snk,
                              UINT8 num_src, BD_ADDR addr, UINT16 uuid_local)
{
    tBTA_AV_CO_PEER *p_peer;

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_disc_res h:x%x num_seps:%d num_snk:%d num_src:%d",
                     hndl, num_seps, num_snk, num_src);

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_disc_res could not find peer entry");
        return;
    }

    /* Sanity check : this should never happen */
    if (p_peer->opened)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_disc_res peer already opened");
    }

    /* Copy the discovery results */
    bdcpy(p_peer->addr, addr);
    p_peer->num_snks = num_snk;
    p_peer->num_srcs = num_src;
    p_peer->num_seps = num_seps;
    p_peer->num_rx_snks = 0;
    p_peer->num_rx_srcs = 0;
    p_peer->num_sup_snks = 0;
    if (uuid_local == UUID_SERVCLASS_AUDIO_SINK)
    {
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SOURCE;
    }
    else if (uuid_local == UUID_SERVCLASS_AUDIO_SOURCE)
    {
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SINK;
    }
    p_peer->got_disc_res = TRUE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_cfg_res
 **
 ** Description      This callout function is executed by AV to report the
 **                  number of stream end points (SEP) were found during the
 **                  incoming AVDT stream config request process.
 **
 **
 ** Returns          void.
 **
 *******************************************************************************/
void bta_av_co_audio_cfg_res(tBTA_AV_HNDL hndl, UINT8 num_seps, UINT8 num_snk,
                             UINT8 num_src, BD_ADDR addr, UINT16 uuid_local)
{
    tBTA_AV_CO_PEER *p_peer;

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_cfg_res h:x%x num_seps:%d num_snk:%d num_src:%d",
                     hndl, num_seps, num_snk, num_src);

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_cfg_res could not find peer entry");
        return;
    }

    /* Sanity check : this should never happen */
    if (p_peer->opened)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_cfg_res peer already opened");
    }

    /* Copy the discovery results */
    bdcpy(p_peer->addr, addr);
    if (!p_peer->got_disc_res)
    {
        p_peer->num_snks = num_snk;
        p_peer->num_srcs = num_src;
        p_peer->num_seps = num_seps;
        p_peer->num_rx_snks = 0;
        p_peer->num_rx_srcs = 0;
        p_peer->num_sup_snks = 0;
    }
    if (uuid_local == UUID_SERVCLASS_AUDIO_SINK)
    {
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SOURCE;
    }
    else if (uuid_local == UUID_SERVCLASS_AUDIO_SOURCE)
    {
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SINK;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_build_src_cfg
 **
 ** Description      This function will build preferred config from src capabilities
 **
 **
 ** Returns          Pass or Fail for current getconfig.
 **
 *******************************************************************************/
void bta_av_build_src_cfg(UINT8 *p_pref_cfg, UINT8 *p_src_cap)
{
    tA2D_SBC_CIE src_cap;
    tA2D_SBC_CIE pref_cap;
    UINT8 status = 0;
    APPL_TRACE_DEBUG("\n\rbta_av_build_src_cfg | p_pref_cfg[0]:%d, p_src_cap[0]:%d", p_pref_cfg[0], p_src_cap[0]);

    /* If the codec is NOT standard SBC (SBC type is 0x00) */
    /* p_src_cap[2] holds the Codec Type: 0x00=SBC, 0x02=AAC, 0xFF=Vendor (aptX/LDAC) */
    if (p_src_cap != NULL && p_src_cap[2] != A2D_MEDIA_CT_SBC)
    {
        UINT8 codec_type = p_src_cap[2];
        UINT8 total_len = p_src_cap[0] + 1; /* Total block length defined by AVDTP header */

        /* Calculate only the payload/parameter length (subtracting the 3-byte AVDTP system header) */
        UINT8 info_len = (total_len > A2D_SYSTEM_HEADER_LEN) ? (total_len - A2D_SYSTEM_HEADER_LEN) : 0;

        /* Bound check to prevent buffer overflow in Bluedroid control blocks */
        if (total_len > AVDT_CODEC_SIZE)
        {
            total_len = AVDT_CODEC_SIZE;
        }
        if (info_len > AVDT_CODEC_SIZE)
        {
            info_len = AVDT_CODEC_SIZE;
        }

        /* 1. Directly mirror the phone's requested codec configuration layout */
        memcpy(p_pref_cfg, p_src_cap, total_len);

        /* 2. Lock the global control block registries with dynamic payload lengths */
        bta_av_co_cb.codec_cfg.id = codec_type;
        if (info_len > 0)
        {
            memcpy(bta_av_co_cb.codec_cfg.info, &p_src_cap[A2D_SYSTEM_HEADER_LEN], info_len);
        }

        bta_av_co_cb.codec_cfg_setconfig.id = codec_type;
        if (info_len > 0)
        {
            memcpy(bta_av_co_cb.codec_cfg_setconfig.info, &p_src_cap[A2D_SYSTEM_HEADER_LEN], info_len);
        }

        APPL_TRACE_DEBUG("bta_av_build_src_cfg: Non-SBC codec (type %d, len %d) mirrored and locked.", codec_type, info_len);
        return; /* Safe exit, bypasses SBC parser loop entirely */
    }
    else
    {
        APPL_TRACE_DEBUG("bta_av_build_src_cfg: Processing standard SBC configuration.");
    }

    /* initialize it to default SBC configuration */
    A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *)&btc_av_sbc_default_config, p_pref_cfg);
    /* now try to build a preferred one */
    /* parse configuration */
    if ((status = A2D_ParsSbcInfo(&src_cap, p_src_cap, TRUE)) != 0)
    {
        APPL_TRACE_DEBUG(" Cant parse src cap ret = %d", status);
        return;
    }

    if (src_cap.samp_freq & A2D_SBC_IE_SAMP_FREQ_48)
    {
        pref_cap.samp_freq = A2D_SBC_IE_SAMP_FREQ_48;
    }
    else if (src_cap.samp_freq & A2D_SBC_IE_SAMP_FREQ_44)
    {
        pref_cap.samp_freq = A2D_SBC_IE_SAMP_FREQ_44;
    }

    if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_JOINT)
    {
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_JOINT;
    }
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_STEREO)
    {
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_STEREO;
    }
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_DUAL)
    {
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_DUAL;
    }
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_MONO)
    {
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_MONO;
    }

    if (src_cap.block_len & A2D_SBC_IE_BLOCKS_16)
    {
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_16;
    }
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_12)
    {
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_12;
    }
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_8)
    {
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_8;
    }
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_4)
    {
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_4;
    }

    if (src_cap.num_subbands & A2D_SBC_IE_SUBBAND_8)
    {
        pref_cap.num_subbands = A2D_SBC_IE_SUBBAND_8;
    }
    else if (src_cap.num_subbands & A2D_SBC_IE_SUBBAND_4)
    {
        pref_cap.num_subbands = A2D_SBC_IE_SUBBAND_4;
    }

    if (src_cap.alloc_mthd & A2D_SBC_IE_ALLOC_MD_L)
    {
        pref_cap.alloc_mthd = A2D_SBC_IE_ALLOC_MD_L;
    }
    else if (src_cap.alloc_mthd & A2D_SBC_IE_ALLOC_MD_S)
    {
        pref_cap.alloc_mthd = A2D_SBC_IE_ALLOC_MD_S;
    }

    pref_cap.max_bitpool = src_cap.max_bitpool;
    pref_cap.min_bitpool = src_cap.min_bitpool;

    A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *)&pref_cap, p_pref_cfg);
}

/*******************************************************************************
 **
 ** Function         bta_av_audio_sink_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  A2DP Sink audio stream in Initiator.
 **
 **
 ** Returns          Pass or Fail for current getconfig.
 **
 *******************************************************************************/
UINT8 bta_av_audio_sink_getconfig(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type,
                                  UINT8 *p_codec_info, UINT8 *p_sep_info_idx, UINT8 seid, UINT8 *p_num_protect,
                                  UINT8 *p_protect_info)
{

    UINT8 result = A2D_FAIL;
    BOOLEAN supported;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_src;
    UINT8 pref_cfg[AVDT_CODEC_SIZE];
    UINT8 index;

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig handle:0x%x codec_type:%d seid:%d",
                     hndl, codec_type, seid);
    APPL_TRACE_DEBUG("num_protect:0x%02x protect_info:0x%02x%02x%02x",
                     *p_num_protect, p_protect_info[0], p_protect_info[1], p_protect_info[2]);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_audio_sink_getconfig could not find peer entry");
        return A2D_FAIL;
    }

    APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig peer(o=%d,n_snks=%d,n_rx_snks=%d,n_sup_snks=%d)",
                     p_peer->opened, p_peer->num_srcs, p_peer->num_rx_srcs, p_peer->num_sup_srcs);

    p_peer->num_rx_srcs++;

    /* Check if this is a supported configuration */
    supported = FALSE;
    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        supported = TRUE;
        break;

    default:
        break;
    }
#if 0 /* AAC/aptX/LDAC sink negotiation retained for reference; stock SBC-only is active. */
    switch (codec_type)
    {
    case 2: // AAC
#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
        supported = TRUE;
#else
        supported = FALSE;
#endif
        break;

    case 0xFF: // Vendor Specific (aptX/LDAC)
#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1) || (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
        supported = TRUE;
#else
        supported = FALSE;
#endif
        break;

    default:
        break;
    }
#endif
    if (supported)
    {
        /* If there is room for a new one */
        if (p_peer->num_sup_srcs < BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs))
        {
            p_src = &p_peer->srcs[p_peer->num_sup_srcs++];

            APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig saved caps[%x:%x:%x:%x:%x:%x]",
                             p_codec_info[1], p_codec_info[2], p_codec_info[3],
                             p_codec_info[4], p_codec_info[5], p_codec_info[6]);

            memcpy(p_src->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
            p_src->codec_type = codec_type;
            p_src->sep_info_idx = *p_sep_info_idx;
            p_src->seid = seid;
            p_src->num_protect = *p_num_protect;
            memcpy(p_src->protect_info, p_protect_info, BTA_AV_CP_INFO_LEN);
        }
        else
        {
            APPL_TRACE_ERROR("bta_av_audio_sink_getconfig no more room for SRC info");
        }
    }

    /* If last SNK get capabilities or all supported codec caps retrieved */
    if ((p_peer->num_rx_srcs == p_peer->num_srcs) ||
        (p_peer->num_sup_srcs == BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs)))
    {
        APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig last SRC reached");

        /* Protect access to bta_av_co_cb.codec_cfg */
        osi_mutex_global_lock();

        /* Find a src that matches the codec config */
        if (bta_av_co_audio_peer_src_supports_codec(p_peer, &index))
        {
            APPL_TRACE_DEBUG(" Codec Supported ");
            p_src = &p_peer->srcs[index];

            /* Save the new configuration */
            p_peer->p_src = p_src;
            /* get preferred config from src_caps */
            bta_av_build_src_cfg(pref_cfg, p_src->codec_caps);
            memcpy(p_peer->codec_cfg, pref_cfg, AVDT_CODEC_SIZE);

            APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig  p_codec_info[%x:%x:%x:%x:%x:%x]",
                             p_peer->codec_cfg[1], p_peer->codec_cfg[2], p_peer->codec_cfg[3],
                             p_peer->codec_cfg[4], p_peer->codec_cfg[5], p_peer->codec_cfg[6]);
            /* By default, no content protection */
            *p_num_protect = 0;
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
            p_peer->cp_active = FALSE;
            bta_av_co_cb.cp.active = FALSE;
#endif

            *p_sep_info_idx = p_src->sep_info_idx;
            memcpy(p_codec_info, p_peer->codec_cfg, AVDT_CODEC_SIZE);
            result = A2D_SUCCESS;
        }
#if 0 /* Premium codec cascade (LDAC/aptX/AAC priority) retained for reference; stock first-match SBC is active. */
        // ============================================================================
        // ADVANCED SINK CODEC CASCADE SEARCH (PRIORITY TO PREMIUM AUDIO)
        // ============================================================================
        /* Loop through all reported codecs in reverse order to prefer premium formats (LDAC -> aptX -> AAC -> SBC) */
        for (int i = (int)p_peer->num_sup_srcs - 1; i >= 0; i--)
        {
            /* Safely sync the control block codec tracker with the candidate peer source codec ID */
            bta_av_co_cb.codec_cfg.id = p_peer->srcs[i].codec_type;
            // printf("\n\r### DEBUG ### bta_av_audio_sink_getconfig: Evaluating index %d, candidate codec_id: %d\n\r", i, bta_av_co_cb.codec_cfg.id);

            if (bta_av_co_audio_peer_src_supports_codec(p_peer, &index))
            {
                APPL_TRACE_DEBUG(" Codec Supported (Cascade Premium Match) ");
                p_src = &p_peer->srcs[index];
                p_peer->p_src = p_src;

                /* Dynamically build preferred configuration for the winning premium codec layout */
                bta_av_build_src_cfg(pref_cfg, p_src->codec_caps);
                memcpy(p_peer->codec_cfg, pref_cfg, AVDT_CODEC_SIZE);

                APPL_TRACE_API("Selected codec index: %d, type: %d, seid: %d", index, p_src->codec_type, p_src->seid);

                APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig  p_codec_info[%x:%x:%x:%x:%x:%x]",
                                 p_peer->codec_cfg[1], p_peer->codec_cfg[2], p_peer->codec_cfg[3],
                                 p_peer->codec_cfg[4], p_peer->codec_cfg[5], p_peer->codec_cfg[6]);
                /* By default, no content protection */
                *p_num_protect = 0;
                /* Latch the unique hardware SEID directly into the winning index variable */
                g_winning_codec_index = p_src->seid;
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
                p_peer->cp_active = FALSE;
                bta_av_co_cb.cp.active = FALSE;
#endif

                *p_sep_info_idx = p_src->sep_info_idx;
                memcpy(p_codec_info, p_peer->codec_cfg, AVDT_CODEC_SIZE);
                result = A2D_SUCCESS;

                /* Release lock and exit instantly since the highest available codec is successfully negotiated */
                osi_mutex_global_unlock();
                return result;
            }
        }
        // ============================================================================
#endif

        /* Protect access to bta_av_co_cb.codec_cfg */
        osi_mutex_global_unlock();
    }
    return result;
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  audio stream.
 **
 **
 ** Returns          Stream codec and content protection configuration info.
 **
 *******************************************************************************/
UINT8 bta_av_co_audio_getconfig(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type,
                                UINT8 *p_codec_info, UINT8 *p_sep_info_idx, UINT8 seid, UINT8 *p_num_protect,
                                UINT8 *p_protect_info)

{
    UINT8 result = A2D_FAIL;
    BOOLEAN supported;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    UINT8 codec_cfg[AVDT_CODEC_SIZE];
    UINT8 index;

    FUNC_TRACE();

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);

    APPL_TRACE_DEBUG("### DEBUG ### bta_av_co_audio_getconfig ENTER! hndl: %d, seid: %d", hndl, seid);
    if (p_codec_info != NULL)
    {
        APPL_TRACE_DEBUG("### DEBUG ### Raw p_codec_info bytes in RAM: [0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X]",
                         p_codec_info[0], p_codec_info[1], p_codec_info[2],
                         p_codec_info[3], p_codec_info[4], p_codec_info[5],
                         p_codec_info[6], p_codec_info[7], p_codec_info[8]);
    }

    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_getconfig could not find peer entry");
        return A2D_FAIL;
    }

    if (p_peer->uuid_to_connect == UUID_SERVCLASS_AUDIO_SOURCE)
    {
        APPL_TRACE_DEBUG("### DEBUG ### bta_av_co_audio_getconfig: ENTER via UUID_SERVCLASS_AUDIO_SOURCE, type: %d, seid: %d\n", codec_type, seid);
        result = bta_av_audio_sink_getconfig(hndl, codec_type, p_codec_info, p_sep_info_idx,
                                             seid, p_num_protect, p_protect_info);
        APPL_TRACE_DEBUG("### DEBUG ### bta_av_co_audio_getconfig: bta_av_audio_sink_getconfig RETURNED result: %d\n", result);
        return result;
    }
    APPL_TRACE_WARNING("bta_av_co_audio_getconfig handle:0x%x codec_type:%d seid:%d",
                       hndl, codec_type, seid);
    APPL_TRACE_WARNING("num_protect:0x%02x protect_info:0x%02x%02x%02x",
                       *p_num_protect, p_protect_info[0], p_protect_info[1], p_protect_info[2]);

    APPL_TRACE_WARNING("bta_av_co_audio_getconfig peer(o=%d,n_snks=%d,n_rx_snks=%d,n_sup_snks=%d)",
                       p_peer->opened, p_peer->num_snks, p_peer->num_rx_snks, p_peer->num_sup_snks);

    p_peer->num_rx_snks++;

    /* Check if this is a supported configuration */
    supported = FALSE;
    /* Check if this is a supported configuration */
    supported = FALSE;
    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        supported = TRUE;
        break;

    case 2: // BTA_AV_CODEC_M24 (AAC)
#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
        supported = TRUE; // Zezwalaj tylko gdy aktywny w menuconfig
#else
        supported = FALSE;
#endif
        break;

    case 0xFF: // Vendor (LDAC / aptX)
#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1) || (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
        supported = TRUE; // Zezwalaj tylko gdy aktywny w menuconfig
#else
        supported = FALSE;
#endif
        break;

    default:
        break;
    }
    if (supported)
    {
        /* If there is room for a new one */
        if (p_peer->num_sup_snks < BTA_AV_CO_NUM_ELEMENTS(p_peer->snks))
        {
            p_sink = &p_peer->snks[p_peer->num_sup_snks++];

            APPL_TRACE_DEBUG("bta_av_co_audio_getconfig saved caps[%x:%x:%x:%x:%x:%x]",
                             p_codec_info[1], p_codec_info[2], p_codec_info[3],
                             p_codec_info[4], p_codec_info[5], p_codec_info[6]);

            memcpy(p_sink->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
            p_sink->codec_type = codec_type;
            p_sink->sep_info_idx = *p_sep_info_idx;
            p_sink->seid = seid;
            p_sink->num_protect = *p_num_protect;
            memcpy(p_sink->protect_info, p_protect_info, BTA_AV_CP_INFO_LEN);
        }
        else
        {
            APPL_TRACE_ERROR("bta_av_co_audio_getconfig no more room for SNK info");
        }
    }

    /* If last SNK get capabilities or all supported codec capa retrieved */
    if ((p_peer->num_rx_snks == p_peer->num_snks) ||
        (p_peer->num_sup_snks == BTA_AV_CO_NUM_ELEMENTS(p_peer->snks)))
    {
        APPL_TRACE_DEBUG("bta_av_co_audio_getconfig last sink reached");

        /* Protect access to bta_av_co_cb.codec_cfg */
        osi_mutex_global_lock();

        /* Find a sink that matches the codec config */
        if (bta_av_co_audio_peer_supports_codec(p_peer, &index))
        {
            /* stop fetching caps once we retrieved a supported codec */
            if (p_peer->acp)
            {
                *p_sep_info_idx = p_peer->num_seps;
                APPL_TRACE_EVENT("no need to fetch more SEPs");
            }

            p_sink = &p_peer->snks[index];

            /* Build the codec configuration for this sink */
            if (bta_av_co_audio_codec_build_config(p_sink->codec_caps, codec_cfg))
            {
                APPL_TRACE_DEBUG("bta_av_co_audio_getconfig reconfig p_codec_info[%x:%x:%x:%x:%x:%x]",
                                 codec_cfg[1], codec_cfg[2], codec_cfg[3],
                                 codec_cfg[4], codec_cfg[5], codec_cfg[6]);

                /* Save the new configuration */
                p_peer->p_snk = p_sink;
                memcpy(p_peer->codec_cfg, codec_cfg, AVDT_CODEC_SIZE);

                /* By default, no content protection */
                *p_num_protect = 0;

#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
                /* Check if this sink supports SCMS */
                if (bta_av_co_audio_sink_has_scmst(p_sink))
                {
                    p_peer->cp_active = TRUE;
                    bta_av_co_cb.cp.active = TRUE;
                    *p_num_protect = BTA_AV_CP_INFO_LEN;
                    memcpy(p_protect_info, bta_av_co_cp_scmst, BTA_AV_CP_INFO_LEN);
                }
                else
                {
                    p_peer->cp_active = FALSE;
                    bta_av_co_cb.cp.active = FALSE;
                }
#endif

                /* If acceptor -> reconfig otherwise reply for configuration */
                if (p_peer->acp)
                {
                    if (p_peer->recfg_needed)
                    {
                        APPL_TRACE_DEBUG("bta_av_co_audio_getconfig call BTA_AvReconfig(x%x)", hndl);
                        BTA_AvReconfig(hndl, TRUE, p_sink->sep_info_idx, p_peer->codec_cfg, *p_num_protect, (UINT8 *)bta_av_co_cp_scmst);
                    }
                }
                else
                {
                    *p_sep_info_idx = p_sink->sep_info_idx;
                    memcpy(p_codec_info, p_peer->codec_cfg, AVDT_CODEC_SIZE);
                }
                result = A2D_SUCCESS;
            }
        }
        /* Protect access to bta_av_co_cb.codec_cfg */
        osi_mutex_global_unlock();
    }
    return result;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_setconfig
 **
 ** Description      This callout function is executed by AV to set the codec and
 **                  content protection configuration of the audio stream.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_setconfig(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type,
                               UINT8 *p_codec_info, UINT8 seid, BD_ADDR addr, UINT8 num_protect, UINT8 *p_protect_info,
                               UINT8 t_local_sep, UINT8 avdt_handle)
{
    tBTA_AV_CO_PEER *p_peer;
    UINT8 status = A2D_SUCCESS;
    UINT8 category = A2D_SUCCESS;
    BOOLEAN recfg_needed = FALSE;
    UINT8 codec_cfg_status = A2D_SUCCESS;
    UNUSED(seid);
    UNUSED(addr);

    FUNC_TRACE();

    APPL_TRACE_WARNING("bta_av_co_audio_setconfig p_codec_info[%x:%x:%x:%x:%x:%x]",
                       p_codec_info[1], p_codec_info[2], p_codec_info[3],
                       p_codec_info[4], p_codec_info[5], p_codec_info[6]);
    APPL_TRACE_WARNING("num_protect:0x%02x protect_info:0x%02x%02x%02x",
                       num_protect, p_protect_info[0], p_protect_info[1], p_protect_info[2]);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_setconfig could not find peer entry");

        /* Call call-in rejecting the configuration */
        bta_av_ci_setconfig(hndl, A2D_BUSY, AVDT_ASC_CODEC, 0, NULL, FALSE, avdt_handle);
        return;
    }
    APPL_TRACE_WARNING("bta_av_co_audio_setconfig peer(o=%d,n_snks=%d,n_rx_snks=%d,n_sup_snks=%d)",
                       p_peer->opened, p_peer->num_snks, p_peer->num_rx_snks, p_peer->num_sup_snks);

    /* Sanity check: should not be opened at this point */
    if (p_peer->opened)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_setconfig peer already in use");
    }

#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
    if (num_protect != 0)
    {
        /* If CP is supported */
        if ((num_protect != 1) || (bta_av_co_cp_is_scmst(p_protect_info) == FALSE))
        {
            APPL_TRACE_ERROR("bta_av_co_audio_setconfig wrong CP configuration");
            status = A2D_BAD_CP_TYPE;
            category = AVDT_ASC_PROTECT;
        }
    }
#else
    /* Do not support content protection for the time being */
    if (num_protect != 0)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_setconfig wrong CP configuration");
        status = A2D_BAD_CP_TYPE;
        category = AVDT_ASC_PROTECT;
    }
#endif
    if (status == A2D_SUCCESS)
    {
        if (AVDT_TSEP_SNK == t_local_sep)
        {
            codec_cfg_status = bta_av_co_audio_sink_supports_config(codec_type, p_codec_info);
            APPL_TRACE_DEBUG(" Peer is  A2DP SRC ");
        }
        if (AVDT_TSEP_SRC == t_local_sep)
        {
            codec_cfg_status = bta_av_co_audio_media_supports_config(codec_type, p_codec_info);
            APPL_TRACE_DEBUG(" Peer is A2DP SINK ");
        }
        /* Check if codec configuration is supported */
        if (codec_cfg_status == A2D_SUCCESS)
        {

            /* Protect access to bta_av_co_cb.codec_cfg */
            osi_mutex_global_lock();
            bta_av_co_cb.codec_cfg.id = codec_type;
            /* Check if the configuration matches the current codec config */
            APPL_TRACE_DEBUG("### DEBUG ### bta_av_co_audio_setconfig codec_cfg.id: %d, codec_type: %d", bta_av_co_cb.codec_cfg.id, codec_type);
            switch (bta_av_co_cb.codec_cfg.id)
            {
            case BTC_AV_CODEC_SBC:
                if ((codec_type != BTA_AV_CODEC_SBC) || memcmp(p_codec_info, bta_av_co_cb.codec_cfg.info, 5))
                {
                    recfg_needed = TRUE;
                }
                else if ((num_protect == 1) && (!bta_av_co_cb.cp.active))
                {
                    recfg_needed = TRUE;
                }

                /* if remote side requests a restricted notify sinks preferred bitpool range as all other params are
                   already checked for validify */
                APPL_TRACE_WARNING("remote peer setconfig bitpool range [%d:%d]",
                                   p_codec_info[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                                   p_codec_info[BTA_AV_CO_SBC_MAX_BITPOOL_OFF]);

                bta_av_co_cb.codec_cfg_setconfig.id = BTC_AV_CODEC_SBC;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_info, AVDT_CODEC_SIZE);
                if (AVDT_TSEP_SNK == t_local_sep)
                {
                    memcpy(bta_av_co_cb.codec_cfg.info, p_codec_info, AVDT_CODEC_SIZE);
                    recfg_needed = FALSE;
                }
                break;

            case BTA_AV_CODEC_M24: // AAC
                codec_cfg_status = A2D_SUCCESS;
                recfg_needed = FALSE;

                bta_av_co_cb.codec_cfg_setconfig.id = BTA_AV_CODEC_M24;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_info, AVDT_CODEC_SIZE);
                memcpy(bta_av_co_cb.codec_cfg.info, p_codec_info, AVDT_CODEC_SIZE);
                break;

            case BTA_AV_CODEC_VEND: // BTA_AV_CODEC_VEND (LDAC)
                codec_cfg_status = A2D_SUCCESS;
                recfg_needed = FALSE;

                bta_av_co_cb.codec_cfg_setconfig.id = BTA_AV_CODEC_VEND;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_info, AVDT_CODEC_SIZE);
                memcpy(bta_av_co_cb.codec_cfg.info, p_codec_info, AVDT_CODEC_SIZE);
                break;
            }
            /* Protect access to bta_av_co_cb.codec_cfg */
            osi_mutex_global_unlock();
        }
        else
        {
            category = AVDT_ASC_CODEC;
            status = A2D_FAIL;
        }
    }

    if (status != A2D_SUCCESS)
    {
        APPL_TRACE_DEBUG("bta_av_co_audio_setconfig reject s=%d c=%d", codec_cfg_status, category);

        /* Call call-in rejecting the configuration */
        bta_av_ci_setconfig(hndl, codec_cfg_status, category, 0, NULL, FALSE, avdt_handle);
    }
    else
    {
        /* Mark that this is an acceptor peer */
        p_peer->acp = TRUE;
        p_peer->recfg_needed = recfg_needed;

        APPL_TRACE_DEBUG("bta_av_co_audio_setconfig accept reconf=%d", recfg_needed);

        /* Call call-in accepting the configuration */
        bta_av_ci_setconfig(hndl, A2D_SUCCESS, A2D_SUCCESS, 0, NULL, recfg_needed, avdt_handle);
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_open
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  is opened.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_open(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type, UINT8 *p_codec_info,
                          UINT16 mtu)
{
    tBTA_AV_CO_PEER *p_peer;
    UNUSED(p_codec_info);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_open mtu:%d codec_type:%d", mtu, codec_type);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_setconfig could not find peer entry");
    }
    else
    {
        p_peer->opened = TRUE;
        p_peer->mtu = mtu;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_close
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  is closed.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_close(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type, UINT16 mtu)

{
    tBTA_AV_CO_PEER *p_peer;
    UNUSED(codec_type);
    UNUSED(mtu);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_close");

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer)
    {
        /* Mark the peer closed and clean the peer info */
        memset(p_peer, 0, sizeof(*p_peer));
    }
    else
    {
        APPL_TRACE_ERROR("bta_av_co_audio_close could not find peer entry");
    }

    /* reset remote preference through setconfig */
    bta_av_co_cb.codec_cfg_setconfig.id = BTC_AV_CODEC_NONE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_start
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is started.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_start(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type,
                           UINT8 *p_codec_info, BOOLEAN *p_no_rtp_hdr)
{
    UNUSED(hndl);
    UNUSED(codec_type);
    UNUSED(p_codec_info);
    UNUSED(p_no_rtp_hdr);

    FUNC_TRACE();

    APPL_TRACE_WARNING("bta_av_co_audio_start");
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_stop
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is stopped.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
extern void bta_av_co_audio_stop(tBTA_AV_HNDL hndl, tBTA_AV_CODEC codec_type)
{
    UNUSED(hndl);
    UNUSED(codec_type);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_stop");
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_src_data_path
 **
 ** Description      This function is called to manage data transfer from
 **                  the audio codec to AVDTP.
 **
 ** Returns          Pointer to the buffer to send, NULL if no buffer to send
 **
 *******************************************************************************/
void *bta_av_co_audio_src_data_path(tBTA_AV_CODEC codec_type, UINT32 *p_len,
                                    UINT32 *p_timestamp)
{
#if BTC_AV_SRC_INCLUDED
    BT_HDR *p_buf;
    UNUSED(p_len);

    FUNC_TRACE();

    p_buf = btc_a2dp_source_audio_readbuf();
    if (p_buf != NULL)
    {
        switch (codec_type)
        {
        case BTA_AV_CODEC_SBC:
            /* In media packet SBC, the following information is available:
             * p_buf->layer_specific : number of SBC frames in the packet
             * p_buf->word[0] : timestamp
             */
            /* Retrieve the timestamp information from the media packet */
            *p_timestamp = *((UINT32 *)(p_buf + 1));

            /* Set up packet header */
            bta_av_sbc_bld_hdr(p_buf, p_buf->layer_specific);
            break;

        default:
            APPL_TRACE_ERROR("bta_av_co_audio_src_data_path Unsupported codec type (%d)", codec_type);
            break;
        }
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
        {
            UINT8 *p;
            if (bta_av_co_cp_is_active())
            {
                p_buf->len++;
                p_buf->offset--;
                p = (UINT8 *)(p_buf + 1) + p_buf->offset;
                *p = bta_av_co_cp_get_flag();
            }
        }
#endif
    }
    return p_buf;
#else  /* BTC_AV_SRC_INCLUDED */
    return NULL;
#endif /* BTC_AV_SRC_INCLUDED */
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_drop
 **
 ** Description      An Audio packet is dropped. .
 **                  It's very likely that the connected headset with this handle
 **                  is moved far away. The implementation may want to reduce
 **                  the encoder bit rate setting to reduce the packet size.
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_drop(tBTA_AV_HNDL hndl)
{
    FUNC_TRACE();

    APPL_TRACE_ERROR("bta_av_co_audio_drop dropped: x%x", hndl);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_delay
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  needs to send the initial delay report to the connected SRC.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_delay(tBTA_AV_HNDL hndl, UINT16 delay)
{
    FUNC_TRACE();

    APPL_TRACE_ERROR("bta_av_co_audio_delay handle: x%x, delay:0x%x", hndl, delay);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_build_config
 **
 ** Description      Build the codec configuration
 **
 ** Returns          TRUE if the codec was built successfully, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_codec_build_config(const UINT8 *p_codec_caps, UINT8 *p_codec_cfg)
{
    FUNC_TRACE();

    memset(p_codec_cfg, 0, AVDT_CODEC_SIZE);

    switch (bta_av_co_cb.codec_cfg.id)
    {
    case BTC_AV_CODEC_SBC:
        /*  only copy the relevant portions for this codec to avoid issues when
            comparing codec configs covering larger codec sets than SBC (7 bytes) */
        memcpy(p_codec_cfg, bta_av_co_cb.codec_cfg.info, BTA_AV_CO_SBC_MAX_BITPOOL_OFF + 1);

        /* Update the bit pool boundaries with the codec capabilities */
        p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF] = p_codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF];
        p_codec_cfg[BTA_AV_CO_SBC_MAX_BITPOOL_OFF] = p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF];

        APPL_TRACE_EVENT("bta_av_co_audio_codec_build_config : bitpool min %d, max %d",
                         p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                         p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF]);
        break;
    case BTA_AV_CODEC_M24:
        APPL_TRACE_DEBUG("bta_av_co_audio_codec_build_config: building AAC configuration header");
        memcpy(p_codec_cfg, bta_av_co_cb.codec_cfg.info, AVDT_CODEC_SIZE);
        break;

    // FIX: ADD COMPATIBILITY TRAILING FOR VENDOR PREMIUM CODECS (aptX / LDAC)
    case A2D_MEDIA_CT_NON_A2DP:
        APPL_TRACE_DEBUG("bta_av_co_audio_codec_build_config: building Vendor specific configuration header");
        memcpy(p_codec_cfg, bta_av_co_cb.codec_cfg.info, AVDT_CODEC_SIZE);
        break;
    default:
        APPL_TRACE_ERROR("bta_av_co_audio_codec_build_config: unsupported codec id %d", bta_av_co_cb.codec_cfg.id);
        return FALSE;
        break;
    }
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_cfg_matches_caps
 **
 ** Description      Check if a codec config matches a codec capabilities
 **
 ** Returns          TRUE if it codec config is supported, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_codec_cfg_matches_caps(UINT8 codec_id, const UINT8 *p_codec_caps, const UINT8 *p_codec_cfg)
{
    FUNC_TRACE();

    switch (codec_id)
    {
    case BTC_AV_CODEC_SBC:

        APPL_TRACE_WARNING("bta_av_co_audio_codec_cfg_matches_caps : min %d/%d max %d/%d",
                           p_codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                           p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                           p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF],
                           p_codec_cfg[BTA_AV_CO_SBC_MAX_BITPOOL_OFF]);

        /* Must match all items exactly except bitpool boundaries which can be adjusted */
        if (!((p_codec_caps[BTA_AV_CO_SBC_FREQ_CHAN_OFF] & p_codec_cfg[BTA_AV_CO_SBC_FREQ_CHAN_OFF]) &&
              (p_codec_caps[BTA_AV_CO_SBC_BLOCK_BAND_OFF] & p_codec_cfg[BTA_AV_CO_SBC_BLOCK_BAND_OFF])))
        {
            APPL_TRACE_WARNING("FALSE %x %x %x %x",
                               p_codec_caps[BTA_AV_CO_SBC_FREQ_CHAN_OFF],
                               p_codec_cfg[BTA_AV_CO_SBC_FREQ_CHAN_OFF],
                               p_codec_caps[BTA_AV_CO_SBC_BLOCK_BAND_OFF],
                               p_codec_cfg[BTA_AV_CO_SBC_BLOCK_BAND_OFF]);
            return FALSE;
        }
        break;

    default:
        APPL_TRACE_ERROR("bta_av_co_audio_codec_cfg_matches_caps: unsupported codec id %d", codec_id);
        return FALSE;
        break;
    }
    APPL_TRACE_EVENT("TRUE");

    return TRUE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_match
 **
 ** Description      Check if a codec capabilities supports the codec config
 **
 ** Returns          TRUE if the connection supports this codec, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_codec_match(const UINT8 *p_codec_caps)
{
    FUNC_TRACE();

    return bta_av_co_audio_codec_cfg_matches_caps(bta_av_co_cb.codec_cfg.id, p_codec_caps, bta_av_co_cb.codec_cfg.info);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_reset_config
 **
 ** Description      Reset the peer codec configuration
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
static void bta_av_co_audio_peer_reset_config(tBTA_AV_CO_PEER *p_peer)
{
    FUNC_TRACE();

    /* Indicate that there is no currently selected sink */
    p_peer->p_snk = NULL;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_is_scmst
 **
 ** Description      Check if a content protection service is SCMS-T
 **
 ** Returns          TRUE if this CP is SCMS-T, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_cp_is_scmst(const UINT8 *p_protectinfo)
{
    UINT16 cp_id;
    FUNC_TRACE();

    if (*p_protectinfo >= BTA_AV_CP_LOSC)
    {
        p_protectinfo++;
        STREAM_TO_UINT16(cp_id, p_protectinfo);
        if (cp_id == BTA_AV_CP_SCMS_T_ID)
        {
            APPL_TRACE_DEBUG("bta_av_co_cp_is_scmst: SCMS-T found");
            return TRUE;
        }
    }

    return FALSE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_has_scmst
 **
 ** Description      Check if a sink supports SCMS-T
 **
 ** Returns          TRUE if the sink supports this CP, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK *p_sink)
{
    UINT8 index;
    const UINT8 *p;
    FUNC_TRACE();

    /* Check if sink supports SCMS-T */
    index = p_sink->num_protect;
    p = &p_sink->protect_info[0];

    while (index)
    {
        if (bta_av_co_cp_is_scmst(p))
        {
            return TRUE;
        }
        /* Move to the next SC */
        p += *p + 1;
        /* Decrement the SC counter */
        index--;
    }
    APPL_TRACE_DEBUG("bta_av_co_audio_sink_has_scmst: SCMS-T not found");
    return FALSE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_supports_cp
 **
 ** Description      Check if a sink supports the current content protection
 **
 ** Returns          TRUE if the sink supports this CP, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_sink_supports_cp(const tBTA_AV_CO_SINK *p_sink)
{
    FUNC_TRACE();

    /* Check if content protection is enabled for this stream */
    if (bta_av_co_cp_get_flag() != BTA_AV_CP_SCMS_COPY_FREE)
    {
        return bta_av_co_audio_sink_has_scmst(p_sink);
    }
    else
    {
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_cp: not required");
        return TRUE;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_supports_codec
 **
 ** Description      Check if a connection supports the codec config
 **
 ** Returns          TRUE if the connection supports this codec, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_peer_supports_codec(tBTA_AV_CO_PEER *p_peer, UINT8 *p_snk_index)
{
    int index;
    UINT8 codec_type;
    FUNC_TRACE();

    /* Configure the codec type to look for */
    codec_type = bta_av_co_cb.codec_cfg.id;

    for (index = 0; index < p_peer->num_sup_snks; index++)
    {
        if (p_peer->snks[index].codec_type == codec_type)
        {
            switch (bta_av_co_cb.codec_cfg.id)
            {
            case BTC_AV_CODEC_SBC:
                if (p_snk_index)
                {
                    *p_snk_index = index;
                }
                // printf("\n\r ### DEBUG ### bta_av_co_audio_peer_supports_codec: SBC codec approved for id %d\n\r", codec_type);
                return bta_av_co_audio_codec_match(p_peer->snks[index].codec_caps);
                break;

            default:
                APPL_TRACE_ERROR("bta_av_co_audio_peer_supports_codec: unsupported codec id %d", bta_av_co_cb.codec_cfg.id);
                return FALSE;
                break;
            }
        }
    }
    return FALSE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_src_supports_codec
 **
 ** Description      Check if a peer acting as src supports codec config
 **
 ** Returns          TRUE if the connection supports this codec, FALSE otherwise
 **
 *******************************************************************************/
static BOOLEAN bta_av_co_audio_peer_src_supports_codec(tBTA_AV_CO_PEER *p_peer, UINT8 *p_src_index)
{
    int index;
    UINT8 codec_type;
    FUNC_TRACE();

    APPL_TRACE_DEBUG("peer_src_supports_codec: total codecs reported by phone: %d", p_peer->num_sup_srcs);
    for (index = 0; index < p_peer->num_sup_srcs; index++)
    {
        APPL_TRACE_DEBUG("peer_src_supports_codec: slot [%d] | peer_codec_type: %d",
                         index, p_peer->srcs[index].codec_type);
    }

    /* Target codec identifier requested by the upper layer */
    codec_type = bta_av_co_cb.codec_cfg.id;

    for (index = (int)p_peer->num_sup_srcs - 1; index >= 0; index--)
    {
        if (p_peer->srcs[index].codec_type == codec_type)
        {
            if (p_src_index)
            {
                *p_src_index = index;
            }

            switch (codec_type)
            {
            case BTA_AV_CODEC_SBC:
                /* FIX: Map internal comparison registry back to the clean structural format */
                if (0 == bta_av_sbc_cfg_matches_cap((UINT8 *)p_peer->srcs[index].codec_caps,
                                                    (tA2D_SBC_CIE *)&bta_av_co_sbc_sink_caps))
                {
                    APPL_TRACE_DEBUG("peer_src_supports_codec: SBC match found at index %d", index);
                    return TRUE;
                }
                break;

#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
            case BTA_AV_CODEC_M24:
                APPL_TRACE_DEBUG("peer_src_supports_codec: AAC match found at index %d", index);
                return TRUE;
#endif

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1) || (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
            case A2D_MEDIA_CT_NON_A2DP: /* Vendor-specific codecs (0xFF) */
            {
                const uint8_t *caps = p_peer->srcs[index].codec_caps;
                uint32_t vendor_id = (uint32_t)caps[3] |
                                     ((uint32_t)caps[4] << 8) |
                                     ((uint32_t)caps[5] << 16) |
                                     ((uint32_t)caps[6] << 24);
                uint16_t codec_id = (uint16_t)caps[7] | ((uint16_t)caps[8] << 8);
                bool supported = false;

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1)
                supported = (vendor_id == 0x0000000a &&
                             (codec_id == 0x0001 || codec_id == 0x0002));
#endif
#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
                supported = supported || (vendor_id == A2DP_LDAC_VENDOR_ID &&
                                          codec_id == A2DP_LDAC_CODEC_ID);
#endif
                APPL_TRACE_DEBUG("peer_src_supports_codec: vendor=0x%08x codec=0x%04x supported=%d index=%d",
                                 vendor_id, codec_id, supported, index);
                return supported;
            }
#endif

            default:
                APPL_TRACE_ERROR("peer_src_supports_codec: unsupported codec id %d", codec_type);
                return FALSE;
            }
        }
    }
    return FALSE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_supports_config
 **
 ** Description      Check if the media source supports a given configuration
 **
 ** Returns          TRUE if the media source supports this config, FALSE otherwise
 **
 *******************************************************************************/
static UINT8 bta_av_co_audio_sink_supports_config(UINT8 codec_type, const UINT8 *p_codec_cfg)
{
    FUNC_TRACE();
    UINT8 status = A2D_BAD_CODEC_TYPE;

    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: SBC validation");
        /* Validate using the strict structural validator to avoid offset memory corruption */
        status = bta_av_sbc_cfg_in_cap((UINT8 *)p_codec_cfg, (tA2D_SBC_CIE *)&bta_av_co_sbc_sink_caps);
        break;

    case BTA_AV_CODEC_M24: // AAC
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: AAC validation");
#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
        // AAC is enabled in menuconfig, we can accept the configuration and update the global structure
        bta_av_co_cb.codec_cfg_setconfig.id = BTA_AV_CODEC_M24;
        memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_cfg, AVDT_CODEC_SIZE);
        memcpy(bta_av_co_cb.codec_cfg.info, p_codec_cfg, AVDT_CODEC_SIZE);
        status = A2D_SUCCESS;
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: AAC approved");
#else
        // If disabled in menuconfig, simulate lack of support and DO NOT touch the bta_av_co_cb memory!
        status = A2D_BAD_CODEC_TYPE;
        APPL_TRACE_WARNING("bta_av_co_audio_sink_supports_config: AAC rejected (disabled in menuconfig)");
#endif
        break;

    case A2D_MEDIA_CT_NON_A2DP: /* Vendor-specific codecs (aptX, LDAC) use 0xFF */
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: Vendor codec validation");
        {
            // Parse Vendor ID (4 bytes, Little-Endian layout)
            UINT32 vendor_id = (UINT32)p_codec_cfg[0] | 
                               ((UINT32)p_codec_cfg[1] << 8) | 
                               ((UINT32)p_codec_cfg[2] << 16) | 
                               ((UINT32)p_codec_cfg[3] << 24);

            // Parse Codec ID (2 bytes, Little-Endian layout)
            UINT16 codec_id = (UINT16)p_codec_cfg[4] | 
                              ((UINT16)p_codec_cfg[5] << 8);

            APPL_TRACE_DEBUG("Vendor identification -> Vendor ID: 0x%08x, Codec ID: 0x%04x", vendor_id, codec_id);

            BOOLEAN codec_supported = FALSE;

            // TRACK 1: Qualcomm standard aptX
            if (vendor_id == 0x0000000a && codec_id == 0x0001)
            {
#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1)
                codec_supported = TRUE;
                APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: Standard aptX identified and approved.");
#else
                APPL_TRACE_WARNING("bta_av_co_audio_sink_supports_config: Standard aptX rejected (disabled in menuconfig)");
#endif
            }
            // TRACK 2: Qualcomm aptX HD
            else if (vendor_id == 0x0000000a && codec_id == 0x0002)
            {
#if (defined(APTX_HD_DEC_INCLUDED) && APTX_HD_DEC_INCLUDED == 1)
                codec_supported = TRUE;
                APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: aptX HD identified and approved.");
#else
                APPL_TRACE_WARNING("bta_av_co_audio_sink_supports_config: aptX HD rejected (disabled in menuconfig)");
#endif
            }
            // TRACK 3: Sony LDAC
            else if (vendor_id == 0x0000012d && codec_id == 0x00aa)
            {
#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
                codec_supported = TRUE;
                APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_config: Sony LDAC identified and approved.");
#else
                APPL_TRACE_WARNING("bta_av_co_audio_sink_supports_config: Sony LDAC rejected (disabled in menuconfig)");
#endif
            }

            // Commit structures only if the specific vendor profile is fully validated and compiled
            if (codec_supported)
            {
                bta_av_co_cb.codec_cfg_setconfig.id = A2D_MEDIA_CT_NON_A2DP;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_cfg, AVDT_CODEC_SIZE);
                memcpy(bta_av_co_cb.codec_cfg.info, p_codec_cfg, AVDT_CODEC_SIZE);
                status = A2D_SUCCESS;
            }
            else
            {
                status = A2D_BAD_CODEC_TYPE;
            }
        }
        break;

    case BTA_AV_CODEC_M12:
    case BTA_AV_CODEC_ATRAC:
        status = A2D_NS_CODEC_TYPE;
        APPL_TRACE_ERROR("bta_av_co_audio_sink_supports_config unsupported codec type %d", codec_type);
        break;

    default:
        APPL_TRACE_ERROR("bta_av_co_audio_sink_supports_config invalid codec type %d", codec_type);
        break;
    }

    ESP_LOGE("sink_supports_config", "bta_av_co_audio_sink_supports_config: codec_type=%d, status=%d", codec_type, status);
    return status;
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_media_supports_config
 **
 ** Description      Check if the media sink supports a given configuration
 **
 ** Returns          TRUE if the media source supports this config, FALSE otherwise
 **
 *******************************************************************************/
static UINT8 bta_av_co_audio_media_supports_config(UINT8 codec_type, const UINT8 *p_codec_cfg)
{
    FUNC_TRACE();
    UINT8 status = A2D_BAD_CODEC_TYPE;

    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        status = bta_av_sbc_cfg_in_cap((UINT8 *)p_codec_cfg, (tA2D_SBC_CIE *)&bta_av_co_sbc_caps);
        break;

#if (defined(AAC_DEC_INCLUDED) && AAC_DEC_INCLUDED == 1)
    case BTA_AV_CODEC_M24:
        APPL_TRACE_DEBUG("bta_av_co_audio_media_supports_config: AAC approved");
        status = A2D_SUCCESS;
        break;
#endif

#if (defined(APTX_DEC_INCLUDED) && APTX_DEC_INCLUDED == 1) || (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == 1)
    case A2D_MEDIA_CT_NON_A2DP:
        APPL_TRACE_DEBUG("bta_av_co_audio_media_supports_config: Vendor codec approved");
        status = A2D_SUCCESS;
        break;
#endif

    case BTA_AV_CODEC_M12:
    case BTA_AV_CODEC_ATRAC:
        status = A2D_NS_CODEC_TYPE;
        APPL_TRACE_ERROR("bta_av_co_audio_media_supports_config unsupported codec type %d", codec_type);
        break;
    default:
        APPL_TRACE_ERROR("bta_av_co_audio_media_supports_config invalid codec type %d", codec_type);
        break;
    }
    return status;
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_supported
 **
 ** Description      Check if all opened connections are compatible with a codec
 **                  configuration and content protection
 **
 ** Returns          TRUE if all opened devices support this codec, FALSE otherwise
 **
 *******************************************************************************/
BOOLEAN bta_av_co_audio_codec_supported(tBTC_AV_STATUS *p_status)
{
    UINT8 index;
    UINT8 snk_index;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    UINT8 codec_cfg[AVDT_CODEC_SIZE];
    UINT8 num_protect = 0;
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
    BOOLEAN cp_active;
#endif

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported");

    /* Check AV feeding is supported */
    *p_status = BTC_ERROR_SRV_AV_FEEDING_NOT_SUPPORTED;

    for (index = 0; index < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); index++)
    {
        p_peer = &bta_av_co_cb.peers[index];
        if (p_peer->opened)
        {
            if (bta_av_co_audio_peer_src_supports_codec(p_peer, &snk_index))
            {
                p_sink = &p_peer->snks[snk_index];

                /* Check that this sink is compatible with the CP */
                if (!bta_av_co_audio_sink_supports_cp(p_sink))
                {
                    APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported sink %d of peer %d doesn't support cp",
                                     snk_index, index);
                    *p_status = BTC_ERROR_SRV_AV_CP_NOT_SUPPORTED;
                    return FALSE;
                }

                /* Build the codec configuration for this sink */
                if (bta_av_co_audio_codec_build_config(p_sink->codec_caps, codec_cfg))
                {
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
                    /* Check if this sink supports SCMS */
                    cp_active = bta_av_co_audio_sink_has_scmst(p_sink);
#endif
                    /* Check if this is a new configuration (new sink or new config) */
                    if ((p_sink != p_peer->p_snk) ||
                        (memcmp(codec_cfg, p_peer->codec_cfg, AVDT_CODEC_SIZE))
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
                        || (p_peer->cp_active != cp_active)
#endif
                    )
                    {
                        /* Save the new configuration */
                        p_peer->p_snk = p_sink;
                        memcpy(p_peer->codec_cfg, codec_cfg, AVDT_CODEC_SIZE);
#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
                        p_peer->cp_active = cp_active;
                        if (p_peer->cp_active)
                        {
                            bta_av_co_cb.cp.active = TRUE;
                            num_protect = BTA_AV_CP_INFO_LEN;
                        }
                        else
                        {
                            bta_av_co_cb.cp.active = FALSE;
                        }
#endif
                        APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported call BTA_AvReconfig(x%x)", BTA_AV_CO_AUDIO_INDX_TO_HNDL(index));
                        BTA_AvReconfig(BTA_AV_CO_AUDIO_INDX_TO_HNDL(index), TRUE, p_sink->sep_info_idx,
                                       p_peer->codec_cfg, num_protect, (UINT8 *)bta_av_co_cp_scmst);
                    }
                }
            }
            else
            {
                APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported index %d doesn't support codec", index);
                return FALSE;
            }
        }
    }

    *p_status = BTC_AV_SUCCESS;
    return TRUE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_reset
 **
 ** Description      Reset the current codec configuration
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_codec_reset(void)
{
    osi_mutex_global_lock();
    FUNC_TRACE();

    /* Reset the current configuration to SBC */
    bta_av_co_cb.codec_cfg.id = BTC_AV_CODEC_SBC;

    if (A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, (tA2D_SBC_CIE *)&btc_av_sbc_default_config, bta_av_co_cb.codec_cfg.info) != A2D_SUCCESS)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_codec_reset A2D_BldSbcInfo failed");
    }

    osi_mutex_global_unlock();
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_set_codec
 **
 ** Description      Set the current codec configuration from the feeding type.
 **                  This function is starting to modify the configuration, it
 **                  should be protected.
 **
 ** Returns          TRUE if successful, FALSE otherwise
 **
 *******************************************************************************/
BOOLEAN bta_av_co_audio_set_codec(const tBTC_AV_MEDIA_FEEDINGS *p_feeding, tBTC_AV_STATUS *p_status)
{
    tA2D_SBC_CIE sbc_config;
    tBTC_AV_CODEC_INFO new_cfg;

    FUNC_TRACE();

    /* Check AV feeding is supported */
    *p_status = BTC_ERROR_SRV_AV_FEEDING_NOT_SUPPORTED;

    APPL_TRACE_DEBUG("bta_av_co_audio_set_codec cid=%d", p_feeding->format);

    /* Supported codecs */
    switch (p_feeding->format)
    {
    case BTC_AV_CODEC_PCM:
        new_cfg.id = BTC_AV_CODEC_SBC;

        sbc_config = btc_av_sbc_default_config;
        if ((p_feeding->cfg.pcm.num_channel != 1) &&
            (p_feeding->cfg.pcm.num_channel != 2))
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM channel number unsupported");
            return FALSE;
        }
        if ((p_feeding->cfg.pcm.bit_per_sample != 8) &&
            (p_feeding->cfg.pcm.bit_per_sample != 16))
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM sample size unsupported");
            return FALSE;
        }
        switch (p_feeding->cfg.pcm.sampling_freq)
        {
        case 8000:
        case 12000:
        case 16000:
        case 24000:
        case 32000:
        case 48000:
            sbc_config.samp_freq = A2D_SBC_IE_SAMP_FREQ_48;
            break;

        case 11025:
        case 22050:
        case 44100:
            sbc_config.samp_freq = A2D_SBC_IE_SAMP_FREQ_44;
            break;
        default:
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM sampling frequency unsupported");
            return FALSE;
            break;
        }
        /* Build the codec config */
        if (A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, &sbc_config, new_cfg.info) != A2D_SUCCESS)
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec A2D_BldSbcInfo failed");
            return FALSE;
        }
        break;

    default:
        APPL_TRACE_ERROR("bta_av_co_audio_set_codec Feeding format unsupported");
        return FALSE;
        break;
    }

    /* The new config was correctly built */
    bta_av_co_cb.codec_cfg = new_cfg;

    /* Check all devices support it */
    *p_status = BTC_AV_SUCCESS;
    return bta_av_co_audio_codec_supported(p_status);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_get_sbc_config
 **
 ** Description      Retrieves the SBC codec configuration.  If the codec in use
 **                  is not SBC, return the default SBC codec configuration.
 **
 ** Returns          TRUE if codec is SBC, FALSE otherwise
 **
 *******************************************************************************/
BOOLEAN bta_av_co_audio_get_sbc_config(tA2D_SBC_CIE *p_sbc_config, UINT16 *p_minmtu)
{
    BOOLEAN result = FALSE;
    UINT8 index, jndex;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;

    APPL_TRACE_EVENT("bta_av_co_cb.codec_cfg.id : codec 0x%x", bta_av_co_cb.codec_cfg.id);

    /* Minimum MTU is by default very large */
    *p_minmtu = 0xFFFF;

    osi_mutex_global_lock();
    if (bta_av_co_cb.codec_cfg.id == BTC_AV_CODEC_SBC)
    {
        if (A2D_ParsSbcInfo(p_sbc_config, bta_av_co_cb.codec_cfg.info, FALSE) == A2D_SUCCESS)
        {
            for (index = 0; index < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); index++)
            {
                p_peer = &bta_av_co_cb.peers[index];
                if (p_peer->opened)
                {
                    if (p_peer->mtu < *p_minmtu)
                    {
                        *p_minmtu = p_peer->mtu;
                    }
                    for (jndex = 0; jndex < p_peer->num_sup_snks; jndex++)
                    {
                        p_sink = &p_peer->snks[jndex];
                        if (p_sink->codec_type == A2D_MEDIA_CT_SBC)
                        {
                            /* Update the bitpool boundaries of the current config */
                            p_sbc_config->min_bitpool =
                                BTA_AV_CO_MAX(p_sink->codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                                              p_sbc_config->min_bitpool);
                            p_sbc_config->max_bitpool =
                                BTA_AV_CO_MIN(p_sink->codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF],
                                              p_sbc_config->max_bitpool);
                            APPL_TRACE_EVENT("bta_av_co_audio_get_sbc_config : sink bitpool min %d, max %d",
                                             p_sbc_config->min_bitpool, p_sbc_config->max_bitpool);
                            break;
                        }
                    }
                }
            }
            result = TRUE;
        }
    }

    if (!result)
    {
        /* Not SBC, still return the default values */
        *p_sbc_config = btc_av_sbc_default_config;
    }
    osi_mutex_global_unlock();

    return result;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_discard_config
 **
 ** Description      Discard the codec configuration of a connection
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
void bta_av_co_audio_discard_config(tBTA_AV_HNDL hndl)
{
    tBTA_AV_CO_PEER *p_peer;

    FUNC_TRACE();

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_discard_config could not find peer entry");
        return;
    }

    /* Reset the peer codec configuration */
    bta_av_co_audio_peer_reset_config(p_peer);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_init
 **
 ** Description      Initialization
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
void bta_av_co_init(void)
{
    FUNC_TRACE();

    /* Reset the control block */
    memset(&bta_av_co_cb, 0, sizeof(bta_av_co_cb));

    bta_av_co_cb.codec_cfg_setconfig.id = BTC_AV_CODEC_NONE;

#if defined(BTA_AV_CO_CP_SCMS_T) && (BTA_AV_CO_CP_SCMS_T == TRUE)
    bta_av_co_cp_set_flag(BTA_AV_CP_SCMS_COPY_NEVER);
#else
    bta_av_co_cp_set_flag(BTA_AV_CP_SCMS_COPY_FREE);
#endif

    /* Reset the current config */
    bta_av_co_audio_codec_reset();
}

/*******************************************************************************
 **
 ** Function         bta_av_co_peer_cp_supported
 **
 ** Description      Checks if the peer supports CP
 **
 ** Returns          TRUE if the peer supports CP
 **
 *******************************************************************************/
BOOLEAN bta_av_co_peer_cp_supported(tBTA_AV_HNDL hndl)
{
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    UINT8 index;

    FUNC_TRACE();

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_peer_cp_supported could not find peer entry");
        return FALSE;
    }

    for (index = 0; index < p_peer->num_sup_snks; index++)
    {
        p_sink = &p_peer->snks[index];
        if (p_sink->codec_type == A2D_MEDIA_CT_SBC)
        {
            return bta_av_co_audio_sink_has_scmst(p_sink);
        }
    }
    APPL_TRACE_ERROR("bta_av_co_peer_cp_supported did not find SBC sink");
    return FALSE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_get_remote_bitpool_pref
 **
 ** Description      Check if remote side did a setconfig within the limits
 **                  of our exported bitpool range. If set we will set the
 **                  remote preference.
 **
 ** Returns          TRUE if config set, FALSE otherwize
 **
 *******************************************************************************/

BOOLEAN bta_av_co_get_remote_bitpool_pref(UINT8 *min, UINT8 *max)
{
    /* check if remote peer did a set config */
    if (bta_av_co_cb.codec_cfg_setconfig.id == BTC_AV_CODEC_NONE)
    {
        if (bta_av_co_cb.codec_cfg.id == A2D_MEDIA_CT_NON_A2DP)
        {
            /* Inject safe baseline values for the premium track to pass the internal stack parser */
            *min = btc_av_sbc_default_config.min_bitpool;
            *max = btc_av_sbc_default_config.max_bitpool;
            return TRUE;
        }
        return FALSE;
    }

    *min = bta_av_co_cb.codec_cfg_setconfig.info[BTA_AV_CO_SBC_MIN_BITPOOL_OFF];
    *max = bta_av_co_cb.codec_cfg_setconfig.info[BTA_AV_CO_SBC_MAX_BITPOOL_OFF];

    return TRUE;
}
uint8_t bta_av_co_get_current_codec_id(void)
{
    static uint8_t last_logged_codec_id = 0xFE;
    uint8_t current_id = bta_av_co_cb.codec_cfg.id;

    // On codec_id change, give debug log
    if (current_id != last_logged_codec_id)
    {
        // printf("\n\r ### DEBUG ### bta_av_co_get_current_codec_id: codec id changed to %d\n\r", current_id);
        last_logged_codec_id = current_id;
    }

    return current_id;
}
// uint8_t bta_av_co_get_current_codec_index(void)
// {
//     /* Simply return the globally stored index from the setconfig transaction block */
//     /* bta_av_co_cb.codec_cfg_setconfig contains the active parameters assigned by the source */
//     return bta_av_co_cb.codec_cfg_setconfig.id;
// }
/* the call out functions for audio stream */
const tBTA_AV_CO_FUNCTS bta_av_a2d_cos = {
    bta_av_co_audio_init,
    bta_av_co_audio_disc_res,
    bta_av_co_audio_cfg_res,
    bta_av_co_audio_getconfig,
    bta_av_co_audio_setconfig,
    bta_av_co_audio_open,
    bta_av_co_audio_close,
    bta_av_co_audio_start,
    bta_av_co_audio_stop,
    bta_av_co_audio_src_data_path,
    bta_av_co_audio_delay};

#endif /* #if BTC_AV_INCLUDED */

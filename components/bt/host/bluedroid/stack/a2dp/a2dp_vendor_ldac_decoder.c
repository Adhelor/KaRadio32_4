/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/bt_trace.h"
#include "stack/a2dp_vendor_ldac_constants.h"
#include "stack/a2dp_vendor_ldac_decoder.h"
#include "stack/a2dp_ldac_pcm.h"
#include "esp_log.h"
#include "ldacdec.h"

#if (defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == TRUE)

typedef struct
{
    ldacdec_t decoder;
    decoded_data_callback_t decode_callback;
} tA2DP_LDAC_DECODER_CB;

static tA2DP_LDAC_DECODER_CB a2dp_ldac_decoder_cb;
static bool a2dp_ldac_decoder_initialized;

bool a2dp_ldac_decoder_init(decoded_data_callback_t decode_callback)
{
    int res;

    if (!a2dp_ldac_decoder_initialized)
    {
        res = ldacdecInit(&a2dp_ldac_decoder_cb.decoder);
        if (res)
        {
            APPL_TRACE_ERROR("%s: decoder init failed %d", __func__, res);
            return false;
        }
        a2dp_ldac_decoder_initialized = true;
    }
    a2dp_ldac_decoder_cb.decode_callback = decode_callback;
    return true;
}

bool a2dp_ldac_decoder_init_pcm(void)
{
    return a2dp_ldac_decoder_init(NULL);
}

size_t a2dp_ldac_decoder_decode_packet_header(BT_HDR *p_buf)
{
    size_t header_len = sizeof(struct media_packet_header) +
                        A2DP_LDAC_MPL_HDR_LEN;
    p_buf->offset += header_len;
    p_buf->len -= header_len;
    return 0;
}

static bool find_sync_word(unsigned char **buf, int *size)
{
    while ((*size) > 0 && **buf != 0xAA)
    {
        (*buf)++;
        (*size)--;
    }

    return (*size) > 0;
}

bool a2dp_ldac_decoder_decode_packet(BT_HDR *p_buf, unsigned char *buf, size_t buf_len)
{
    ldacdec_t *decoder = &a2dp_ldac_decoder_cb.decoder;
    int ret;

    unsigned char *src = ((unsigned char *)(p_buf + 1) + p_buf->offset);
    int src_size = p_buf->len;
    unsigned char *dst = buf;
    int32_t avail = buf_len;

    while (src_size > 0 && avail > 0)
    {
        int bytes_used;
        int out_size;

        if (!find_sync_word(&src, &src_size))
        {
            break;
        }

        ret = ldacDecode(decoder, src, src_size, (void *)dst, &bytes_used);
        if (ret != 0)
        {
            APPL_TRACE_ERROR("%s: decoder error %d", __func__, ret);
            return false;
        }
        src += bytes_used;
        src_size -= bytes_used;

        frame_t *frame = &decoder->frame;
        out_size = frame->frameSamples * frame->channelCount * sizeof(int16_t);
        dst += out_size;
        avail -= out_size;
    }

    if (src_size > 0 && avail <= 0)
    {
        LOG_ERROR("%s: Insufficient output buffer size. %d bytes remain.", __func__, src_size);
    }

    size_t len = buf_len - avail;
    len = len <= buf_len ? len : buf_len;
    if (a2dp_ldac_decoder_cb.decode_callback != NULL && len > 0)
    {
        a2dp_ldac_decoder_cb.decode_callback((uint8_t *)buf, len);
    }
    return true;
}

bool a2dp_ldac_decoder_decode_payload(const uint8_t *payload, size_t payload_len,
                                      uint8_t *pcm, size_t pcm_capacity, size_t *pcm_len,
                                      uint32_t *sample_rate, uint8_t *channel_count)
{
    static const size_t max_frame_pcm_bytes = MAX_FRAME_SAMPLES * 2 * sizeof(int16_t);
    uint8_t *src;
    int src_size;
    size_t pcm_written = 0;

    if (!a2dp_ldac_decoder_initialized || payload == NULL || pcm == NULL || pcm_len == NULL ||
        payload_len <= A2DP_LDAC_MPL_HDR_LEN)
    {
        return false;
    }

    src = (uint8_t *)payload + A2DP_LDAC_MPL_HDR_LEN;
    src_size = payload_len - A2DP_LDAC_MPL_HDR_LEN;

    while (src_size > 0)
    {
        int bytes_used = 0;
        size_t frame_pcm_bytes;

        if (!find_sync_word(&src, &src_size))
        {
            break;
        }
        if (pcm_capacity - pcm_written < max_frame_pcm_bytes)
        {
            break;
        }

        if (ldacDecode(&a2dp_ldac_decoder_cb.decoder, src, src_size,
                       (int16_t *)(pcm + pcm_written), &bytes_used) != 0 ||
            bytes_used <= 0 || bytes_used > src_size)
        {
            return false;
        }

        frame_pcm_bytes = (size_t)a2dp_ldac_decoder_cb.decoder.frame.frameSamples *
                          a2dp_ldac_decoder_cb.decoder.frame.channelCount * sizeof(int16_t);
        src += bytes_used;
        src_size -= bytes_used;
        pcm_written += frame_pcm_bytes;
    }

    *pcm_len = pcm_written;
    if (pcm_written == 0)
    {
        return false;
    }
    if (sample_rate != NULL)
    {
        *sample_rate = (uint32_t)ldacdecGetSampleRate(&a2dp_ldac_decoder_cb.decoder);
    }
    if (channel_count != NULL)
    {
        *channel_count = (uint8_t)ldacdecGetChannelCount(&a2dp_ldac_decoder_cb.decoder);
    }
    return true;
}

#endif /* defined(LDAC_DEC_INCLUDED) && LDAC_DEC_INCLUDED == TRUE) */

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
#include "sdkconfig.h"
#if CONFIG_BT_SPEAKER_MODE
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "addon.h"
#include "bt_app_core.h"
#include "bt_app_av.h"
#include "bt_speaker.h"
#include "gpio.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "webclient.h"
#include "webserver.h"
#include "eeprom.h"
#include "audio_renderer.h"
#include "audio_player.h"
#include "interface.h"
#include "webclient.h"
#include "app_main.h"
#include "esp_bt.h"
#include "common_buffer.h"
#include "spiram_fifo.h"
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
#include "stack/a2dp_ldac_pcm.h"
#include "stack/a2dp_vendor_ldac_constants.h"
#endif
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
#include "stack/a2dp_codec_api.h"
#include "stack/a2dp_vendor_aptx_decoder.h"
extern uint8_t bta_av_co_get_current_vendor_codec_index(void);
#endif

extern bool bta_av_co_get_current_ldac_audio_format(uint32_t *sample_rate, uint8_t *channel_count);
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
static bool bt_is_current_aptx_codec(void)
{
    uint8_t codec_index = bta_av_co_get_current_vendor_codec_index();
#if defined(CONFIG_BT_A2DP_APTX_VARIANT_STANDARD) && CONFIG_BT_A2DP_APTX_VARIANT_STANDARD
    return codec_index == BTAV_A2DP_CODEC_INDEX_SINK_APTX;
#elif defined(CONFIG_BT_A2DP_APTX_VARIANT_HD) && CONFIG_BT_A2DP_APTX_VARIANT_HD
    return codec_index == BTAV_A2DP_CODEC_INDEX_SINK_APTX_HD;
#elif defined(CONFIG_BT_A2DP_APTX_VARIANT_LL) && CONFIG_BT_A2DP_APTX_VARIANT_LL
    return codec_index == BTAV_A2DP_CODEC_INDEX_SINK_APTX_LL;
#else
    return false;
#endif
}
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/i2s.h"
#include "sys/lock.h"

#include "audio_renderer.h"
#define TAG "BT_PCM_DECODER"
/* AVRCP used transaction labels */
#define APP_RC_CT_TL_GET_CAPS (0)
#define APP_RC_CT_TL_GET_META_DATA (1)
#define APP_RC_CT_TL_RN_TRACK_CHANGE (2)
#define APP_RC_CT_TL_RN_PLAYBACK_CHANGE (3)
#define APP_RC_CT_TL_RN_PLAY_POS_CHANGE (4)

#ifdef __cplusplus
extern "C"
{
#endif
    extern uint8_t bta_av_co_get_current_codec_id(void);
#ifdef __cplusplus
}
#endif
/* a2dp event handler */
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param);
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);
/* avrc event handler */
static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param);
void volume_set_by_controller(uint8_t avrcp_vol);
static uint32_t m_pkt_cnt = 0;
esp_a2d_audio_state_t m_audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
extern player_t *player;
static pcm_format_t bt_buffer_fmt = {
    .sample_rate = 44100,
    .bit_depth = I2S_BITS_PER_SAMPLE_16BIT,
    .num_channels = 2,
    .buffer_format = PCM_INTERLEAVED};
bool bt_is_playing = false;
static bool bt_paused = false;
static bool s_metadata_pending = false;
static uint8_t s_metadata_received = 0;
static TimerHandle_t s_metadata_timer = NULL;
static volatile bool s_avrc_up = false;
static uint32_t s_sbc_sample_rate = 0;

// Some peers (Windows) omit album/genre, so the full set never completes
static void bt_av_metadata_flush(TimerHandle_t timer)
{
    if (s_metadata_pending)
    {
        s_metadata_pending = false;
        clientPrintHeaders();
    }
}

void bt_pcm_decoder_task(void *pvParameters)
{
    renderer_config_t *renderer_instance;
    renderer_instance = renderer_get();
    player_t *player = pvParameters;
    if (renderer_instance == NULL || player == NULL)
    {
        ESP_LOGE(BT_AV_TAG, "BT PCM decoder started without renderer or player");
        vTaskDelete(NULL);
        return;
    }
    if (!i2s_driver_installed && (!init_i2s() || !i2s_driver_installed))
    {
        ESP_LOGE(BT_AV_TAG, "BT PCM decoder started without an installed I2S driver");
        player->decoder_status = STOPPED;
        vTaskDelete(NULL);
        return;
    }
    renderer_zero_dma_buffer();
    extern bool _reboot;
    extern pcm_format_t bt_buffer_fmt;
    char pcm_buf[1024];
    unsigned chunk_size = sizeof(pcm_buf);
    int64_t stats_start_us = esp_timer_get_time();
    uint32_t consumed_bytes = 0;
    ESP_LOGI(BT_AV_TAG, "Starting BT PCM decoder task");
    if (s_sbc_sample_rate != 0)
    {
        audio_player_set_stream_vbr(AUDIO_STREAM_CODEC_SBC, s_sbc_sample_rate);
    }
    while (player->command != CMD_STOP && !_reboot)
    {
        int64_t now_us = esp_timer_get_time();
        if (now_us - stats_start_us >= 5000000)
        {
            ESP_LOGI(BT_AV_TAG, "BT PCM consumer: bytes=%u fifo=%u",
                     (unsigned)consumed_bytes, spiRamFifoFill());
            consumed_bytes = 0;
            stats_start_us = now_us;
        }
        if (player->command == CMD_PAUSE)
        {
            renderer_zero_dma_buffer();

                 while (player->command == CMD_PAUSE && player->command != CMD_STOP && !_reboot)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (player->command == CMD_STOP || _reboot)
        {
            break;
        }
        // Only read from SPI RAM if we are strictly in active playing mode
        if (spiRamFifoTryRead(pcm_buf, chunk_size) != chunk_size)
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        render_samples(pcm_buf, chunk_size, &bt_buffer_fmt);
        consumed_bytes += chunk_size;
    }
    if (i2s_driver_installed)
    {
        i2s_stop(renderer_instance->i2s_num);
        renderer_zero_dma_buffer();
        i2s_driver_uninstall(renderer_instance->i2s_num);
        i2s_driver_installed = false;
    }
    player->decoder_status = STOPPED;
    ESP_LOGI(BT_AV_TAG, "BT PCM decoder task stopped");
    // if (pcm_buf != NULL)
    // buf_destroy(pcm_buf);
    vTaskDelete(NULL);

    return;
}

#if (defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER) || \
    (defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER)
static bool bt_compressed_sync_fifo_epoch(buffer_t *input, unsigned *fifo_epoch,
                                          const char *codec_name)
{
    unsigned current_epoch = spiRamFifoEpoch();
    if (*fifo_epoch == current_epoch)
    {
        return false;
    }

    size_t discarded_bytes = buf_data_unread(input);
    ESP_LOGI(BT_AV_TAG, "%s FIFO epoch %u -> %u, dropping %u buffered bytes", codec_name,
             *fifo_epoch, current_epoch, (unsigned)discarded_bytes);
    *fifo_epoch = current_epoch;
    input->read_pos = input->base;
    input->fill_pos = input->base;
    input->bytes_consumed = 0;
    return true;
}
#endif

#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
#define BT_LDAC_INPUT_BUFFER_SIZE 8192
#define BT_LDAC_PCM_BUFFER_SIZE (15 * 128 * 2 * sizeof(int16_t))

void bt_ldac_decoder_task(void *pvParameters)
{
    player_t *player = pvParameters;
    renderer_config_t *renderer_instance = renderer_get();
    buffer_t *input = NULL;
    uint8_t *pcm = NULL;
    pcm_format_t pcm_format = {
        .sample_rate = 44100,
        .num_channels = 2,
        .buffer_format = PCM_INTERLEAVED,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .bit_depth = I2S_DATA_BIT_WIDTH_16BIT,
#else
        .bit_depth = I2S_BITS_PER_SAMPLE_16BIT,
#endif
    };
    extern bool _reboot;

    if (player == NULL || renderer_instance == NULL)
    {
        ESP_LOGE(BT_AV_TAG, "LDAC decoder started without renderer or player");
        goto cleanup;
    }

    input = buf_create_dma(BT_LDAC_INPUT_BUFFER_SIZE);
    pcm = malloc(BT_LDAC_PCM_BUFFER_SIZE);
    if (input == NULL || pcm == NULL || !a2dp_ldac_decoder_init_pcm())
    {
        ESP_LOGE(BT_AV_TAG, "LDAC decoder task initialization failed");
        goto cleanup;
    }

    if (!i2s_driver_installed && (!init_i2s() || !i2s_driver_installed))
    {
        ESP_LOGE(BT_AV_TAG, "LDAC decoder started without an installed I2S driver");
        goto cleanup;
    }
    renderer_zero_dma_buffer();
    unsigned fifo_epoch = spiRamFifoEpoch();
    TickType_t stats_tick = xTaskGetTickCount();
    unsigned decoded_packets = 0;
    unsigned consumed_packets = 0;
    unsigned decode_failures = 0;
    unsigned invalid_packets = 0;
    unsigned empty_polls = 0;
    unsigned decode_calls = 0;
    uint32_t total_decode_us = 0;
    uint32_t total_render_us = 0;
    uint32_t total_pcm_bytes = 0;
    uint32_t total_pcm_audio_us = 0;
    uint32_t total_encoded_bytes = 0;
    uint32_t max_decode_us = 0;
    uint32_t max_render_us = 0;
    uint32_t max_cycle_us = 0;
    int64_t stats_start_us = esp_timer_get_time();
    ESP_LOGI(BT_AV_TAG, "Starting compressed LDAC decoder task");

    while (player->command != CMD_STOP && !_reboot)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - stats_tick) >= pdMS_TO_TICKS(5000))
        {
            int64_t stats_now_us = esp_timer_get_time();
            uint32_t wall_us = (uint32_t)(stats_now_us - stats_start_us);
            uint32_t audio_permille = wall_us ? (uint32_t)(((uint64_t)total_pcm_audio_us * 1000U) / wall_us) : 0;
            uint32_t i2s_write_us = renderer_take_i2s_write_time_us();
            ESP_LOGD(BT_AV_TAG,
                     "LDAC 5s: consumed=%u encoded_bytes=%u decoded=%u decode_fail=%u invalid=%u empty_polls=%u avg_decode_us=%u max_decode_us=%u avg_render_us=%u max_render_us=%u max_cycle_us=%u pcm_bytes=%u pcm_audio_us=%u wall_us=%u audio_permille=%u i2s_write_us=%u spi_fifo=%u task_buf=%u",
                     consumed_packets, (unsigned)total_encoded_bytes, decoded_packets,
                     decode_failures, invalid_packets, empty_polls,
                     decode_calls ? total_decode_us / decode_calls : 0,
                     (unsigned)max_decode_us,
                     decoded_packets ? total_render_us / decoded_packets : 0,
                     (unsigned)max_render_us, (unsigned)max_cycle_us,
                     (unsigned)total_pcm_bytes, (unsigned)total_pcm_audio_us, (unsigned)wall_us,
                     (unsigned)audio_permille, (unsigned)i2s_write_us,
                     spiRamFifoFill(), (unsigned)buf_data_unread(input));
            decoded_packets = 0;
            consumed_packets = 0;
            decode_failures = 0;
            invalid_packets = 0;
            empty_polls = 0;
            decode_calls = 0;
            total_decode_us = 0;
            total_render_us = 0;
            total_pcm_bytes = 0;
            total_pcm_audio_us = 0;
            total_encoded_bytes = 0;
            max_decode_us = 0;
            max_render_us = 0;
            max_cycle_us = 0;
            stats_tick = now;
            stats_start_us = stats_now_us;
        }

        if (player->command == CMD_PAUSE)
        {
            renderer_zero_dma_buffer();
                 while (player->command == CMD_PAUSE && player->command != CMD_STOP && !_reboot)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (player->command == CMD_STOP || _reboot)
        {
            break;
        }

        if (bt_compressed_sync_fifo_epoch(input, &fifo_epoch, "LDAC"))
        {
            continue;
        }

        size_t available = buf_data_unread(input);
        if (available < sizeof(uint16_t))
        {
            size_t bytes_read = fill_read_buffer(input);
            if (bt_compressed_sync_fifo_epoch(input, &fifo_epoch, "LDAC"))
            {
                continue;
            }
            if (bytes_read == 0)
            {
                empty_polls++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }

        uint16_t packet_size = ((uint16_t)input->read_pos[0] << 8) | input->read_pos[1];
        if (packet_size <= A2DP_LDAC_MPL_HDR_LEN ||
            packet_size > input->len - sizeof(uint16_t))
        {
            invalid_packets++;
            input->read_pos += sizeof(uint16_t);
            input->bytes_consumed += sizeof(uint16_t);
            continue;
        }

        if (available < packet_size + sizeof(uint16_t))
        {
            size_t bytes_read = fill_read_buffer(input);
            if (bt_compressed_sync_fifo_epoch(input, &fifo_epoch, "LDAC"))
            {
                continue;
            }
            if (bytes_read == 0)
            {
                empty_polls++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }

        if (bt_compressed_sync_fifo_epoch(input, &fifo_epoch, "LDAC"))
        {
            continue;
        }

        const uint8_t *packet = input->read_pos + sizeof(uint16_t);
        input->read_pos += packet_size + sizeof(uint16_t);
        input->bytes_consumed += packet_size + sizeof(uint16_t);
        consumed_packets++;
        total_encoded_bytes += packet_size;

        size_t pcm_len = 0;
        uint32_t sample_rate = pcm_format.sample_rate;
        uint32_t bitrate = 0;
        uint8_t channel_count = pcm_format.num_channels;
        int64_t cycle_start_us = esp_timer_get_time();
        int64_t decode_start_us = cycle_start_us;
        bool decode_ok = a2dp_ldac_decoder_decode_payload(packet, packet_size, pcm,
                                                          BT_LDAC_PCM_BUFFER_SIZE, &pcm_len,
                                                          &sample_rate, &channel_count, &bitrate);
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start_us);
        decode_calls++;
        total_decode_us += decode_us;
        if (decode_us > max_decode_us)
        {
            max_decode_us = decode_us;
        }
        if (decode_ok)
        {
            decoded_packets++;
            pcm_format.sample_rate = sample_rate;
            pcm_format.num_channels = channel_count;
            uint32_t bytes_per_sample = pcm_format.bit_depth / 8;
            uint32_t bytes_per_audio_frame = bytes_per_sample * channel_count;
            if (sample_rate > 0 && bytes_per_audio_frame > 0)
            {
                total_pcm_bytes += (uint32_t)pcm_len;
                uint32_t packet_audio_us = (uint32_t)(((uint64_t)pcm_len * 1000000U) /
                                                      ((uint64_t)sample_rate * bytes_per_audio_frame));
                total_pcm_audio_us += packet_audio_us;
                audio_player_set_stream_info(AUDIO_STREAM_CODEC_LDAC, sample_rate, bitrate);
            }
            int64_t render_start_us = esp_timer_get_time();
            render_samples((char *)pcm, pcm_len, &pcm_format);
            uint32_t render_us = (uint32_t)(esp_timer_get_time() - render_start_us);
            uint32_t cycle_us = (uint32_t)(esp_timer_get_time() - cycle_start_us);
            total_render_us += render_us;
            if (render_us > max_render_us)
            {
                max_render_us = render_us;
            }
            if (cycle_us > max_cycle_us)
            {
                max_cycle_us = cycle_us;
            }
        }
        else
        {
            decode_failures++;
        }
    }

cleanup:
    if (input != NULL)
    {
        buf_destroy(input);
    }
    free(pcm);
    if (renderer_instance != NULL && i2s_driver_installed)
    {
        i2s_stop(renderer_instance->i2s_num);
        renderer_zero_dma_buffer();
    }
    if (player != NULL)
    {
        player->decoder_status = STOPPED;
    }
    ESP_LOGI(BT_AV_TAG, "LDAC decoder task stopped");
    vTaskDelete(NULL);
}
#endif

#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
#define BT_APTX_INPUT_BUFFER_SIZE 8192
#define BT_APTX_PCM_BUFFER_SIZE (15 * 128 * 2 * sizeof(int32_t))

static bool bt_aptx_sync_fifo_epoch(buffer_t *input, unsigned *fifo_epoch)
{
    if (!bt_compressed_sync_fifo_epoch(input, fifo_epoch, "aptX"))
    {
        return false;
    }
    if (!a2dp_aptx_decoder_reset())
    {
        ESP_LOGE(BT_AV_TAG, "aptX decoder reset failed after FIFO epoch change");
    }
    return true;
}

void bt_aptx_decoder_task(void *pvParameters)
{
    player_t *player = pvParameters;
    renderer_config_t *renderer_instance = renderer_get();
    buffer_t *input = NULL;
    uint8_t *pcm = NULL;
    bool decoder_initialized = false;
    pcm_format_t pcm_format = {
        .sample_rate = 48000,
        .num_channels = 2,
        .buffer_format = PCM_INTERLEAVED,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .bit_depth =
#if defined(CONFIG_BT_A2DP_APTX_VARIANT_HD) && CONFIG_BT_A2DP_APTX_VARIANT_HD
            I2S_DATA_BIT_WIDTH_32BIT,
#elif defined(CONFIG_BT_A2DP_APTX_VARIANT_LL) && CONFIG_BT_A2DP_APTX_VARIANT_LL
            I2S_DATA_BIT_WIDTH_32BIT,
#else
            I2S_DATA_BIT_WIDTH_16BIT,
#endif
#else
        .bit_depth =
#if defined(CONFIG_BT_A2DP_APTX_VARIANT_HD) && CONFIG_BT_A2DP_APTX_VARIANT_HD
            I2S_BITS_PER_SAMPLE_32BIT,
#elif defined(CONFIG_BT_A2DP_APTX_VARIANT_LL) && CONFIG_BT_A2DP_APTX_VARIANT_LL
            I2S_BITS_PER_SAMPLE_32BIT,
#else
            I2S_BITS_PER_SAMPLE_16BIT,
#endif
#endif
    };
    extern bool _reboot;

    if (player == NULL || renderer_instance == NULL)
    {
        ESP_LOGE(BT_AV_TAG, "aptX decoder started without renderer or player");
        goto cleanup;
    }

    input = buf_create_dma(BT_APTX_INPUT_BUFFER_SIZE);
    pcm = malloc(BT_APTX_PCM_BUFFER_SIZE);
    if (input == NULL || pcm == NULL || !a2dp_aptx_decoder_init(NULL))
    {
        ESP_LOGE(BT_AV_TAG, "aptX decoder task initialization failed");
        goto cleanup;
    }
    decoder_initialized = true;

    if (!i2s_driver_installed && (!init_i2s() || !i2s_driver_installed))
    {
        ESP_LOGE(BT_AV_TAG, "aptX decoder started without an installed I2S driver");
        goto cleanup;
    }
    renderer_zero_dma_buffer();
    unsigned fifo_epoch = spiRamFifoEpoch();
    unsigned debug_packets = 0;
    TickType_t stats_tick = xTaskGetTickCount();
    unsigned consumed_packets = 0;
    unsigned decoded_packets = 0;
    unsigned decode_failures = 0;
    uint32_t encoded_bytes = 0;
#if defined(CONFIG_BT_A2DP_APTX_VARIANT_HD) && CONFIG_BT_A2DP_APTX_VARIANT_HD
    audio_player_set_stream_info(AUDIO_STREAM_CODEC_APTX, pcm_format.sample_rate,
                                 pcm_format.sample_rate * 2 * 24 / 4);
#else
    audio_player_set_stream_info(AUDIO_STREAM_CODEC_APTX, pcm_format.sample_rate,
                                 pcm_format.sample_rate * 2 * 16 / 4);
#endif
    ESP_LOGI(BT_AV_TAG, "Starting compressed aptX decoder task");

    while (player->command != CMD_STOP && !_reboot)
    {
        if ((xTaskGetTickCount() - stats_tick) >= pdMS_TO_TICKS(5000))
        {
            ESP_LOGD(BT_AV_TAG,
                     "aptX 5s: consumed=%u encoded_bytes=%u decoded=%u decode_fail=%u spi_fifo=%u task_buf=%u",
                     consumed_packets, (unsigned)encoded_bytes, decoded_packets, decode_failures,
                     spiRamFifoFill(), (unsigned)buf_data_unread(input));
            consumed_packets = 0;
            decoded_packets = 0;
            decode_failures = 0;
            encoded_bytes = 0;
            stats_tick = xTaskGetTickCount();
        }
        if (player->command == CMD_PAUSE)
        {
            renderer_zero_dma_buffer();
                 while (player->command == CMD_PAUSE && player->command != CMD_STOP && !_reboot)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (player->command == CMD_STOP || _reboot)
        {
            break;
        }

        if (bt_aptx_sync_fifo_epoch(input, &fifo_epoch))
        {
            continue;
        }

        size_t available = buf_data_unread(input);
        if (available < sizeof(uint16_t))
        {
            size_t bytes_read = fill_read_buffer(input);
            if (bt_aptx_sync_fifo_epoch(input, &fifo_epoch))
            {
                continue;
            }
            if (bytes_read == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }

        uint16_t packet_size = ((uint16_t)input->read_pos[0] << 8) | input->read_pos[1];
        if (packet_size == 0 || packet_size > input->len - sizeof(uint16_t))
        {
            input->read_pos += sizeof(uint16_t);
            input->bytes_consumed += sizeof(uint16_t);
            continue;
        }
        if (available < packet_size + sizeof(uint16_t))
        {
            size_t bytes_read = fill_read_buffer(input);
            if (bt_aptx_sync_fifo_epoch(input, &fifo_epoch))
            {
                continue;
            }
            if (bytes_read == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            continue;
        }
        if (bt_aptx_sync_fifo_epoch(input, &fifo_epoch))
        {
            continue;
        }

        const uint8_t *packet = input->read_pos + sizeof(uint16_t);
        input->read_pos += packet_size + sizeof(uint16_t);
        input->bytes_consumed += packet_size + sizeof(uint16_t);
        consumed_packets++;
        encoded_bytes += packet_size;

        size_t pcm_len = 0;
        bool decode_ok = a2dp_aptx_decoder_decode_payload(packet, packet_size, pcm,
                                                          BT_APTX_PCM_BUFFER_SIZE, &pcm_len);
        if (decode_ok)
        {
            decoded_packets++;
        }
        else
        {
            decode_failures++;
        }
        if (debug_packets < 5)
        {
            ESP_LOGD(BT_AV_TAG, "aptX packet len=%u decode=%d pcm=%u bytes head=%02x:%02x:%02x:%02x",
                     packet_size, decode_ok, (unsigned)pcm_len,
                     packet[0], packet[1], packet[2], packet[3]);
            debug_packets++;
        }
        if (pcm_len > 0)
        {
            render_samples((char *)pcm, pcm_len, &pcm_format);
        }
    }

cleanup:
    if (decoder_initialized)
    {
        a2dp_aptx_decoder_cleanup();
    }
    if (input != NULL)
    {
        buf_destroy(input);
    }
    free(pcm);
    if (renderer_instance != NULL && i2s_driver_installed)
    {
        i2s_stop(renderer_instance->i2s_num);
        renderer_zero_dma_buffer();
    }
    if (player != NULL)
    {
        player->decoder_status = STOPPED;
    }
    ESP_LOGI(BT_AV_TAG, "aptX decoder task stopped");
    vTaskDelete(NULL);
}
#endif

static void bt_av_request_metadata(void)
{
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE |
                        ESP_AVRC_MD_ATTR_ARTIST |
                        ESP_AVRC_MD_ATTR_ALBUM |
                        ESP_AVRC_MD_ATTR_GENRE;
    s_metadata_received = 0;
    s_metadata_pending = true;
    if (esp_avrc_ct_send_metadata_cmd(APP_RC_CT_TL_GET_META_DATA, attr_mask) != ESP_OK)
    {
        s_metadata_pending = false;
        ESP_LOGW(BT_RC_CT_TAG, "Failed to request AVRCP metadata");
    }
}

static TickType_t s_last_flush_tick;
static bool s_track_reached_end;
static bool s_track_end_notification_pending;

static void bt_flush_audio(void)
{
    unsigned queued_bytes = spiRamFifoFill();
    unsigned old_epoch = spiRamFifoEpoch();
    spiRamFifoReset();
    ESP_LOGD(BT_AV_TAG, "BT audio FIFO reset: %u queued bytes, epoch %u -> %u",
             queued_bytes, old_epoch, spiRamFifoEpoch());
    renderer_zero_dma_buffer();
    s_last_flush_tick = xTaskGetTickCount();
}

static void bt_av_new_track(void)
{
    bt_av_request_metadata();

    esp_avrc_ct_send_register_notification_cmd(1,
                                               ESP_AVRC_RN_TRACK_CHANGE, 0);
    if (!s_track_end_notification_pending &&
        esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_REACHED_END, 0) == ESP_OK)
    {
        s_track_end_notification_pending = true;
    }
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id)
    {
    case ESP_AVRC_RN_TRACK_REACHED_END:
        s_track_reached_end = true;
        s_track_end_notification_pending = false;
        ESP_LOGD(BT_AV_TAG, "Track reached natural end");
        if (esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_REACHED_END, 0) == ESP_OK)
        {
            s_track_end_notification_pending = true;
        }
        break;
    case ESP_AVRC_RN_TRACK_CHANGE:
    {
        bool recently_forced = xTaskGetTickCount() - s_last_flush_tick <= pdMS_TO_TICKS(1500);
        ESP_LOGD(BT_AV_TAG, "Track changed: reason=%s, preserving queued audio",
                 recently_forced ? "forced/local" : (s_track_reached_end ? "natural-end" : "unclassified"));
        s_track_reached_end = false;
        clearHeaders();
        bt_av_new_track();
        break;
    }
    }
}
void volume_set_by_controller(uint8_t avrcp_vol)
{
    // Example: map 0x00..0x7F to 0..255
    uint8_t local_vol = (uint8_t)((avrcp_vol * 255) / 0x7F);

    // Set your internal volume variable
    g_device->vol = local_vol;
    setIvol(local_vol);

    // Optionally, update the renderer/DAC immediately
    char volstr[8];
    sprintf(volstr, "%d", local_vol);
    setVolume(volstr);

    ESP_LOGI(BT_RC_TG_TAG, "Volume set by controller: AVRCP=%d, Local=%d", avrcp_vol, local_vol);

    // Notify the phone of the new volume (if needed)
    esp_avrc_rn_param_t rn_param;
    rn_param.volume = avrcp_vol;
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
}
static void bt_send_passthrough_pair(uint8_t key_code);

void bt_pause_resume(void)
{
    // Toggle play/pause depending on current state
    extern esp_a2d_audio_state_t m_audio_state;
    if (m_audio_state == ESP_A2D_AUDIO_STATE_STARTED)
    {
        bt_send_passthrough_pair(ESP_AVRC_PT_CMD_PAUSE);
    }
    else
    {
        bt_send_passthrough_pair(ESP_AVRC_PT_CMD_PLAY);
    }
}

static portMUX_TYPE s_avrc_tl_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_avrc_transaction_label = 5;

static uint8_t bt_next_avrc_transaction_label(void)
{
    portENTER_CRITICAL(&s_avrc_tl_mux);
    uint8_t label = s_avrc_transaction_label;
    s_avrc_transaction_label = s_avrc_transaction_label == 15 ? 5 : s_avrc_transaction_label + 1;
    portEXIT_CRITICAL(&s_avrc_tl_mux);
    return label;
}

static void bt_send_passthrough_pair(uint8_t key_code)
{
    uint8_t press_label = bt_next_avrc_transaction_label();
    uint8_t release_label = bt_next_avrc_transaction_label();
    esp_avrc_ct_send_passthrough_cmd(press_label, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(release_label, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_next_track(void)
{
    bt_flush_audio();
    bt_send_passthrough_pair(ESP_AVRC_PT_CMD_FORWARD);
}

void bt_prev_track(void)
{
    bt_flush_audio();
    bt_send_passthrough_pair(ESP_AVRC_PT_CMD_BACKWARD);
}

/* callback for A2DP sink */
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "a2dp conn_state_cb, state %d", a2d->conn_stat.state);
        if (ESP_A2D_CONNECTION_STATE_DISCONNECTED == a2d->conn_stat.state)
        {
            _bt_conn_state = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
            bt_on_disconnect(a2d->conn_stat.remote_bda);
            // Resetting mid-teardown leaves Windows with a greyed-out Disconnect button
            for (int i = 0; i < 40 && s_avrc_up; i++)
                vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(500));
            bt_full_stack_reset(false); // Reset BT stack if disconnected
            bt_is_playing = false;
        }
        else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING)
        {
            if (!init_i2s())
            {
                ESP_LOGE(BT_AV_TAG, "init_i2s failed");
            }
        }
        else if (ESP_A2D_CONNECTION_STATE_CONNECTED == a2d->conn_stat.state)
        {
            if (!i2s_driver_installed && !init_i2s())
            {
                ESP_LOGE(BT_AV_TAG, "init_i2s failed after A2DP connection");
            }
            bt_on_connect(a2d->conn_stat.remote_bda);

            if (player && player->media_stream)
            {
                // ============================================================================
                // MULTI-CODEC STREAM ROUTING CONTROLS (FDKAAC SELECTOR)
                // ============================================================================
                // We fetch the currently active codec ID directly from the validated geter.
                // 0 = SBC, 2 = AAC (M24), 0xFF = Vendor Specific (aptX/LDAC)
                uint8_t active_codec = bta_av_co_get_current_codec_id();

                if (active_codec == 2) // BTA_AV_CODEC_M24 (True AAC stream negotiated)
                {
                    player->media_stream->content_type = AUDIO_BT_AAC;
                    ESP_LOGI(BT_AV_TAG, "A2DP Stream negotiated as AAC. Launching fdkaac_decoder_task!");
                }
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) || defined(CONFIG_BT_A2DP_APTX_DECODER)
                else if (active_codec == 0xFF) // Vendor Specific (aptX/LDAC)
                {
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
                    if (bta_av_co_get_current_ldac_audio_format(NULL, NULL))
                    {
                        player->media_stream->content_type = AUDIO_BT_LDAC;
                        ESP_LOGI(BT_AV_TAG, "LDAC negotiated; compressed decoder task selected.");
                    }
#endif
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
                    if (bt_is_current_aptx_codec())
                    {
                        player->media_stream->content_type = AUDIO_BT_APTX;
                        ESP_LOGI(BT_AV_TAG, "aptX negotiated; compressed decoder task selected.");
                    }
#endif
                }
#endif

                else // Fallback to PCM for SBC...
                {
                    player->media_stream->content_type = AUDIO_BT_PCM;
                    ESP_LOGI(BT_AV_TAG, "A2DP Stream negotiated as SBC/aptX/LDAC. Launching PCM decoder task!");
                }
                audio_player_start();
            }
            _bt_conn_state = a2d->conn_stat.state; // Store the connection state globally
            break;
        }
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "a2dp audio_state_cb state %d", a2d->audio_stat.state);
        m_audio_state = a2d->audio_stat.state;

        // Fetch the verified active codec ID from your Bluedroid control block
        // 0 = SBC, 2 = AAC, 0xFF = Vendor Proprietary (aptX/LDAC)
        uint8_t active_codec = bta_av_co_get_current_codec_id();

        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state)
        {
            m_pkt_cnt = 0;
            bt_is_playing = true;

            if (player)
            {
                // PURE STATE PERSISTENCE: If the decoder task is already alive and suspended,
                // we simply trigger CMD_RESUME without altering any stream properties!
                if (player->command == CMD_PAUSE && player->decoder_status == RUNNING)
                {
                    player->command = CMD_RESUME;
                    ESP_LOGI(BT_AV_TAG, "Persistent decoder un-paused smoothly via AUDIO_BT state.");
                }
                else
                {
                    // COLD START SEQUENCE: Explicitly inject the genuine codec profile type
                    // into the media_stream container BEFORE calling audio_player_start()!
                    if (player->media_stream)
                    {
                        if (active_codec == 2) // AAC (MPEG-2, 4 AAC)
                        {
                            player->media_stream->content_type = AUDIO_BT_AAC;
                            ESP_LOGI(BT_AV_TAG, "Mapping cold-start pipeline to native AUDIO_BT_AAC track.");
                        }
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
                        else if (active_codec == 0xFF && bta_av_co_get_current_ldac_audio_format(NULL, NULL))
                        {
                            player->media_stream->content_type = AUDIO_BT_LDAC;
                            ESP_LOGI(BT_AV_TAG, "Mapping cold-start pipeline to compressed AUDIO_BT_LDAC track.");
                        }
#endif
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
                        else if (active_codec == 0xFF && bt_is_current_aptx_codec())
                        {
                            player->media_stream->content_type = AUDIO_BT_APTX;
                            ESP_LOGI(BT_AV_TAG, "Mapping cold-start pipeline to compressed AUDIO_BT_APTX track.");
                        }
#endif
                        else // Fallback to PCM for SBC and unhandled codecs
                        {
                            player->media_stream->content_type = AUDIO_BT_PCM;
                            ESP_LOGI(BT_AV_TAG, "Mapping cold-start pipeline to native AUDIO_BT_PCM track.");
                        }
                        player->media_stream->eof = false;
                    }

                    player->command = CMD_START;
                    player->decoder_command = CMD_NONE;
                    audio_player_start(); // This will trigger start_decoder_task with the clean enum mapping
                }
            }

            if (bt_paused)
            {
                bt_av_request_metadata();
                bt_paused = false;
            }
        }
        else if (ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND == a2d->audio_stat.state)
        {
            bt_paused = true;

            // Signal the persistent loop to lock without destroying codec context structures
            if (player && player->decoder_status == RUNNING)
            {
                player->command = CMD_PAUSE;
                ESP_LOGI(BT_AV_TAG, "Active decoder thread successfully locked to persistent standby loop.");
            }

            const char *meta = clientGetHeader()->members.mArr[ICY_TITLE];
            if (!meta)
                meta = "";
            size_t meta_len = strlen(meta);
            const char *paused = " - Paused";
            size_t paused_len = strlen(paused);
            char *paused_buf = malloc(meta_len + paused_len + 1);
            if (paused_buf)
            {
                strcpy(paused_buf, meta);
                strcat(paused_buf, paused);
                clientSaveOneHeader(paused_buf, strlen(paused_buf), ICY_TITLE);
                clientPrintHeaders();
                free(paused_buf);
            }
        }
        else if (ESP_A2D_AUDIO_STATE_STOPPED == a2d->audio_stat.state)
        {
            bt_is_playing = false;
            if (player && player->decoder_status == RUNNING)
            {
                player->command = CMD_PAUSE;
                spiRamFifoReset();
                renderer_zero_dma_buffer();
                ESP_LOGI(BT_AV_TAG, "A2DP stream stopped; decoder retained in standby.");
            }
            else
            {
                audio_player_stop();
                spiRamFifoReset();
            }
            g_device->vol = getIvol();
            ESP_LOGI(BT_AV_TAG, "A2DP audio stream stopped");
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        uint8_t codec_type = a2d->audio_cfg.mcc.type;

        ESP_LOGI(BT_AV_TAG, "A2DP audio_cfg_cb, true codec type: %d", codec_type);

        renderer_config_t *renderer_instance = renderer_get();
        int sample_rate = 44100; // Default baseline fallback

        // ============================================================================
        // ROUTING SYSTEM FOR EACH COMPATIBLE CODEC PROFILE
        // ============================================================================
        if (codec_type == ESP_A2D_MCT_SBC)
        {
            char oct0 = a2d->audio_cfg.mcc.cie.sbc[0];
            uint8_t channels = (oct0 & (1 << 3)) ? 1 : 2;
            if (oct0 & (0x01 << 6))
                sample_rate = 32000;
            else if (oct0 & (0x01 << 5))
                sample_rate = 44100;
            else if (oct0 & (0x01 << 4))
                sample_rate = 48000;

            s_sbc_sample_rate = sample_rate;
            bt_buffer_fmt.sample_rate = sample_rate;
            bt_buffer_fmt.num_channels = channels;
            audio_player_set_stream_vbr(AUDIO_STREAM_CODEC_SBC, sample_rate);
            ESP_LOGI(BT_AV_TAG, "SBC configured, samplerate=%d channels=%u",
                     sample_rate, (unsigned)channels);
        }
#ifdef CONFIG_BT_A2DP_AAC_DECODER
        else if (codec_type == ESP_A2D_MCT_M24) // AAC (0x02)
        {
            // Extract frequency bits from the genuine verified AAC configuration array
            // info[3] is high freq byte, info[4] is low freq byte + channels
            uint8_t aac_high = a2d->audio_cfg.mcc.cie.sbc[1]; // mapping layout matching your bte_av_media_callback offset
            uint8_t aac_low = a2d->audio_cfg.mcc.cie.sbc[2];

            if (aac_high & 0x01)
                sample_rate = 44100;
            else if (aac_low & 0x80)
                sample_rate = 48000;

            ESP_LOGI(BT_AV_TAG, "AAC configured, true sample rate=%d", sample_rate);
        }
#endif
#if (defined(CONFIG_BT_A2DP_APTX_DECODER)) || (defined(CONFIG_BT_A2DP_LDAC_DECODER))
        else if (codec_type == ESP_A2D_MCT_NON_A2DP) // Vendor (0xFF)
        {
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
            if (bt_is_current_aptx_codec())
            {
                sample_rate = 48000; // aptX standard targets 48kHz
                ESP_LOGI(BT_AV_TAG, "aptX Premium configured, sample rate=%d", sample_rate);
            }
#endif
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
            uint32_t ldac_sample_rate = 0;
            if (bta_av_co_get_current_ldac_audio_format(&ldac_sample_rate, NULL))
            {
                sample_rate = (int)ldac_sample_rate;
                audio_player_reset_stream_info(AUDIO_STREAM_CODEC_LDAC, ldac_sample_rate);
                ESP_LOGI(BT_AV_TAG, "LDAC configured, sample rate=%d", sample_rate);
            }
#endif
        }
#endif

        else
        {
            ESP_LOGE(BT_AV_TAG, "Unsupported codec profile bypassed: %d", codec_type);
            break;
        }

        // Apply hardware clock modifications to I2S bus directly based on the winner codec layout
        i2s_set_clk(0, sample_rate, renderer_instance->bit_depth, 2);
        break;
    }
    case ESP_A2D_SEP_REG_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)p_param;
        ESP_LOGI(BT_AV_TAG, "A2DP SEP registration seid=%u state=%u",
                 a2d->a2d_sep_reg_stat.seid, a2d->a2d_sep_reg_stat.reg_state);
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    }
    /* when a2dp init or deinit completed, this event comes */
    case ESP_A2D_PROF_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (ESP_A2D_INIT_SUCCESS == a2d->a2d_prof_stat.init_state)
        {
            ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: Init Complete");
#if !defined(CONFIG_BT_A2DP_AAC_DECODER) || !CONFIG_BT_A2DP_AAC_DECODER
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#endif
        }
        else
        {
            ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: Deinit Complete");
        }
        break;
    }

    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    ESP_LOGI(BT_RC_CT_TAG, "%s event: %d", __func__, event);
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);
    switch (event)
    {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_AV_TAG, "avrc conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (rc->conn_stat.connected)
        {
            ESP_LOGI(BT_RC_CT_TAG, "AVRC connection established");
            int dev_num = esp_bt_gap_get_bond_device_num();
            esp_bd_addr_t *esp_list = malloc(sizeof(esp_bd_addr_t) * dev_num);
            if (esp_list && esp_bt_gap_get_bond_device_list(&dev_num, esp_list) == ESP_OK)
            {
                int esp_index = -1;
                for (int i = 0; i < dev_num; i++)
                {
                    if (memcmp(bda, esp_list[i], 6) == 0)
                    {
                        esp_index = i;
                        break;
                    }
                }
                if (esp_index >= 0)
                {
                    // Store for later update
                    bt_update_bonded_device(esp_index, bda, NULL);
                    // Request remote device name
                    esp_bt_gap_read_remote_name(bda);
                }
            }
            else
            {
                ESP_LOGE(BT_RC_CT_TAG, "Failed to get bonded device list or no devices found");
            }
            if (esp_list)
                free(esp_list);
            clearHeaders();
            bt_av_new_track();
        }
        else
        {
            s_metadata_pending = false;
            s_metadata_received = 0;
        }
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough rsp: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code,
                 rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT:
    {
        ESP_LOGI(BT_AV_TAG, "avrc metadata rsp: attribute id 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);

        uint8_t icy_header_num = 0xFF;
        uint8_t metadata_bit = 0;
        switch (rc->meta_rsp.attr_id)
        {
        case ESP_AVRC_MD_ATTR_TITLE:
            icy_header_num = ICY_TITLE;
            metadata_bit = 1 << 0;
            break;
        case ESP_AVRC_MD_ATTR_ARTIST:
            icy_header_num = ICY_ARTIST;
            metadata_bit = 1 << 1;
            break;
        case ESP_AVRC_MD_ATTR_ALBUM:
            icy_header_num = ICY_ALBUM;
            metadata_bit = 1 << 2;
            break;
        case ESP_AVRC_MD_ATTR_GENRE:
            icy_header_num = ICY_GENRE;
            metadata_bit = 1 << 3;
            break;
        default:
            ESP_LOGW(BT_AV_TAG, "Unhandled AVRCP metadata attr_id: 0x%x", rc->meta_rsp.attr_id);
            break;
        }
        if (icy_header_num != 0xFF && s_metadata_pending)
        {
            clientSaveOneHeader((const char *)rc->meta_rsp.attr_text, strlen((const char *)rc->meta_rsp.attr_text), icy_header_num);
            s_metadata_received |= metadata_bit;
            if (s_metadata_received == 0x0F)
            {
                s_metadata_pending = false;
                if (s_metadata_timer)
                    xTimerStop(s_metadata_timer, 0);
                clientPrintHeaders();
            }
            else
            {
                if (!s_metadata_timer)
                    s_metadata_timer = xTimerCreate("avrc_meta", pdMS_TO_TICKS(400), pdFALSE, NULL, bt_av_metadata_flush);
                if (s_metadata_timer)
                    xTimerReset(s_metadata_timer, 0);
            }
        }
        free(rc->meta_rsp.attr_text);
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        switch (rc->change_ntf.event_id)
        {
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            ESP_LOGI(BT_RC_CT_TAG, "Playback status changed: %d", rc->change_ntf.event_parameter.playback);
            break;
        case ESP_AVRC_RN_TRACK_CHANGE:
            ESP_LOGI(BT_RC_CT_TAG, "Track changed");
            break;
        case ESP_AVRC_RN_TRACK_REACHED_END:
            ESP_LOGI(BT_RC_CT_TAG, "Track reached end");
            break;
        case ESP_AVRC_RN_PLAY_POS_CHANGED:
            ESP_LOGI(BT_RC_CT_TAG, "Playback position changed: %d ms", rc->change_ntf.event_parameter.play_pos);
            break;
        default:
            ESP_LOGI(BT_RC_CT_TAG, "Other notification: %d", rc->change_ntf.event_id);
            break;
        }

        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %x, TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    default:
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}
void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    // Dispatch to your handler
    bt_av_hdl_avrc_tg_evt(event, param);
}
static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    ESP_LOGI(BT_RC_TG_TAG, "%s event: %d", __func__, event);

    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)(p_param);

    switch (event)
    {
    /* when connection state changed, this event comes */
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_TG_TAG, "AVRC conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        break;
    }
    /* when passthrough commanded, this event comes */
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC passthrough cmd: key_code 0x%x, key_state %d", rc->psth_cmd.key_code, rc->psth_cmd.key_state);
        break;
    }
    /* when absolute volume command from remote device set, this event comes */
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC set absolute volume: %d%%", (int)rc->set_abs_vol.volume * 100 / 0x7f);
        volume_set_by_controller(rc->set_abs_vol.volume);
        break;
    }
    /* when notification registered, this event comes */
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC register event notification: %d, param: 0x%x", rc->reg_ntf.event_id, rc->reg_ntf.event_parameter);
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
        {
            //    s_volume_notify = true;
            esp_avrc_rn_param_t rn_param;
            //  rn_param.volume = s_volume;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;
    }
    /* when feature of remote device indicated, this event comes */
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    {
        ESP_LOGI(BT_RC_TG_TAG, "AVRC remote features: %x, CT features: %x", rc->rmt_feats.feat_mask, rc->rmt_feats.ct_feat_flag);
        break;
    }
    /* others */
    default:
        ESP_LOGE(BT_RC_TG_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

/********************************
 * EXTERNAL FUNCTION DEFINITIONS
 *******************************/

void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event)
    {
        ESP_LOGI(BT_AV_TAG, "%s event: %d", __func__, event);
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_PROF_STATE_EVT:
    case ESP_A2D_SEP_REG_STATE_EVT:
    {
        bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "a2dp invalid cb event: %d", event);
        break;
    }
}

/* cb with decoded samples */
void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    extern bool _reboot;
    static int64_t pcm_stats_start_us;
    static uint32_t pcm_input_bytes;
    static uint32_t pcm_accepted_bytes;
    static uint32_t pcm_callbacks;
    static uint8_t pcm_initial_logs;
    if (data && len > 0)
    {
        if (player && player->command != CMD_START && m_audio_state != ESP_A2D_AUDIO_STATE_STARTED && !_reboot)
        {
            // Force restart pipeline
            audio_player_start();
            renderer_start();
            ESP_LOGI(BT_AV_TAG, "Forced pipeline restart on incoming audio data");
        }

        int result = audio_stream_consumer((const char *)data, len);
        if (player && player->media_stream &&
            player->media_stream->content_type == AUDIO_BT_PCM)
        {
            if (pcm_initial_logs < 5)
            {
                ESP_LOGI(BT_AV_TAG,
                         "BT PCM callback #%u: len=%u result=%d command=%d audio_state=%d fifo=%u",
                         (unsigned)(pcm_initial_logs + 1), (unsigned)len, result,
                         player->command, m_audio_state, spiRamFifoFill());
                pcm_initial_logs++;
            }
            int64_t now_us = esp_timer_get_time();
            if (pcm_stats_start_us == 0)
                pcm_stats_start_us = now_us;
            pcm_input_bytes += len;
            if (result == 0)
                pcm_accepted_bytes += len;
            pcm_callbacks++;
            if (now_us - pcm_stats_start_us >= 5000000)
            {
                ESP_LOGI(BT_AV_TAG,
                         "BT PCM ingress: callbacks=%u input=%u accepted=%u fifo=%u result=%d",
                         (unsigned)pcm_callbacks, (unsigned)pcm_input_bytes,
                         (unsigned)pcm_accepted_bytes, spiRamFifoFill(), result);
                pcm_input_bytes = 0;
                pcm_accepted_bytes = 0;
                pcm_callbacks = 0;
                pcm_stats_start_us = now_us;
            }
        }
        if (result == 0 && player != NULL && player->media_stream != NULL &&
            player->media_stream->content_type == AUDIO_BT_LDAC)
        {
            static int64_t window_start_us;
            static uint32_t packet_count;
            static uint32_t byte_count;
            int64_t now_us = esp_timer_get_time();
            if (window_start_us == 0)
            {
                window_start_us = now_us;
            }
            packet_count++;
            byte_count += len;
            if (now_us - window_start_us >= 5000000)
            {
                ESP_LOGD(BT_AV_TAG, "LDAC ingress: packets=%u bytes=%u period_us=%u",
                         packet_count, byte_count, (unsigned)(now_us - window_start_us));
                packet_count = 0;
                byte_count = 0;
                window_start_us = now_us;
            }
        }
    }
}
void bt_app_set_format_params(uint32_t sample_rate, uint8_t num_channels)
{
    extern pcm_format_t bt_buffer_fmt; // Global format for BT stream in KaRadio32

    bt_buffer_fmt.sample_rate = sample_rate;
    bt_buffer_fmt.num_channels = num_channels;
    bt_buffer_fmt.buffer_format = PCM_INTERLEAVED; // Enforce interleaved layout for FDK-AAC

    // Set appropriate baseline bit depth depending on active framework constants
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    bt_buffer_fmt.bit_depth = I2S_DATA_BIT_WIDTH_16BIT;
#else
    bt_buffer_fmt.bit_depth = I2S_BITS_PER_SAMPLE_16BIT; // Triggers Path B (16-bit) in your renderer
#endif
}
void bt_app_set_pcm_format(uint32_t sample_rate, uint8_t num_channels, uint8_t bit_depth)
{
    extern pcm_format_t bt_buffer_fmt; // Global format for BT stream in KaRadio32

    bt_buffer_fmt.sample_rate = sample_rate;
    bt_buffer_fmt.num_channels = num_channels;

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
    if (bit_depth == 32)
    {
        bt_buffer_fmt.bit_depth = I2S_DATA_BIT_WIDTH_32BIT;
    }
    else
    {
        bt_buffer_fmt.bit_depth = I2S_DATA_BIT_WIDTH_16BIT;
    }
#else
    if (bit_depth == 32)
    {
        bt_buffer_fmt.bit_depth = I2S_BITS_PER_SAMPLE_32BIT;
    }
    else
    {
        bt_buffer_fmt.bit_depth = I2S_BITS_PER_SAMPLE_16BIT;
    }
#endif

    bt_buffer_fmt.buffer_format = PCM_LEFT_RIGHT;
}

void bt_app_alloc_meta_buffer(esp_avrc_ct_cb_param_t *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(param);
    uint8_t *attr_text = (uint8_t *)malloc(rc->meta_rsp.attr_length + 1);
    memcpy(attr_text, rc->meta_rsp.attr_text, rc->meta_rsp.attr_length);
    attr_text[rc->meta_rsp.attr_length] = 0;
    rc->meta_rsp.attr_text = attr_text;
    ESP_LOGI(BT_RC_CT_TAG, "Allocated metadata buffer: %s", rc->meta_rsp.attr_text);
}
void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(BT_RC_CT_TAG, "%s event: %d", __func__, event);
    switch (event)
    {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        bt_app_alloc_meta_buffer(param);
        // Dispatch to handler after allocation
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_avrc_up = param->conn_stat.connected;
        if (param->conn_stat.connected)
        {
            bt_on_connect(param->conn_stat.remote_bda);
        }
        else
        {
            bt_on_disconnect(param->conn_stat.remote_bda);
        }
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    {
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "avrc invalid cb event: %d", event);
        break;
    }
}
#endif
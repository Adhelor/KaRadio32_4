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
    //  buffer might contain noise
    i2s_zero_dma_buffer(renderer_instance->i2s_num);
    extern bool _reboot;
    extern pcm_format_t bt_buffer_fmt;
    char pcm_buf[1024];
    unsigned chunk_size = sizeof(pcm_buf);
    ESP_LOGI(BT_AV_TAG, "Starting BT PCM decoder task");
    while (player->command != CMD_STOP && !_reboot)
    {
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
        spiRamFifoRead(pcm_buf, sizeof(pcm_buf));
        render_samples(pcm_buf, chunk_size, &bt_buffer_fmt);
    }
    i2s_stop(renderer_instance->i2s_num);
    i2s_zero_dma_buffer(renderer_instance->i2s_num);
    renderer_zero_dma_buffer();
    if (i2s_driver_installed)
    {
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

static void bt_flush_audio(void)
{
    spiRamFifoReset();
    renderer_zero_dma_buffer();
    s_last_flush_tick = xTaskGetTickCount();
}

static void bt_av_new_track(void)
{
    bt_av_request_metadata();

    esp_avrc_ct_send_register_notification_cmd(1,
                                               ESP_AVRC_RN_TRACK_CHANGE, 0);
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id)
    {
    case ESP_AVRC_RN_TRACK_CHANGE:
        // skip if a sink-initiated flush just happened, it would eat the new track's start
        if (xTaskGetTickCount() - s_last_flush_tick > pdMS_TO_TICKS(1500))
        {
            bt_flush_audio();
        }
        clearHeaders();
        bt_av_new_track();
        break;
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
void bt_pause_resume(void)
{
    // Toggle play/pause depending on current state
    extern esp_a2d_audio_state_t m_audio_state;
    if (m_audio_state == ESP_A2D_AUDIO_STATE_STARTED)
    {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
    else
    {
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
}

void bt_next_track(void)
{
    bt_flush_audio();
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_prev_track(void)
{
    bt_flush_audio();
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
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
            g_bt_conn_state = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
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
                    extern uint8_t g_winning_codec_index;
#if (defined(CONFIG_BT_A2DP_LDAC_DECODER))
                    if (g_winning_codec_index == 6 || g_winning_codec_index == 7) // LDAC
                    {
                        player->media_stream->content_type = AUDIO_BT_PCM;
                        extern void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
                        extern bool a2dp_ldac_decoder_init(void (*decode_callback)(const uint8_t *data, uint32_t len));

                        if (a2dp_ldac_decoder_init(bt_app_a2d_data_cb))
                        {
                            ESP_LOGW(BT_AV_TAG, "LDAC Engine safely pre-allocated and initialized at connection layer.");
                        }
                        else
                        {
                            ESP_LOGE(BT_AV_TAG, "CRITICAL: LDAC Engine pre-allocation FAILED!");
                        }
                    }
#endif
#if (defined(CONFIG_BT_A2DP_APTX_DECODER))
                    if (g_winning_codec_index == 4 || g_winning_codec_index == 5)
                    {
                        player->media_stream->content_type = AUDIO_BT_PCM;

                        /* Forward declaration for your genuine aptX allocation initializer if present */
                        extern bool a2dp_aptx_decoder_init(void);

                        /* Secure pre-allocation window for CSR/Qualcomm math structures */

                        if (a2dp_aptx_decoder_init())
                        {
                            ESP_LOGW(BT_AV_TAG, "aptX Engine successfully pre-allocated at connection layer.");
                        }
                        else
                        {
                            ESP_LOGE(BT_AV_TAG, "CRITICAL: aptX Engine pre-allocation FAILED!");
                        }
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
            g_bt_conn_state = a2d->conn_stat.state; // Store the connection state globally
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
                        else // Fallback to PCM for SBC, aptX, and LDAC profiles
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

            // Force a real volume re-apply to the renderer, the pipeline was just (re)created
            setVolumei(g_device->vol);
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
            if (oct0 & (0x01 << 6))
                sample_rate = 32000;
            else if (oct0 & (0x01 << 5))
                sample_rate = 44100;
            else if (oct0 & (0x01 << 4))
                sample_rate = 48000;

            ESP_LOGI(BT_AV_TAG, "SBC configured, samplerate=%d", sample_rate);
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
            extern uint8_t g_winning_codec_index;

            // Differentiate inside the Vendor block using your global index layout
            if (g_winning_codec_index == 4 || g_winning_codec_index == 5) // aptX
            {
                sample_rate = 48000; // aptX standard targets 48kHz
                ESP_LOGI(BT_AV_TAG, "aptX Premium configured, sample rate=%d", sample_rate);
            }
            else if (g_winning_codec_index == 6 || g_winning_codec_index == 7) // LDAC
            {
                sample_rate = 96000; // LDAC High-Res targets 96kHz natively
                ESP_LOGI(BT_AV_TAG, "LDAC Ultra-HD configured, sample rate=%d", sample_rate);
            }
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
    if (data && len > 0)
    {
        if (player && player->command != CMD_START && m_audio_state != ESP_A2D_AUDIO_STATE_STARTED && !_reboot) // force restart if windows is making shit
        {
            // Force restart pipeline
            audio_player_start();
            renderer_start();
            ESP_LOGI(BT_AV_TAG, "Forced pipeline restart on incoming audio data");
        }

        audio_stream_consumer((const char *)data, len);
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
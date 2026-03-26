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

void bt_pcm_decoder_task(void *pvParameters)
{
    renderer_config_t *renderer_instance;
    renderer_instance = renderer_get();
    player_t *player = pvParameters;
    extern bool _reboot;
    char pcm_buf[1024];
    unsigned chunk_size = sizeof(pcm_buf);
    ESP_LOGI(BT_AV_TAG, "Starting BT PCM decoder task");
    while (player->command != CMD_STOP && !_reboot)
    {
        spiRamFifoRead(pcm_buf, sizeof(pcm_buf));
        render_samples(pcm_buf, chunk_size, &bt_buffer_fmt);
    }
    i2s_stop(renderer_instance->i2s_num);
    i2s_zero_dma_buffer(renderer_instance->i2s_num);
    renderer_zero_dma_buffer();
    if (pcm_buf != NULL)
        // buf_destroy(pcm_buf);
    vTaskDelete(NULL);
    ESP_LOGI(BT_AV_TAG, "BT PCM decoder task stopped");
}

static void bt_av_new_track()
{
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE |
                        ESP_AVRC_MD_ATTR_ARTIST |
                        ESP_AVRC_MD_ATTR_ALBUM |
                        ESP_AVRC_MD_ATTR_GENRE;
    esp_avrc_ct_send_metadata_cmd(APP_RC_CT_TL_GET_META_DATA, attr_mask);

    esp_avrc_ct_send_register_notification_cmd(1,
                                               ESP_AVRC_RN_TRACK_CHANGE, 0);
}

static void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id)
    {
    case ESP_AVRC_RN_TRACK_CHANGE:
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
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void bt_prev_track(void)
{
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
            bt_on_disconnect(a2d->conn_stat.remote_bda);
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
            audio_player_start();
            if (player && player->media_stream)
                player->media_stream->content_type = AUDIO_BT_PCM;
        }
        g_bt_conn_state = a2d->conn_stat.state; // Store the connection state globally
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "a2dp audio_state_cb state %d", a2d->audio_stat.state);
        m_audio_state = a2d->audio_stat.state;
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state)
        {
            m_pkt_cnt = 0;
            if (player && player->media_stream)
            {
                player->media_stream->content_type = AUDIO_BT_PCM;
            }
            player->command = CMD_START;
            player->decoder_command = CMD_NONE;
            player->media_stream->eof = false;
            audio_player_start(); // <-- This will start the decoder task if needed
            bt_av_new_track();
            setIvol(g_device->vol);
            bt_is_playing = true;
            if (bt_paused) {
            bt_av_new_track(); // Request metadata from host again
            bt_paused = false;
        }
        }
        else if (ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND == a2d->audio_stat.state)
        {
            // bt_is_playing = false;
            bt_paused = true;
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
            audio_player_stop();
            spiRamFifoReset();
            ESP_LOGI(BT_AV_TAG, "A2DP audio stream stopped");
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        ESP_LOGI(BT_AV_TAG, "a2dp audio_cfg_cb , codec type %d", a2d->audio_cfg.mcc.type);
        renderer_config_t *renderer_instance;
        renderer_instance = renderer_get();
        // for now only SBC stream is supported
        if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC)
        {
            int sample_rate = 16000;
            char oct0 = a2d->audio_cfg.mcc.cie.sbc[0];
            if (oct0 & (0x01 << 6))
            {
                sample_rate = 32000;
            }
            else if (oct0 & (0x01 << 5))
            {
                sample_rate = 44100;
            }
            else if (oct0 & (0x01 << 4))
            {
                sample_rate = 48000;
            }
            i2s_set_clk(0, sample_rate, renderer_instance->bit_depth, 2);
            ESP_LOGI(BT_AV_TAG, "configure audio player %x-%x-%x-%x\n",
                     a2d->audio_cfg.mcc.cie.sbc[0],
                     a2d->audio_cfg.mcc.cie.sbc[1],
                     a2d->audio_cfg.mcc.cie.sbc[2],
                     a2d->audio_cfg.mcc.cie.sbc[3]);
            ESP_LOGI(BT_AV_TAG, "audio player configured, samplerate=%d", sample_rate);
        }
        break;
    }
    /* when a2dp init or deinit completed, this event comes */
    case ESP_A2D_PROF_STATE_EVT:
    {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (ESP_A2D_INIT_SUCCESS == a2d->a2d_prof_stat.init_state)
        {
            ESP_LOGI(BT_AV_TAG, "A2DP PROF STATE: Init Complete");
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
             // Start a new track on connection
            if (esp_list)
                free(esp_list);
            bt_av_new_track();
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
        switch (rc->meta_rsp.attr_id)
        {
        case ESP_AVRC_MD_ATTR_TITLE:
            icy_header_num = ICY_TITLE;
            // kprintf("##CLI.META#: %s \n", (const char *)rc->meta_rsp.attr_text);
            // addonParse("##CLI.META#: %s \n", (const char *)rc->meta_rsp.attr_text);
            break;
        case ESP_AVRC_MD_ATTR_ARTIST:
            icy_header_num = ICY_ARTIST;
            // kprintf("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            // addonParse("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            break;
        case ESP_AVRC_MD_ATTR_ALBUM:
            icy_header_num = ICY_ALBUM;
            // kprintf("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            // addonParse("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            break;
        case ESP_AVRC_MD_ATTR_GENRE:
            icy_header_num = ICY_GENRE;
            // kprintf("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            // addonParse("##CLI.ICY%d#: %s\n", icy_header_num, (const char *)rc->meta_rsp.attr_text);
            break;
        default:
            ESP_LOGW(BT_AV_TAG, "Unhandled AVRCP metadata attr_id: 0x%x", rc->meta_rsp.attr_id);
            break;
        }
        if (icy_header_num != 0xFF)
        {
            // Always update the header, even if empty, to match webradio logic
            clientSaveOneHeader((const char *)rc->meta_rsp.attr_text, strlen((const char *)rc->meta_rsp.attr_text), icy_header_num);
        }
        free(rc->meta_rsp.attr_text);

        // After all metadata responses for a track, print all headers (simulate webradio)
        clientPrintHeaders();
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
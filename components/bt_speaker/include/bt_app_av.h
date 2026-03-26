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

#ifndef __BT_APP_AV_H__
#define __BT_APP_AV_H__

#include <stdint.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "spiram_fifo.h"
#include "common_buffer.h"
#include "audio_renderer.h"
#include "audio_player.h"
#include "bt_app_core.h"

#define MAX_FRAME_SIZE (2889)


#define BT_AV_TAG               "BT_AV"
#define BT_RC_TG_TAG    "RC_TG"
#define BT_RC_CT_TAG    "RC_CT"

#define ICY_TITLE 9 
#define ICY_NAME 1
#define ICY_ALBUM 4
#define ICY_GENRE 3
#define ICY_ARTIST 0 
#define ICY_DESCRIPTION 2
/**
 * @brief     callback function for A2DP sink
 */
void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/**
 * @brief     callback function for A2DP sink audio data stream
 */
void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);

/**
 * @brief     callback function for AVRCP controller
 */
void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

void bt_next_track(void);
void bt_prev_track(void);
void bt_pause_resume(void);
void bt_pcm_decoder_task(void *param);
extern esp_a2d_audio_state_t m_audio_state;
#endif /* __BT_APP_AV_H__*/
#endif // CONFIG_BT_SPEAKER_MODE

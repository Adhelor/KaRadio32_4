/*
 * a2dp.h
 *
 *  Created on: 07.05.2017
 *      Author: michaelboeckling
 */
#include "sdkconfig.h"
#if CONFIG_BT_SPEAKER_MODE

#ifndef _INCLUDE_BT_SPEAKER_H_
#define _INCLUDE_BT_SPEAKER_H_

#include "common_component.h"
#include "audio_renderer.h"
#include "esp_a2dp_api.h"
#define BT_INDEX_MASK 0x0F
#define BT_OFFSET_MASK 0xF0
#define BT_OFFSET_SHIFT 4

#define BT_SINDEX_ENCODE(first, offset) (((offset) << BT_OFFSET_SHIFT) | ((first) & BT_INDEX_MASK))
#define BT_SINDEX_FIRST(idx) ((idx) & BT_INDEX_MASK)
#define BT_SINDEX_SECOND(idx) (((idx) & BT_OFFSET_MASK) ? (BT_SINDEX_FIRST(idx) + ((idx) >> BT_OFFSET_SHIFT)) : -1)


#define CONFIG_BT_NAME "KaRadio Speaker"
extern esp_a2d_connection_state_t g_bt_conn_state;
void bt_speaker_start(renderer_config_t *renderer_config);
void bt_full_stack_reset(bool force);
void bt_list_bonded_devices(void); 
void bt_remove_all_bonded_devices(void);
void bt_update_bonded_device(int esp_index, const uint8_t *mac, const char *name);
void bt_on_connect(const uint8_t *mac);
void bt_on_disconnect(const uint8_t *mac);

#endif /* _INCLUDE_BT_SPEAKER_H_ */
#endif // CONFIG_BT_SPEAKER_MODE

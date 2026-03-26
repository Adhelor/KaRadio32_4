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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt_defs.h"
#include "bt_speaker.h"
#include "esp_bt.h"
#include "bt_app_core.h"
#include "bt_app_av.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "eeprom.h"
#include "gpio.h"
#include "addon.h"
#include "audio_renderer.h"
#include "esp_bt_defs.h"
#include "eeprom.h"
#include "interface.h"
#include "webclient.h"

#define TAG "BT_SPEAKER"
// extern void clearHeaders(void);
uint8_t bt_sindex = 0xFF; // source device index - 0xFF = not connected /
static renderer_config_t *ren_conf = NULL;
static int bt_list_pending = 0;
esp_a2d_connection_state_t g_bt_conn_state = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
/* event for handler "bt_av_hdl_stack_up */
enum
{
    BT_APP_EVT_STACK_UP = 0,
};
extern void latin_string_swap_task(void *pvParameters);
/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

// Helper: find index of MAC in list, return -1 if not found
static int bt_find_mac_index(const bt_bonded_list_t *list, const uint8_t *mac)
{
    for (int i = 0; i < list->count; ++i)
    {
        if (memcmp(list->devices[i].mac, mac, 6) == 0)
            return i;
    }
    return -1;
}
// Call this on new connection (with MAC of connecting device)
void bt_on_connect(const uint8_t *mac)
{
    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);
    int idx = bt_find_mac_index(&list, mac);
    if (idx < 0)
        return; // Not found, should not happen

    if (bt_sindex == 0xFF)
    {
        // First connection
        bt_sindex = BT_SINDEX_ENCODE(idx, 0);
        clientSetName(list.devices[idx].name, idx);
        clientSaveOneHeader(" Connected - Ready for stream", 30, ICY_TITLE);
        clientPrintHeaders();
    }
    else if (BT_SINDEX_SECOND(bt_sindex) == -1 && BT_SINDEX_FIRST(bt_sindex) != idx)
    {
        // Second connection, only if not already connected
        int first = BT_SINDEX_FIRST(bt_sindex);
        int offset = (idx > first) ? (idx - first) : (idx + list.count - first);
        bt_sindex = BT_SINDEX_ENCODE(first, offset);
    }
    // else: already two connections or duplicate, do not update
}
// Call this on disconnect (with MAC of disconnecting device)
void bt_on_disconnect(const uint8_t *mac)
{
    if (bt_sindex == 0xFF)
        return; // No connection

    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);
    int idx = bt_find_mac_index(&list, mac);
    if (idx < 0)
        return; // Not in list

    int first = BT_SINDEX_FIRST(bt_sindex);
    int second = BT_SINDEX_SECOND(bt_sindex);

    if (idx == first && second != -1)
    {
        // First device disconnected, second becomes first
        bt_sindex = BT_SINDEX_ENCODE(second, 0);
        clientSetName(list.devices[second].name, second);
    }
    else if (idx == second)
    {
        // Second device disconnected, keep first
        bt_sindex = BT_SINDEX_ENCODE(first, 0);
    }
    else if (idx == first && second == -1)
    {
        // Only one device, now disconnected
        bt_sindex = 0xFF;
        clientSetName(g_device->BTname, 0); // Reset to default name
        char pinbuf[50];
        snprintf(pinbuf, sizeof(pinbuf), "Waiting for connection - PIN: %s", g_device->BTpass);
        clientSaveOneHeader(pinbuf, strlen(pinbuf), ICY_TITLE);
        clientSaveOneHeader("",1,ICY_ARTIST);
        clientSaveOneHeader("",1,ICY_ALBUM);
        clientPrintHeaders();
    }
    else
    {
        // Not connected or unknown device, do nothing
        return;
    }
    // else: ignore (should not happen)
}

void bt_list_bonded_devices(void)
{
    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);
    kprintf("=============================================================================\n");
    kprintf("   #  Name                          |           MAC Address                  \n");
    kprintf("=============================================================================\n");
    if (list.count == 0)
    {
        kprintf("No bonded devices stored.\n");
        kprintf("##BT.LIST#: DONE\n");
        return;
    }
    for (int i = 0; i < list.count; i++)
    {
        kprintf("%3d: %-28.28s | %02x:%02x:%02x:%02x:%02x:%02x\n",
                i + 1,
                list.devices[i].name,
                list.devices[i].mac[0], list.devices[i].mac[1], list.devices[i].mac[2],
                list.devices[i].mac[3], list.devices[i].mac[4], list.devices[i].mac[5]);
    }
    kprintf("##BT.LIST#: DONE\n");

    // Only compare lists in BT mode
    if (!_isRadio)
    {
        int esp_count = esp_bt_gap_get_bond_device_num();
        esp_bd_addr_t *esp_list = NULL;
        bool match = true;

        if (esp_count == list.count && esp_count > 0)
        {
            esp_list = malloc(sizeof(esp_bd_addr_t) * esp_count);
            if (esp_list && esp_bt_gap_get_bond_device_list(&esp_count, esp_list) == ESP_OK)
            {
                for (int i = 0; i < esp_count; i++)
                {
                    if (memcmp(list.devices[i].mac, esp_list[esp_count - 1 - i], 6) != 0)
                    {
                        match = false;
                        kprintf("Mismatch at index %d: Local [%02x:%02x:%02x:%02x:%02x:%02x] vs ESP32 [%02x:%02x:%02x:%02x:%02x:%02x]\n",
                                i,
                                list.devices[i].mac[0], list.devices[i].mac[1], list.devices[i].mac[2],
                                list.devices[i].mac[3], list.devices[i].mac[4], list.devices[i].mac[5],
                                esp_list[esp_count - 1 - i][0], esp_list[esp_count - 1 - i][1], esp_list[esp_count - 1 - i][2],
                                esp_list[esp_count - 1 - i][3], esp_list[esp_count - 1 - i][4], esp_list[esp_count - 1 - i][5]);
                    }
                }
            }
            else
            {
                match = false;
            }
            if (esp_list)
                free(esp_list);

            if (match)
                kprintf("##BT.LIST#: Local and ESP32 bonded device lists MATCH.\n");
            else
                kprintf("##BT.LIST#: Local and ESP32 bonded device lists DO NOT match.\n");
        }
        else if (esp_count == 0)
        {
            kprintf("##BT.LIST#: No bonded devices found in ESP32.\n");
        } 
    }
}
void bt_save_bonded_device(const uint8_t *mac, const char *name)
{
    ESP_LOGI(TAG, "bt_save_bonded_device %s, %s", mac, name);
    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);
    // Check if already present
    ESP_LOGI(TAG, "bt_save_bonded_device %s, %s", mac, name);
    for (int i = 0; i < list.count; ++i)
    {
        if (memcmp(list.devices[i].mac, mac, 6) == 0)
        {
            strncpy(list.devices[i].name, name, BT_BOND_NAME_LEN);
            list.devices[i].name[BT_BOND_NAME_LEN] = 0;
            ESP_LOGI(TAG, "Updating bonded device: %02x:%02x:%02x:%02x:%02x:%02x name: '%s'",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], list.devices[i].name);
            gpio_set_bt_bonded_list(&list);
            return;
        }
    }
    if (list.count < MAX_BT_BONDS)
    {
        memcpy(list.devices[list.count].mac, mac, 6);
        strncpy(list.devices[list.count].name, name, BT_BOND_NAME_LEN);
        list.devices[list.count].name[BT_BOND_NAME_LEN] = 0;
        ESP_LOGI(TAG, "Adding bonded device: %02x:%02x:%02x:%02x:%02x:%02x name: '%s'",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], list.devices[list.count].name);
        list.count++;
        gpio_set_bt_bonded_list(&list);
    }
}
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
    switch (event)
    {
    case BT_APP_EVT_STACK_UP:
    {
        /* set up device name */
        // char *dev_name = "ESP_SPEAKER";
        char *dev_name = g_device->BTname;
        if (dev_name == NULL || strlen(dev_name) == 0)
        {
            dev_name = CONFIG_BT_NAME; // default name from Kconfig
            strcpy(g_device->BTname, dev_name);
            saveDeviceSettings(g_device); // save default name
        }
        esp_bt_dev_set_device_name(dev_name);

        /* initialize AVRCP controller */
        assert(esp_avrc_ct_init() == ESP_OK);
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
        esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

        /* initialize A2DP sink */
        esp_a2d_sink_init();

        /* set discoverable and connectable mode, wait to be connected */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(BT_AV_TAG, "Device name: %s [%02x:%02x:%02x:%02x:%02x:%02x]",
                     param->read_rmt_name.rmt_name,
                     param->read_rmt_name.bda[0], param->read_rmt_name.bda[1], param->read_rmt_name.bda[2],
                     param->read_rmt_name.bda[3], param->read_rmt_name.bda[4], param->read_rmt_name.bda[5]);
            bt_update_bonded_device(-1, NULL, (const char *)param->read_rmt_name.rmt_name);
        }
        else
        {
            ESP_LOGI(BT_AV_TAG, "Device name: [unknown] [%02x:%02x:%02x:%02x:%02x:%02x]",
                     param->read_rmt_name.bda[0], param->read_rmt_name.bda[1], param->read_rmt_name.bda[2],
                     param->read_rmt_name.bda[3], param->read_rmt_name.bda[4], param->read_rmt_name.bda[5]);
        }
        bt_list_pending--;
        break;
    default:
        break;
    }
}

void bt_speaker_start(renderer_config_t *renderer_config)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(BT_AV_TAG, "BT config: mode=%d, ble_max_conn=%d, bt_max_acl_conn=%d",
             bt_cfg.mode, bt_cfg.ble_max_conn, bt_cfg.bt_max_acl_conn);
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed\n", __func__);
        ESP_LOGW(BT_AV_TAG, "Error initializing BT controller: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(BT_AV_TAG, "larggest free block %d bytes", esp_get_free_heap_size());
    // esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    esp_err_t err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK)
    {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed, err=0x%x (%s)", __func__, err, esp_err_to_name(err));
        ESP_LOGE(BT_AV_TAG, "Controller status: %d", esp_bt_controller_get_status());
        return;
    }

    if (esp_bluedroid_init() != ESP_OK)
    {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed\n", __func__);
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK)
    {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed\n", __func__);
        return;
    }

    /* init renderer */
    renderer_init(renderer_config);
    ren_conf = renderer_config; // Store the renderer config globally
    spiRamFifoInit();
    /* create application task */
    /* set default parameters for Legacy Pairing (use fixed pin code 1234) if no pin stored */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    if (!(g_device->BTpass == NULL || strlen(g_device->BTpass) == 0))
    {
        for (int i = 0; i < strlen(g_device->BTpass); i++)
            pin_code[i] = g_device->BTpass[i];

        esp_bt_gap_set_pin(pin_type, strlen(g_device->BTpass), pin_code);
    }
    else
    {
        pin_code[0] = '1';
        pin_code[1] = '2';
        pin_code[2] = '3';
        pin_code[3] = '4';
        esp_bt_gap_set_pin(pin_type, 4, pin_code);
    }
    esp_bt_gap_register_callback(bt_gap_cb);
    bt_app_task_start_up();

    // Set BT Name and Pass in ICY headers
    clientSetName(g_device->BTname, 0); // Reset to device name
    char pinbuf[50];
    snprintf(pinbuf, sizeof(pinbuf), "Waiting for connection - PIN: %s", g_device->BTpass);
    clientSaveOneHeader(pinbuf, strlen(pinbuf), ICY_TITLE);
    clientSaveOneHeader("",1,ICY_ARTIST); // Clear artist header
    clientSaveOneHeader("",1,ICY_ALBUM); // Clear album header
    clientPrintHeaders(); // and print them
   
    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);
    int esp_count = esp_bt_gap_get_bond_device_num();

    if (list.count == 0 && esp_count > 0)
    {
        ESP_LOGI(BT_AV_TAG, "Local bonded list is empty - Clearing ESP32 bonded devices...");
        bt_remove_all_bonded_devices();
    }
    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);
}
void bt_full_stack_reset(bool force)
{
    audio_player_stop();
    spiRamFifoReset();

    // Check if any BT connection remains
    int connected = esp_bt_gap_get_bond_device_num(); // Number of bonded devices
    bool bt_connected = false;
    esp_a2d_connection_state_t conn_state = g_bt_conn_state;

    if (conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        bt_connected = true;

    // Update bt_sindex according to connection state
    if (!bt_connected || force)
    {
        // No device connected or forced reset: clear index
        bt_sindex = 0xFF;
    }
    else
    {
        // At least one device is still connected
        // Try to keep the correct index for the remaining device
        bt_bonded_list_t list;
        gpio_get_bt_bonded_list(&list);

        // Check which device is still connected
        bool bt_connected = (g_bt_conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED);
        // int remaining_idx = -1;
        // actually dev_num can be 0 or 1 so if something is still connected
        if (bt_connected && bt_sindex != 0xFF)
        {
            int idx1 = BT_SINDEX_FIRST(bt_sindex);
            int idx2 = BT_SINDEX_SECOND(bt_sindex);
            if (idx1 >= 0 && idx1 < list.count)
            {
                bt_sindex = BT_SINDEX_ENCODE(idx1, 0);
            }
            else if (idx2 != -1 && idx2 < list.count)
            {
                bt_sindex = BT_SINDEX_ENCODE(idx2, 0);
            }
            else
            {
                bt_sindex = 0xFF;
            }
        }
    }

    ESP_LOGI(BT_AV_TAG, "BT stack reset: bonded=%d, connected=%d, force=%d, bt_sindex=0x%02X", connected, bt_connected, force, bt_sindex);

    // If no host is connected or force is true, fully reset BT controller
    if (!bt_connected || force)
    {
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
            esp_bt_controller_disable();
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
            esp_bt_controller_deinit();
        // BT is down we can clear other resources before reinitializing
        // Destroy renderer if allocated
        renderer_destroy();

        // Free player and its media_stream if allocated
        extern player_t *player;
        if (player)
        {
            if (player->media_stream)
            {
                free(player->media_stream);
                player->media_stream = NULL;
            }
            free(player);
            player = NULL;
        }

        // Uninstall I2S driver if installed
        i2s_driver_uninstall(I2S_NUM_0);

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) == ESP_OK)
        {
            esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
            ESP_LOGI(BT_AV_TAG, "BT controller reinitialized");
        }
        else
        {
            ESP_LOGE(BT_AV_TAG, "BT controller init failed");
        }
        BTstart(); // Restart BT stack and player/renderer
        ESP_LOGI(BT_AV_TAG, "Full BT stack and pipeline reset");
    }
    else
    {
        ESP_LOGI(BT_AV_TAG, "Another host is still connected, skipping full BT controller reset");
        audio_player_start();
    }
}
void bt_remove_all_bonded_devices(void)
{
    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num == 0)
    {
        ESP_LOGI(BT_AV_TAG, "No bonded devices to remove.");
        gpio_set_bt_bonded_list(NULL); // Clear stored bonded devices
        bt_bonded_list_t empty = {0};
        gpio_set_bt_bonded_list(&empty);
        return;
    }

    esp_bd_addr_t *dev_list = malloc(sizeof(esp_bd_addr_t) * dev_num);
    if (!dev_list)
    {
        ESP_LOGE(BT_AV_TAG, "Failed to allocate memory for bonded device list.");
        return;
    }

    if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK)
    {
        for (int i = 0; i < dev_num; i++)
        {
            esp_bt_gap_remove_bond_device(dev_list[i]);
            uint8_t *addr = dev_list[i];
            ESP_LOGI(BT_AV_TAG, "Removed bonded device: %02x:%02x:%02x:%02x:%02x:%02x",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        }
        gpio_set_bt_bonded_list(NULL); // Clear stored bonded devices
        bt_bonded_list_t empty = {0};
        gpio_set_bt_bonded_list(&empty);
    }
    else
    {
        ESP_LOGE(BT_AV_TAG, "Failed to get bonded device list.");
    }
    free(dev_list);
}

// static int last_esp_index = -1;
// static uint8_t last_mac[6] = {0};
// static bool mac_stored = false;

void bt_update_bonded_device(int esp_index, const uint8_t *mac, const char *name)
{
    bt_bonded_list_t list;
    gpio_get_bt_bonded_list(&list);

    static int pending_idx = -1;
    static uint8_t pending_mac[6] = {0};
    static bool mac_stored = false;

    if (mac && !name)
    {
        // First call: store index and MAC, set index if found or add new with MAC as name
        pending_idx = esp_index;
        memcpy(pending_mac, mac, 6);
        mac_stored = true;

        int found = -1;
        for (int i = 0; i < list.count; i++)
        {
            if (memcmp(list.devices[i].mac, mac, 6) == 0)
            {
                found = i;
                break;
            }
        } 
        ESP_LOGI(TAG, "bt_update_bonded_device: found=%d, count=%d", found, list.count);
        if (found == -1 && list.count < MAX_BT_BONDS)
        {
            found = list.count;
            memcpy(list.devices[found].mac, mac, 6);
            snprintf(list.devices[found].name, BT_BOND_NAME_LEN + 1,
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            list.count++;
            bt_save_bonded_device(list.devices[found].mac, list.devices[found].name);
        }

        // Update bt_sindex for new connection
        if (found != -1)
        {
            if (bt_sindex == 0xFF)
            {
                // First connection
                bt_sindex = BT_SINDEX_ENCODE(found, 0);
            }
            else if (BT_SINDEX_SECOND(bt_sindex) == -1 && BT_SINDEX_FIRST(bt_sindex) != found)
            {
                // Second connection, calculate offset
                int first = BT_SINDEX_FIRST(bt_sindex);
                int offset = (found > first) ? (found - first) : (found + list.count - first);
                bt_sindex = BT_SINDEX_ENCODE(first, offset);
            }
            // else: already two connections, do not update
        }
    }
    else if (!mac && name && mac_stored)
    {
        // Second call: update local list with stored MAC and new name
        int found = -1;
        for (int i = 0; i < list.count; i++)
        {
            if (memcmp(list.devices[i].mac, pending_mac, 6) == 0)
            {
                found = i;
                break;
            }
        }
        if (found == -1 && list.count < MAX_BT_BONDS)
        {
            found = list.count;
            memcpy(list.devices[found].mac, pending_mac, 6);
            strncpy(list.devices[found].name, name, BT_BOND_NAME_LEN);
            list.devices[found].name[BT_BOND_NAME_LEN] = '\0';
            list.count++;
            bt_save_bonded_device(list.devices[found].mac, list.devices[found].name);
        }
        else if (found != -1)
        {
            if (strncmp(list.devices[found].name, name, BT_BOND_NAME_LEN) != 0)
            {
                strncpy(list.devices[found].name, name, BT_BOND_NAME_LEN);
                list.devices[found].name[BT_BOND_NAME_LEN] = '\0';
                bt_save_bonded_device(list.devices[found].mac, list.devices[found].name);
            }
        }
        // No need to update bt_sindex here, as it was handled in the MAC branch

        mac_stored = false;
        pending_idx = -1;
        memset(pending_mac, 0, sizeof(pending_mac));
    }
}
#endif // CONFIG_BT_SPEAKER_MODE

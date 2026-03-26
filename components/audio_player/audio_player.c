/*
 * audio_player.c
 *
 *  Created on: 12.03.2017
 *      Author: michaelboeckling
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"

#include "audio_player.h"
#include "spiram_fifo.h"
#include "freertos/task.h"
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_system.h"
#include "esp_log.h"
#if CONFIG_ESP32_SPIRAM_SUPPORT & CONFIG_SPIRAM ||  CONFIG_ESP32S3_SPIRAM_SUPPORT && CONFIG_SPIRAM
#include "fdk_aac_decoder.h" // No AAC without SPIRAM
#endif
#include "mp3_decoder.h"
#include "webclient.h"
#ifndef CONFIG_NO_VS1053
#include "vs1053.h"
#endif
#include "app_main.h"
#if CONFIG_BT_SPEAKER_MODE
#include "bt_app_av.h"
#endif

#define TAG "audio_player"

player_t *player = NULL;
static player_t *player_instance = NULL;
component_status_t player_status = UNINITIALIZED;
extern bool _isRadio;
extern bool _reboot;
int start_decoder_task(player_t *player)
{
    TaskFunction_t task_func = NULL;
    char *task_name = NULL;
    uint16_t stack_depth = 0;
	int priority = PRIO_MAD;
	if (_reboot)
		return -1;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	ESP_LOGD(TAG, "RAM left %lu", esp_get_free_heap_size());
#else
	ESP_LOGD(TAG, "RAM left %d", esp_get_free_heap_size());
#endif
	if (get_audio_output_mode() == VS1053)
	{
#ifndef CONFIG_NO_VS1053
		task_func = vsTask;
		task_name = (char *)"vsTask";
		stack_depth = 2560; // 3000
		priority = PRIO_VS1053;
#endif
	}
	else
		switch (player->media_stream->content_type)
		{
		case AUDIO_MPEG:
			task_func = mp3_decoder_task;
			task_name = (char *)"mp3_decoder_task";
			stack_depth = 8576; // 8320 /8832 #memchange / 9088 #heap debug printing
			break;
#if (CONFIG_ESP32_SPIRAM_SUPPORT && CONFIG_SPIRAM) ||  (CONFIG_ESP32S3_SPIRAM_SUPPORT && CONFIG_SPIRAM)
		case AUDIO_AAC:
		case OCTET_STREAM: // probably .aac
			if (!bigSram())
			{
				ESP_LOGE(TAG, "aac not supported on WROOM cpu");
				spiRamFifoReset();
				clientDisconnect("no AAC");
				return -1;
			}
			task_func = fdkaac_decoder_task;
			task_name = (char *)"fdkaac_decoder_task";
			stack_depth = 7168; // 6144;
			break;
#endif
#if CONFIG_BT_SPEAKER_MODE
		case AUDIO_BT_PCM: // <-- Bluetooth PCM
			task_func = bt_pcm_decoder_task;
			task_name = (char *)"bt_pcm_decoder_task";
			stack_depth = 2816; // #memchange 3072
			break;
#endif
		default:
			ESP_LOGW(TAG, "unknown mime type: %d", player->media_stream->content_type);
			spiRamFifoReset();
			return -1;
		}

	if (((task_func != NULL)) && (xTaskCreatePinnedToCore(task_func, task_name, stack_depth, player,
														  priority, NULL, CPU_MAD) != pdPASS))
	{

		ESP_LOGE(TAG, "ERROR creating decoder task! Out of memory?");
		spiRamFifoReset();
		return -1;
	}
	else
	{
		player->decoder_status = RUNNING;
	}

	ESP_LOGD(TAG, "decoder task created: %s", task_name);

	return 0;
}

static int t;

/* Writes bytes into the FIFO queue, starts decoder task if necessary. */
int audio_stream_consumer(const char *recv_buf, ssize_t bytes_read)
{

	// don't bother consuming bytes if stopped
	if (player_instance->command == CMD_STOP)
	{
		clientSilentDisconnect();
		return -2;
	}
	if (bytes_read > 0)
		spiRamFifoWrite(recv_buf, bytes_read);

	if (player_instance->decoder_status != RUNNING)
	{
		int bytes_in_buf = spiRamFifoFill();
		uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();

		uint8_t buf_lvl = 90; // BT buffer level
		if (_isRadio)
			buf_lvl = ((bigSram() ? 30 : 80));
		if (fill_level > buf_lvl)
		{
			t = 0;
			onRatio = 100;
			// buffer is filled, start decoder
			if (start_decoder_task(player_instance) != 0)
			{
				ESP_LOGE(TAG, "Decoder task failed");
				audio_player_stop();
				clientDisconnect("decoder failed");
				return -1;
			}
		}
	}

	if (t == 0)
	{
		int bytes_in_buf = spiRamFifoFill();
		uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();

		ESP_LOGI(TAG, "Buffer fill %u%%, %d // %d bytes", fill_level, bytes_in_buf, spiRamFifoLen());
	}
	t = (t + 1) & 255;

	return 0;
}

void audio_player_init(player_t *player)
{
	player_instance = player;
	player_status = INITIALIZED;
}

void audio_player_start()
{	
	ESP_LOGI(TAG, "audio_player_start()");
	if (get_audio_output_mode() != VS1053)
		renderer_start();
	player_instance->media_stream->eof = false;
	player_instance->command = CMD_START;
	player_instance->decoder_command = CMD_NONE;
	    if (player_status != RUNNING) {
        player_status = RUNNING;
        // Force decoder task start if not running
        if (player_instance->decoder_status != RUNNING) {
            int bytes_in_buf = spiRamFifoFill();
            uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();
            if (fill_level > (bigSram() ? 30 : 80)) {
                start_decoder_task(player_instance);
                ESP_LOGI(TAG, "Forced decoder task start in audio_player_start()");
            }
        }
    }
	onRatio = 80;
}

void audio_player_stop()
{
	ESP_LOGI(TAG, "audio_player_stop()");	//		spiRamFifoReset();
	if (player_instance != NULL)
	{
	player_instance->decoder_command = CMD_STOP;
	player_instance->command = CMD_STOP;
	extern bool _isRadio;
	if (_isRadio)
	player_instance->media_stream->eof = true;
	if (get_audio_output_mode() != VS1053)
		renderer_stop();
	player_instance->command = CMD_NONE;
	player_status = STOPPED;
	}
	onRatio = 50;
}

component_status_t get_player_status(void)
{
	return player_status;
}

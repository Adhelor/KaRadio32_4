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
#include "esp_system.h"
#include "esp_log.h"
#if CONFIG_ESP32_SPIRAM_SUPPORT & CONFIG_SPIRAM || CONFIG_ESP32S3_SPIRAM_SUPPORT && CONFIG_SPIRAM
#include "fdk_aac_decoder.h" // No AAC without SPIRAM
#include "FDK_audio.h"
#endif
#include "mp3_decoder.h"
#include "webclient.h"
#ifndef CONFIG_NO_VS1053
#include "vs1053.h"
#endif
#include "app_main.h"
#if CONFIG_BT_SPEAKER_MODE
#include "bt_app_av.h"
extern uint8_t bta_av_co_get_current_codec_id(void);
#endif
#define TAG "audio_player"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

player_t *player = NULL;
static player_t *player_instance = NULL;
component_status_t player_status = UNINITIALIZED;
static portMUX_TYPE stream_info_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_stream_info_t stream_info;
static uint32_t bitrate_window_min;
static uint32_t bitrate_window_max;
static uint8_t bitrate_window_samples;
extern bool _isRadio;
extern bool _reboot;

void audio_player_set_stream_info(audio_stream_codec_t codec, uint32_t sample_rate, uint32_t bitrate)
{
	portENTER_CRITICAL(&stream_info_mux);
	if (codec != stream_info.codec)
	{
		stream_info.codec = codec;
		stream_info.sample_rate = 0;
		stream_info.bitrate = 0;
		stream_info.variable_bitrate = false;
		bitrate_window_min = 0;
		bitrate_window_max = 0;
		bitrate_window_samples = 0;
	}
	if (sample_rate != 0)
		stream_info.sample_rate = sample_rate;
	if (bitrate != 0)
	{
		if (stream_info.bitrate == 0)
		{
			bitrate_window_min = bitrate;
			bitrate_window_max = bitrate;
			bitrate_window_samples = 0;
		}
		else
		{
			if (bitrate < bitrate_window_min)
				bitrate_window_min = bitrate;
			if (bitrate > bitrate_window_max)
				bitrate_window_max = bitrate;
			if (bitrate_window_samples < 32)
				bitrate_window_samples++;
			if (bitrate_window_samples == 32)
			{
				if ((bitrate_window_max - bitrate_window_min) * 100 >= bitrate_window_min * 8)
					stream_info.variable_bitrate = true;
				bitrate_window_min = bitrate;
				bitrate_window_max = bitrate;
				bitrate_window_samples = 0;
			}
		}
		stream_info.bitrate = bitrate;
	}
	portEXIT_CRITICAL(&stream_info_mux);
}

void audio_player_set_stream_vbr(audio_stream_codec_t codec, uint32_t sample_rate)
{
	portENTER_CRITICAL(&stream_info_mux);
	if (codec != stream_info.codec)
	{
		stream_info.codec = codec;
		stream_info.sample_rate = 0;
		stream_info.bitrate = 0;
		bitrate_window_min = 0;
		bitrate_window_max = 0;
		bitrate_window_samples = 0;
	}
	if (sample_rate != 0)
		stream_info.sample_rate = sample_rate;
	stream_info.bitrate = 0;
	stream_info.variable_bitrate = true;
	bitrate_window_min = 0;
	bitrate_window_max = 0;
	bitrate_window_samples = 0;
	portEXIT_CRITICAL(&stream_info_mux);
}

void audio_player_reset_stream_info(audio_stream_codec_t codec, uint32_t sample_rate)
{
	portENTER_CRITICAL(&stream_info_mux);
	stream_info.codec = codec;
	stream_info.sample_rate = sample_rate;
	stream_info.bitrate = 0;
	stream_info.variable_bitrate = false;
	bitrate_window_min = 0;
	bitrate_window_max = 0;
	bitrate_window_samples = 0;
	portEXIT_CRITICAL(&stream_info_mux);
}

void audio_player_get_stream_info(audio_stream_info_t *info)
{
	if (info == NULL)
		return;
	portENTER_CRITICAL(&stream_info_mux);
	*info = stream_info;
	portEXIT_CRITICAL(&stream_info_mux);
}

int start_decoder_task(player_t *player)
{
	TaskFunction_t task_func = NULL;
	char *task_name = NULL;
	uint16_t stack_depth = 0;
	int priority = PRIO_MAD;
	if (player == NULL || player->media_stream == NULL)
	{
		ESP_LOGE(TAG, "Cannot start decoder without player configuration");
		return -1;
	}
	if (_reboot)
		return -1;
	audio_stream_codec_t stream_codec = AUDIO_STREAM_CODEC_NONE;
	if (player->media_stream->content_type == AUDIO_MPEG)
		stream_codec = AUDIO_STREAM_CODEC_MP3;
	else if (player->media_stream->content_type == AUDIO_AAC ||
			 player->media_stream->content_type == OCTET_STREAM ||
			 player->media_stream->content_type == AUDIO_BT_AAC)
		stream_codec = AUDIO_STREAM_CODEC_AAC;
	else if (player->media_stream->content_type == AUDIO_BT_PCM)
		stream_codec = AUDIO_STREAM_CODEC_SBC;
	else if (player->media_stream->content_type == AUDIO_BT_LDAC)
		stream_codec = AUDIO_STREAM_CODEC_LDAC;
	else if (player->media_stream->content_type == AUDIO_BT_APTX)
		stream_codec = AUDIO_STREAM_CODEC_APTX;
	audio_player_set_stream_info(AUDIO_STREAM_CODEC_NONE, 0, 0);
	audio_player_set_stream_info(stream_codec, 0, 0);
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
	{
		ESP_LOGI(TAG, "Content type %d", player->media_stream->content_type);
		switch (player->media_stream->content_type)
		{
		case AUDIO_MPEG:
			task_func = mp3_decoder_task;
			task_name = (char *)"mp3_decoder_task";
			stack_depth = 8576; // 8320 /8832 #memchange / 9088 #heap debug printing

			break;
#if (CONFIG_ESP32_SPIRAM_SUPPORT && CONFIG_SPIRAM) || (CONFIG_ESP32S3_SPIRAM_SUPPORT && CONFIG_SPIRAM)
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
			player->media_stream->transport_type = TT_MP4_ADTS;
			task_name = (char *)"fdkaac_decoder_task";
			stack_depth = 7168; // 6144;
			break;
#endif
#if CONFIG_BT_SPEAKER_MODE
		case AUDIO_BT_AAC:
			// Dynamic check of the active Bluedroid codec ID
			ESP_LOGI(TAG, "Active codec ID: %d", bta_av_co_get_current_codec_id());
#if (CONFIG_ESP32_SPIRAM_SUPPORT && CONFIG_SPIRAM) || (CONFIG_ESP32S3_SPIRAM_SUPPORT && CONFIG_SPIRAM)
			if (!bigSram())
			{
				ESP_LOGE(TAG, "aac not supported on WROOM cpu");
				spiRamFifoReset();
				clientDisconnect("no AAC");
				return -1;
			}
			player->media_stream->transport_type = TT_MP4_LATM_MCP1;
			// Redirect to the native FDK-AAC decoder task
			task_func = fdkaac_decoder_task;
			task_name = (char *)"fdkaac_decoder_task";
			stack_depth = 7168;
#else
			ESP_LOGE(TAG, "AAC requested but SPIRAM is disabled!");
			return -1;
#endif
			break;
		case AUDIO_BT_PCM: // <-- Bluetooth entry point z KaRadio32
		{
			task_func = bt_pcm_decoder_task;
			task_name = (char *)"bt_pcm_decoder_task";
			stack_depth = 2816;
		}
		break;
#if defined(CONFIG_BT_A2DP_LDAC_DECODER) && CONFIG_BT_A2DP_LDAC_DECODER
		case AUDIO_BT_LDAC:
			task_func = bt_ldac_decoder_task;
			task_name = (char *)"bt_ldac_decoder_task";
			stack_depth = 7168;
			break;
#endif
#if defined(CONFIG_BT_A2DP_APTX_DECODER) && CONFIG_BT_A2DP_APTX_DECODER
		case AUDIO_BT_APTX:
			task_func = bt_aptx_decoder_task;
			task_name = (char *)"bt_aptx_decoder_task";
			stack_depth = 7168;
			break;
#endif
#endif
		default:
			ESP_LOGW(TAG, "unknown mime type: %d", player->media_stream->content_type);
			spiRamFifoReset();
			return -1;
		}
	}
	if (((task_func != NULL)) && (xTaskCreatePinnedToCore(task_func, task_name, stack_depth, player,
														  priority, &player->decoder_task_handle, CPU_MAD) != pdPASS))
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
	if (player_instance == NULL || player_instance->media_stream == NULL)
	{
		ESP_LOGE(TAG, "Audio data received without an initialized player");
		return -1;
	}

	// don't bother consuming bytes if stopped
	if (player_instance->command == CMD_STOP)
	{
		// BT mode never runs clientInit(), so sDisconnect is NULL
		if (_isRadio)
			clientSilentDisconnect();
		return -2;
	}
	if (bytes_read > 0)
		spiRamFifoWrite(recv_buf, bytes_read);

	if (player_instance->decoder_status != RUNNING)
	{
		int bytes_in_buf = spiRamFifoFill();
		uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();

		uint8_t buf_lvl = 90; // BT FIFO threshold used by the stock ESP-IDF path
		if (_isRadio)
			buf_lvl = (bigSram() ? 30 : 80);
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

		ESP_LOGD(TAG, "Buffer fill %u%%, %d // %d bytes", fill_level, bytes_in_buf, spiRamFifoLen());
	}
	t = (t + 1) & 255;

	return 0;
}

void audio_player_init(player_t *player)
{
	player_instance = player;
	player->decoder_task_handle = NULL;
	player_status = INITIALIZED;
}

void audio_player_start()
{
	ESP_LOGI(TAG, "audio_player_start()");
	if (player_instance == NULL || player_instance->media_stream == NULL)
	{
		ESP_LOGE(TAG, "Cannot start audio without player configuration");
		return;
	}
	// checkCommand(8, "dbg.fifo");
	if (get_audio_output_mode() != VS1053)
		renderer_start();
	player_instance->media_stream->eof = false;
	player_instance->command = CMD_START;
	player_instance->decoder_command = CMD_NONE;
	if (player_status != RUNNING)
	{
		player_status = RUNNING;
		// Force decoder task start if not running
		if (player_instance->decoder_status != RUNNING)
		{
			int bytes_in_buf = spiRamFifoFill();
			uint8_t fill_level = (bytes_in_buf * 100) / spiRamFifoLen();
			if (fill_level > (bigSram() ? 30 : 80))
			{
				start_decoder_task(player_instance);
				ESP_LOGW(TAG, "Forced decoder task start in audio_player_start()");
			}
		}
	}
	onRatio = 80;
}

void audio_player_stop()
{
	ESP_LOGI(TAG, "audio_player_stop()"); //		spiRamFifoReset();
	if (player_instance != NULL)
	{
		player_instance->decoder_command = CMD_STOP;
		player_instance->command = CMD_STOP;
		extern bool _isRadio;
		if (_isRadio)
			player_instance->media_stream->eof = true;
		if (player_instance->decoder_status == RUNNING)
		{
			int timeout_counter = 0;
			while (player_instance->decoder_status == RUNNING && timeout_counter < 100)
			{
				vTaskDelay(pdMS_TO_TICKS(50));
				timeout_counter++;
			}
			if (player_instance->decoder_status == RUNNING)
			{
				ESP_LOGW(TAG, "Decoder task did not stop within timeout; preserving ownership for self-cleanup.");
				return;
			}
		}
		player_instance->decoder_task_handle = NULL;
		audio_player_set_stream_info(AUDIO_STREAM_CODEC_NONE, 0, 0);
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

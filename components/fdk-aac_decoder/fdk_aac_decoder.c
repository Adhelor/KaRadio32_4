/*
 * fdk_aac_decoder.c
 *
 *  Created on: 08.05.2017
 *      Author: michaelboeckling
 */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
// #include "soc/wdt_periph.h"

#include "common_buffer.h"
#include "interface.h"
#include "aacdecoder_lib.h"
#include "audio_player.h"
#include "app_main.h"
#include "esp_idf_version.h"
#include "driver/i2s.h"
#include "spiram_fifo.h"
#define TAG "Fdkaac_decoder"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

/* small, its just a transfer bucket for the internal decoder buffer */
static const uint32_t INPUT_BUFFER_SIZE = 1024;
static buffer_t *aac_in_buf = NULL;
static buffer_t *aac_pcm_buf = NULL;
buffer_t *get_aac_in_buf(void) { return aac_in_buf; }
buffer_t *get_aac_pcm_buf(void) { return aac_pcm_buf; }

#define MAX_CHANNELS 2
#define MAX_FRAME_SIZE 2048
#define OUTPUT_BUFFER_SIZE (MAX_FRAME_SIZE * sizeof(INT_PCM) * MAX_CHANNELS)

// PCM frame queue between decoder and I2S writer: absorbs HE-AAC decode time spikes (radio only)
#define PCMQ_SLOTS 8
typedef struct
{
	uint8_t *buf;
	uint32_t size;
	pcm_format_t fmt;
} pcmq_item_t;

static QueueHandle_t pcmq_data, pcmq_free;
static SemaphoreHandle_t pcmq_done;
static volatile bool pcmq_stop;
static uint8_t *pcmq_mem[PCMQ_SLOTS];

static void pcmq_task(void *arg)
{
	pcmq_item_t it;
	while (!pcmq_stop)
	{
		if (xQueueReceive(pcmq_data, &it, pdMS_TO_TICKS(100)) != pdTRUE)
			continue;
		render_samples((char *)it.buf, it.size, &it.fmt);
		xQueueSend(pcmq_free, &it.buf, 0);
	}
	xSemaphoreGive(pcmq_done);
	vTaskDelete(NULL);
}

static bool pcmq_start(uint32_t slot_size)
{
	pcmq_data = xQueueCreate(PCMQ_SLOTS, sizeof(pcmq_item_t));
	pcmq_free = xQueueCreate(PCMQ_SLOTS, sizeof(uint8_t *));
	pcmq_done = xSemaphoreCreateBinary();
	int n = 0;
	if (pcmq_data && pcmq_free && pcmq_done)
		for (; n < PCMQ_SLOTS; n++)
		{
			pcmq_mem[n] = malloc(slot_size); // above 4 KB: lands in PSRAM
			if (!pcmq_mem[n])
				break;
			xQueueSend(pcmq_free, &pcmq_mem[n], 0);
		}
	pcmq_stop = false;
	if (n == PCMQ_SLOTS && xTaskCreatePinnedToCore(pcmq_task, "pcmOut", 4096, NULL, PRIO_MAD, NULL, CPU_CLIENT) == pdPASS)
		return true;
	for (int i = 0; i < n; i++)
		free(pcmq_mem[i]);
	if (pcmq_data)
		vQueueDelete(pcmq_data);
	if (pcmq_free)
		vQueueDelete(pcmq_free);
	if (pcmq_done)
		vSemaphoreDelete(pcmq_done);
	pcmq_data = pcmq_free = NULL;
	pcmq_done = NULL;
	return false;
}

static void pcmq_end(void)
{
	if (!pcmq_data)
		return;
	pcmq_stop = true;
	xSemaphoreTake(pcmq_done, portMAX_DELAY);
	for (int i = 0; i < PCMQ_SLOTS; i++)
		free(pcmq_mem[i]);
	vQueueDelete(pcmq_data);
	vQueueDelete(pcmq_free);
	vSemaphoreDelete(pcmq_done);
	pcmq_data = pcmq_free = NULL;
	pcmq_done = NULL;
}

// drops frames not yet written (pause, track change)
static void pcmq_flush(void)
{
	pcmq_item_t it;
	while (pcmq_data && xQueueReceive(pcmq_data, &it, 0) == pdTRUE)
		xQueueSend(pcmq_free, &it.buf, 0);
}

static void out_frame(const player_t *player, const char *buf, uint32_t size, pcm_format_t *fmt)
{
	uint8_t *slot;
	if (!pcmq_data || size > OUTPUT_BUFFER_SIZE)
	{
		render_samples((char *)buf, size, fmt);
		return;
	}
	while (xQueueReceive(pcmq_free, &slot, pdMS_TO_TICKS(50)) != pdTRUE)
		if (player->decoder_command == CMD_STOP)
			return;
	memcpy(slot, buf, size);
	pcmq_item_t it = {.buf = slot, .size = size, .fmt = *fmt};
	xQueueSend(pcmq_data, &it, 0);
}

void fdkaac_decoder_task(void *pvParameters)
{
	// ESP_LOGI(TAG, "(line %u) free heap: %u", __LINE__, esp_get_free_heap_size());
	player_t *player = pvParameters;
	renderer_config_t *renderer_instance;
	renderer_instance = renderer_get();
	AAC_DECODER_ERROR err;
	if (!init_i2s())
	{
		goto abort1;
	}

	// ESP_LOGD(TAG, "init I2S mode %d, port %d, %d bit, %d Hz", renderer_instance->output_mode, renderer_instance->i2s_num, renderer_instance->bit_depth, renderer_instance->sample_rate);
	//  buffer might contain noise
	i2s_zero_dma_buffer(renderer_instance->i2s_num);
	// i2s_start(renderer_instance->i2s_num);
	uint32_t out_buffer_capacity = OUTPUT_BUFFER_SIZE;
	if (player->media_stream->content_type == AUDIO_BT_AAC)
	{
		out_buffer_capacity = OUTPUT_BUFFER_SIZE * 2;
	} /* allocate sample buffer */
	uint32_t out_buffer_sample_capacity = out_buffer_capacity / sizeof(INT_PCM);
	buffer_t *pcm_buf = buf_create_dma(out_buffer_capacity);
	if (pcm_buf == NULL)
	{
		ESP_LOGE(TAG, "malloc(pcm_buf) failed");
		goto abort1;
	}
	aac_pcm_buf = pcm_buf; // Assign to global pointer for monitoring

	/* allocate bitstream buffer */
	//    buffer_t *in_buf = buf_create(INPUT_BUFFER_SIZE*48);
	buffer_t *in_buf = NULL;
	int bf = 12;

	// FIX: LATM streams over Bluetooth require a significantly deeper input bitstream
	// cushion to prevent frame truncation and constant 0x1001 (NOT_ENOUGH_BITS) errors.
	if (player->media_stream->transport_type == 6)
	{
		// Multiply the input buffer allocation size specifically for Bluetooth LATM
		// 1024 * 24 = ~24 KB of hyper-stable deep memory stream buffer
		bf = 16;
	}
	in_buf = buf_create_dma(INPUT_BUFFER_SIZE * bf);
	if (in_buf == NULL)
	{
		ESP_LOGE(TAG, "buf_create in_buf failed");
		buf_destroy(pcm_buf);
		checkCommand(8, "sys.heap");
		aac_pcm_buf = NULL; // Clear global pointer on failure
		goto abort1;
	}
	aac_in_buf = in_buf; // Assign to global pointer for monitoring

	HANDLE_AACDECODER handle = NULL;
	pcm_format_t pcm_format = {.buffer_format = PCM_INTERLEAVED};

	// Only proceed if the stream is AAC/ADTS
	if (player->media_stream->content_type != AUDIO_AAC &&
		player->media_stream->content_type != OCTET_STREAM &&
		player->media_stream->content_type != AUDIO_BT_AAC) // Add other AAC types as needed
	{
		ESP_LOGW(TAG, "Unsupported stream type for AAC decoder: %d", player->media_stream->content_type);
		goto cleanup;
	}

	fill_read_buffer(in_buf);

	/* select bitstream format */
	if (player->media_stream->content_type == AUDIO_MP4)
	{
		goto cleanup;
	}
	else
	{
		/* create decoder instance */
		handle = aacDecoder_Open((TRANSPORT_TYPE)player->media_stream->transport_type, /* num layers */ 1);
		if (handle == 0)
		{
			ESP_LOGE(TAG, "aac invalid handle");
			goto abort1;
		}
	}

	/* configure instance */
	aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_INTERLEAVED, 1);
	aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, -1);
	aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, -1);
	aacDecoder_SetParam(handle, AAC_PCM_LIMITER_ENABLE, 0);
	aacDecoder_SetParam(handle, AAC_QMF_LOWPOWER, -1);

	const uint32_t flags = 0;
	if (player->media_stream->content_type != AUDIO_BT_AAC && !pcmq_start(OUTPUT_BUFFER_SIZE))
		ESP_LOGW(TAG, "PCM queue unavailable, rendering inline");
	uint32_t pcm_size = 0;
	bool first_frame = true;
	int64_t dec_us = 0, aud_us = 0, rnd_us = 0;
	bool warned = false;
	extern bool _reboot;
	unsigned fifo_epoch = spiRamFifoEpoch();
#define DANGEROUS_FRAMES 2
#define FRAME_SIZE (MAX_FRAME_SIZE * sizeof(INT_PCM) * MAX_CHANNELS)

	//    ESP_LOGI(TAG, "(line %u) free heap: %u", __LINE__, esp_get_free_heap_size());

	while (!player->media_stream->eof)
	{
		if (player->decoder_command == CMD_STOP || _reboot)
		{
			break;
		}
		// Hard handle for the dynamic loop pause state
		if (player->command == CMD_PAUSE)
		{
			pcmq_flush();
			renderer_zero_dma_buffer();

			// Stay in a tight power-saving loop until command updates
			while (player->command == CMD_PAUSE)
			{
				vTaskDelay(pdMS_TO_TICKS(20));

				// Emergency exit if the phone/system requests a full stop during pause
				if (player->decoder_command == CMD_STOP || _reboot)
				{
					break;
				}
			}
			if (player->command == CMD_RESUME || player->command == CMD_START)
			{
				if (pcm_buf != NULL)
				{
					pcm_buf->read_pos = pcm_buf->base;
					pcm_buf->fill_pos = pcm_buf->base;
					pcm_buf->len = out_buffer_capacity;
				}
			}
		}
		if (fifo_epoch != spiRamFifoEpoch())
		{
			// FIFO was flushed (track change): drop stale input
			fifo_epoch = spiRamFifoEpoch();
			pcmq_flush();
			in_buf->read_pos = in_buf->base;
			in_buf->fill_pos = in_buf->base;
		}
		size_t bytes_avail = buf_data_unread(in_buf);
		uint32_t bytes_taken = 0;
		UINT buffer_size;
		UINT bytes_valid;
		UCHAR *fill_data;
		bool bt_packet = player->media_stream->content_type == AUDIO_BT_AAC;
		uint16_t packet_size = 0;

		if (bt_packet)
		{
			while (bytes_avail < sizeof(uint16_t))
			{
				size_t bytes_read = fill_read_buffer(in_buf);
				bytes_avail = buf_data_unread(in_buf);
				if (player->decoder_command == CMD_STOP || fifo_epoch != spiRamFifoEpoch())
				{
					break;
				}
				if (bytes_read == 0 && bytes_avail < sizeof(uint16_t))
				{
					vTaskDelay(pdMS_TO_TICKS(2));
				}
			}
			if (player->decoder_command == CMD_STOP)
			{
				break;
			}
			if (fifo_epoch != spiRamFifoEpoch())
			{
				continue;
			}

			packet_size = ((uint16_t)in_buf->read_pos[0] << 8) | in_buf->read_pos[1];
			if (packet_size == 0 || packet_size > in_buf->len - sizeof(uint16_t))
			{
				in_buf->read_pos += sizeof(uint16_t);
				in_buf->bytes_consumed += sizeof(uint16_t);
				continue;
			}

			while (bytes_avail < packet_size + sizeof(uint16_t))
			{
				size_t bytes_read = fill_read_buffer(in_buf);
				bytes_avail = buf_data_unread(in_buf);
				if (player->decoder_command == CMD_STOP || fifo_epoch != spiRamFifoEpoch())
				{
					break;
				}
				if (bytes_read == 0 && bytes_avail < packet_size + sizeof(uint16_t))
				{
					vTaskDelay(pdMS_TO_TICKS(2));
				}
			}
			if (player->decoder_command == CMD_STOP)
			{
				break;
			}
			if (fifo_epoch != spiRamFifoEpoch())
			{
				continue;
			}

			fill_data = in_buf->read_pos + sizeof(uint16_t);
			buffer_size = packet_size;
			bytes_valid = packet_size;
			bytes_taken = packet_size + sizeof(uint16_t);
		}
		else
		{
			if (bytes_avail == 0)
			{
				fill_read_buffer(in_buf);
				bytes_avail = buf_data_unread(in_buf);
				if (player->decoder_command == CMD_STOP)
				{
					break;
				}
				if (bytes_avail == 0)
				{
					vTaskDelay(pdMS_TO_TICKS(2));
					continue;
				}
			}
			fill_data = in_buf->read_pos;
			buffer_size = (UINT)bytes_avail;
			bytes_valid = buffer_size;
		}

//		watchgog reset
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) || CONFIG_IDF_TARGET_ESP32S3)
#if !(CONFIG_IDF_TARGET_ESP32S3)
		TIMERG0.wdtwprotect.wdt_wkey = TIMG_WDT_WKEY_VALUE;
#else
		TIMERG0.wdtwprotect.wdt_wkey = TIMG_WDT_EN;
#endif
		TIMERG0.wdtfeed.wdt_feed = 1;
		TIMERG0.wdtwprotect.wdt_wkey = 0;
#else
		TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
		TIMERG0.wdt_feed = 1;
		TIMERG0.wdt_wprotect = 0;
#endif
		aacDecoder_Fill(handle, &fill_data, &buffer_size, &bytes_valid);
		if (!bt_packet)
		{
			bytes_taken = buffer_size - bytes_valid;
		}
		in_buf->read_pos += bytes_taken;
		pcm_buf->fill_pos = pcm_buf->base + out_buffer_capacity;
		in_buf->bytes_consumed += bytes_taken;

		int64_t t_dec = esp_timer_get_time();
		err = aacDecoder_DecodeFrame(handle, (short int *)pcm_buf->base,
									 out_buffer_sample_capacity, flags);
		if (err == AAC_DEC_OK)
		{
			// diagnostics: decode time vs. audio duration of the same frames (ratio > 90% means the CPU cannot keep up)
			CStreamInfo *si = aacDecoder_GetStreamInfo(handle);
			if (si->sampleRate > 0)
			{
				dec_us += esp_timer_get_time() - t_dec;
				aud_us += (int64_t)si->frameSize * 1000000 / si->sampleRate;
				if (aud_us >= 5000000)
				{
					ESP_LOGD(TAG, "decode load %d%% (%d ms), render %d ms, per %d ms of audio", (int)(dec_us * 100 / aud_us), (int)(dec_us / 1000), (int)(rnd_us / 1000), (int)(aud_us / 1000));
					dec_us = aud_us = rnd_us = 0;
				}
			}
		}
		// Need more LATM data; yield when FDK made no input progress.
		if (err == AAC_DEC_TRANSPORT_SYNC_ERROR || err == AAC_DEC_NOT_ENOUGH_BITS)
		{
			if (bytes_taken == 0)
			{
				vTaskDelay(pdMS_TO_TICKS(2));
			}
			continue;
		}

		if ((err != AAC_DEC_OK) && (err != AAC_DEC_INVALID_CODE_BOOK))
		{
			memset(pcm_buf->base, 0, out_buffer_capacity);
			pcm_size = out_buffer_capacity;
			ESP_LOGW(TAG, "decode error 0x%08x", err);
			out_frame(player, (char *)pcm_buf->base, pcm_size, &pcm_format);
			continue;
		}
		CStreamInfo *mStreamInfo = aacDecoder_GetStreamInfo(handle);
		audio_player_set_stream_info(AUDIO_STREAM_CODEC_AAC,
								 (uint32_t)mStreamInfo->sampleRate,
								 (uint32_t)mStreamInfo->bitRate);

		pcm_size = mStreamInfo->frameSize * sizeof(short) // sizeof(int16_t)
				   * mStreamInfo->numChannels;
		if (pcm_size > out_buffer_capacity)
		{
			ESP_LOGW(TAG, "decode error pcm_size mismatch %d, %d", pcm_size, out_buffer_capacity);
			pcm_size = out_buffer_capacity;
		}
		// Zero any unused part of the buffer to avoid leftover data
		if (pcm_size < out_buffer_capacity)
		{
			memset(pcm_buf->base + pcm_size, 0, out_buffer_capacity - pcm_size);
		}
		/* print first frame, determine pcm buffer length */
		if (first_frame)
		{
			first_frame = false;
			ESP_LOGI(TAG, "pcm_size %d, channels: %d, sample rate: %d, object type: %d, bitrate: %d", pcm_size, mStreamInfo->numChannels,
					 mStreamInfo->sampleRate, mStreamInfo->aot, mStreamInfo->bitRate);

			pcm_format.bit_depth = I2S_BITS_PER_SAMPLE_16BIT;
			pcm_format.num_channels = mStreamInfo->numChannels;
			pcm_format.sample_rate = mStreamInfo->sampleRate;
		}
		pcm_buf->fill_pos = pcm_buf->base + pcm_size;
		// --- DANGEROUS BUFFER LEVEL CHECK ---
		int in_fill = buf_data_unread(in_buf);
		int pcm_fill = buf_data_unread(pcm_buf);
		if (!warned && (in_fill < DANGEROUS_FRAMES * INPUT_BUFFER_SIZE || pcm_fill < DANGEROUS_FRAMES * FRAME_SIZE))
		{
			checkCommand(8, "dbg.fifo");
			warned = true;
		}
		if (warned && (in_fill >= DANGEROUS_FRAMES * INPUT_BUFFER_SIZE && pcm_fill >= DANGEROUS_FRAMES * FRAME_SIZE))
		{
			warned = false;
		}
		// --- END CHECK ---
		int64_t t_rnd = esp_timer_get_time();
		out_frame(player, (char *)pcm_buf->base, pcm_size, &pcm_format);
		rnd_us += esp_timer_get_time() - t_rnd;
	}
	// exit on internal error
	player->command = CMD_STOP;

cleanup:
	pcmq_end();
	// renderer_zero_dma_buffer();
	ESP_LOGI(TAG, "aac decoder stopped");
	if (handle)
		aacDecoder_Close(handle);
	if (in_buf)
		buf_destroy(in_buf);
	if (pcm_buf)
		buf_destroy(pcm_buf);
	aac_in_buf = NULL; // Clear global monitoring pointers
	aac_pcm_buf = NULL;
	renderer_zero_dma_buffer();
	i2s_stop(renderer_instance->i2s_num);
	i2s_driver_uninstall(renderer_instance->i2s_num);
	extern bool i2s_driver_installed;
	i2s_driver_installed = false;
	player->decoder_status = STOPPED;
	player->decoder_command = CMD_NONE;

	//	ESP_LOGD(TAG, "aac_decoder stack: %d\n", uxTaskGetStackHighWaterMark(NULL));
	//	ESP_LOGI(TAG, "%u free heap %u", __LINE__, esp_get_free_heap_size());

	// esp_task_wdt_delete(NULL);
	vTaskDelete(NULL);
	return;
abort1:
	esp_restart();
}

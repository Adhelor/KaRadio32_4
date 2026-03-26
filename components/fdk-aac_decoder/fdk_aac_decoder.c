/*
 * fdk_aac_decoder.c
 *
 *  Created on: 08.05.2017
 *      Author: michaelboeckling
 */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
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
#define TAG "Fdkaac_decoder"

/* small, its just a transfer bucket for the internal decoder buffer */
static const uint32_t INPUT_BUFFER_SIZE = 1024;
static buffer_t *aac_in_buf = NULL;
static buffer_t *aac_pcm_buf = NULL;
buffer_t *get_aac_in_buf(void) { return aac_in_buf; }
buffer_t *get_aac_pcm_buf(void) { return aac_pcm_buf; }

#define MAX_CHANNELS 2
#define MAX_FRAME_SIZE 2048
#define OUTPUT_BUFFER_SIZE (MAX_FRAME_SIZE * sizeof(INT_PCM) * MAX_CHANNELS)

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

	/* allocate sample buffer */
	buffer_t *pcm_buf = buf_create_dma(OUTPUT_BUFFER_SIZE);
	if (pcm_buf == NULL)
	{
		ESP_LOGE(TAG, "malloc(pcm_buf) failed");
		goto abort1;
	}
	aac_pcm_buf = pcm_buf; // Assign to global pointer for monitoring

	/* allocate bitstream buffer */
	//    buffer_t *in_buf = buf_create(INPUT_BUFFER_SIZE*48);
	buffer_t *in_buf = NULL;
	int bf = bigSram() ? 12 : 6;
	in_buf = buf_create_dma(INPUT_BUFFER_SIZE * bf);
	if (in_buf == NULL)
	{
		ESP_LOGE(TAG, "buf_create in_buf failed");
		buf_destroy(pcm_buf);
		aac_pcm_buf = NULL; // Clear global pointer on failure
		goto abort1;
	}
	aac_in_buf = in_buf; // Assign to global pointer for monitoring

	HANDLE_AACDECODER handle = NULL;
	pcm_format_t pcm_format = {.buffer_format = PCM_INTERLEAVED};

	// Only proceed if the stream is AAC/ADTS
	if (player->media_stream->content_type != AUDIO_AAC &&
		player->media_stream->content_type != OCTET_STREAM) // Add other AAC types as needed
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
		handle = aacDecoder_Open(TT_MP4_ADTS, /* num layers */ 1);
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
	uint32_t pcm_size = 0;
	bool first_frame = true;
	bool warned = false;
#define DANGEROUS_FRAMES 2
#define FRAME_SIZE (MAX_FRAME_SIZE * sizeof(INT_PCM) * MAX_CHANNELS)

	//    ESP_LOGI(TAG, "(line %u) free heap: %u", __LINE__, esp_get_free_heap_size());

	while (!player->media_stream->eof)
	{
		/* re-fill buffer if necessary */
		size_t bytes_avail = buf_data_unread(in_buf);

		if (bytes_avail == 0)
		{
			//       if (bytes_avail < INPUT_BUFFER_SIZE) {
			fill_read_buffer(in_buf);
			bytes_avail = buf_data_unread(in_buf);
			//			vTaskDelay(1);
			if (player->decoder_command == CMD_STOP)
			{
				break;
			}
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
		// bytes_avail will be updated and indicate "how much data is left"
		//        size_t bytes_avail = buf_data_unread(in_buf);
		// printf("B%d\n",bytes_avail);
		aacDecoder_Fill(handle, &in_buf->read_pos, &bytes_avail, &bytes_avail);

		uint32_t bytes_taken = buf_data_unread(in_buf) - bytes_avail;
		//        buf_seek_rel(in_buf, bytes_taken);
		in_buf->read_pos += bytes_taken;
		pcm_buf->fill_pos = pcm_buf->base + OUTPUT_BUFFER_SIZE;
		in_buf->bytes_consumed += bytes_taken;

		if (bytes_taken == 0)
		{ // blank output
			//		if (!first_frame) {  // blank output  speed test
			memset(pcm_buf->base, 0, OUTPUT_BUFFER_SIZE);
			pcm_buf->len = OUTPUT_BUFFER_SIZE;
			pcm_size = OUTPUT_BUFFER_SIZE;
			err = AAC_DEC_OK;
		}
		else
		{
			err = aacDecoder_DecodeFrame(handle, (short int *)pcm_buf->base,
										 pcm_buf->len, flags);

			//		if (err != AAC_DEC_OK) continue;
			// need more bytes, lets refill
			if (err == AAC_DEC_TRANSPORT_SYNC_ERROR || err == AAC_DEC_NOT_ENOUGH_BITS)
			{
				ESP_LOGW(TAG, "decode error1 0x%08x", err);
				continue;
			}

			if ((err != AAC_DEC_OK) && (err != AAC_DEC_INVALID_CODE_BOOK))
			{
				memset(pcm_buf->base, 0, OUTPUT_BUFFER_SIZE);
				pcm_size = OUTPUT_BUFFER_SIZE;
				ESP_LOGW(TAG, "decode error 0x%08x", err);
				render_samples((char *)pcm_buf->base, pcm_size, &pcm_format);
				continue;
			}
			CStreamInfo *mStreamInfo = aacDecoder_GetStreamInfo(handle);

			pcm_size = mStreamInfo->frameSize * sizeof(short) // sizeof(int16_t)
					   * mStreamInfo->numChannels;
			if (pcm_size > OUTPUT_BUFFER_SIZE)
			{
				ESP_LOGW(TAG, "decode error pcm_size mismatch %d, %d", pcm_size, OUTPUT_BUFFER_SIZE);
				pcm_size = OUTPUT_BUFFER_SIZE;
			}
			// Zero any unused part of the buffer to avoid leftover data
			if (pcm_size < OUTPUT_BUFFER_SIZE)
			{
				memset(pcm_buf->base + pcm_size, 0, OUTPUT_BUFFER_SIZE - pcm_size);
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
		}
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
		render_samples((char *)pcm_buf->base, pcm_size, &pcm_format);
	}
	//	goto cleanup;
	// exit on internal error
	player->command = CMD_STOP;

cleanup:
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

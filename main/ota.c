/*
 * ESPRSSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
/*
 * Copyright 2017 jp Cocatrix (http://www.karawin.fr)
 */
#define TAG "OTA"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include <string.h>
#include <sys/socket.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "interface.h"
#include "webclient.h"
#include "app_main.h"
#include "websocket.h"
#include "addon.h"
#include <time.h>

// Buffer size for OTA processing
#define OTA_QUEUE_LEN 4
#define OTA_CHUNK_SIZE 1024
#define OTA_ABORT_MARKER UINT16_MAX

typedef struct
{
	uint16_t len;
	uint8_t data[OTA_CHUNK_SIZE];
} ota_chunk_t;
bool bsOTA = false;
static QueueHandle_t ota_queue = NULL;
static SemaphoreHandle_t ota_done = NULL;
static volatile bool ota_active = false;
static portMUX_TYPE ota_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_err_t ota_result = ESP_FAIL;
static size_t ota_written_bytes = 0;
// static unsigned int ota_progress_count = 0;
static size_t ota_expected_size = 0;
static int last_reported_percent = -1;
// One-time OTA PIN storage
static char ota_pin[16] = "";
static time_t ota_pin_expire = 0;

// Generate a one-time PIN, display it on device for 'seconds'
void ota_generate_pin(int seconds)
{
	if (seconds <= 0)
		seconds = 60; // default 60 seconds
	uint32_t r = esp_random();
	uint32_t pinv = r % 10000; // 4 digit PIN
	snprintf(ota_pin, sizeof(ota_pin), "%04u", (unsigned)pinv);
	ota_pin_expire = time(NULL) + seconds;
	showPin(ota_pin);
	ESP_LOGI(TAG, "Generated OTA PIN (displayed)");
}

// small base64 encoder for short buffers
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void base64_encode_short(const unsigned char *in, size_t inlen, char *out, size_t outlen)
{
	size_t i = 0, o = 0;
	while (i + 2 < inlen)
	{
		unsigned int val = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
		if (o + 4 < outlen)
		{
			out[o++] = b64chars[(val >> 18) & 0x3F];
			out[o++] = b64chars[(val >> 12) & 0x3F];
			out[o++] = b64chars[(val >> 6) & 0x3F];
			out[o++] = b64chars[val & 0x3F];
		}
		i += 3;
	}
	if (i < inlen)
	{
		unsigned int val = in[i] << 16;
		if (i + 1 < inlen)
			val |= in[i + 1] << 8;
		if (o + 4 < outlen)
		{
			out[o++] = b64chars[(val >> 18) & 0x3F];
			out[o++] = b64chars[(val >> 12) & 0x3F];
			if (i + 1 < inlen)
				out[o++] = b64chars[(val >> 6) & 0x3F];
			else
				out[o++] = '=';
			out[o++] = '=';
		}
	}
	if (o < outlen)
		out[o] = '\0';
	else
		out[outlen - 1] = '\0';
}

// Validate and consume the stored PIN
// Only accepts the base64-encoded PIN sent by the client (raw PIN is not accepted)
bool ota_check_pin(const char *pin)
{
	if (bsOTA)
	{
		if (!pin)
			return false;
		if (ota_pin[0] == '\0')
			return false;
		time_t now = time(NULL);
		if (ota_pin_expire != 0 && now > ota_pin_expire)
		{
			ota_pin[0] = '\0';
			ota_pin_expire = 0;
			return false;
		}
		// compute base64 of ota_pin
		char expect[16];
		base64_encode_short((const unsigned char *)ota_pin, strlen(ota_pin), expect, sizeof(expect));
		// compare
		if (strcmp(pin, expect) == 0)
		{
			ota_pin[0] = '\0';
			ota_pin_expire = 0;
			return true;
		}
		return false;
	}
	else
		return true; // if PIN is disabled, always accept
}

void ota_clear_pin(void)
{
	ota_pin[0] = '\0';
	ota_pin_expire = 0;
}
/******************************************************************************
 * FunctionName : wsUpgrade
 * Description  : send the OTA feedback to websockets
 * Parameters   : str - error message or empty string
 *                count - bytes written
 *                total - total bytes to write
 * Returns      : none
 *******************************************************************************/
void wsUpgrade(const char *str, int count, int total)
{
	char answer[100];
	if (strlen(str) != 0)
	{
		sprintf(answer, "{\"upgrade\":\"%s\"}", str);
	}
	else
	{
		int value = (total > 0) ? (count * 100 / total) : 0;
		if (value > 100)
			value = 100;
		memset(answer, 0, 100);

		if (value >= 100)
			sprintf(answer, "{\"upgrade\":\"Done. Refresh the page.\"}");
		else if (value == 0)
			sprintf(answer, "{\"upgrade\":\"Starting.\"}");
		else
			sprintf(answer, "{\"upgrade\":\"%d%%\"}", value);
	}
	websocketbroadcast(answer, strlen(answer));
}

/******************************************************************************
 * FunctionName : ota_task
 * Description  : OTA task - writes firmware from buffer to flash partition
 *                with progress monitoring via websocket
 * Parameters   : pvParameter - unused
 * Returns      : none
 *******************************************************************************/
static void ota_task(void *pvParameter)
{
	ESP_LOGW(TAG, "OTA task started");

	esp_ota_handle_t update_handle = 0;
	esp_err_t err;
	const esp_partition_t *update_partition =
		esp_ota_get_next_update_partition(NULL);

	if (!update_partition)
	{
		ESP_LOGE(TAG, "No OTA partition");
		ota_result = ESP_ERR_NOT_FOUND;
		xSemaphoreGive(ota_done);
		vTaskDelete(NULL);
		return;
	}
	if (ota_expected_size > update_partition->size)
	{
		ESP_LOGE(TAG, "OTA image is too large: %u bytes (partition: %u)",
				 (unsigned)ota_expected_size, (unsigned)update_partition->size);
		ota_result = ESP_ERR_INVALID_SIZE;
		xSemaphoreGive(ota_done);
		vTaskDelete(NULL);
		return;
	}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
			 update_partition->subtype, update_partition->address);
#else
	ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%08x",
			 update_partition->subtype, update_partition->address);
#endif

	err = esp_ota_begin(update_partition, ota_expected_size, &update_handle);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_begin failed (0x%x)", err);
		ota_result = err;
		xSemaphoreGive(ota_done);
		vTaskDelete(NULL);
		return;
	}

	ota_result = ESP_OK;
	xSemaphoreGive(ota_done);
	ota_chunk_t chunk;

	while (1)
	{
		if (xQueueReceive(ota_queue, &chunk, portMAX_DELAY) != pdPASS)
			continue;

		if (chunk.len == 0 || chunk.len == OTA_ABORT_MARKER)
		{
			if (chunk.len == OTA_ABORT_MARKER && ota_result == ESP_OK)
				ota_result = ESP_FAIL;
			break;
		}

		if (ota_result != ESP_OK)
			continue;

		err = esp_ota_write(update_handle, chunk.data, chunk.len);
		if (err != ESP_OK)
		{
			ESP_LOGE(TAG, "esp_ota_write failed (0x%x)", err);
			ota_result = err;
			continue;
		}

		ota_written_bytes += chunk.len;

		// update websockets and progress display roughly every 32KB or when percent changes
		if (ota_expected_size > 0)
		{
			int percent = (int)((ota_written_bytes * 100ULL) / ota_expected_size);
			if (percent >= 100)
				percent = 99;
			if (percent != last_reported_percent)
			{
				last_reported_percent = percent;
				setOtaProgress(percent);
				wsUpgrade("", percent, 100);
			}
		}
	}

	if (ota_result == ESP_OK && ota_written_bytes != ota_expected_size)
	{
		ESP_LOGE(TAG, "Incomplete OTA image: %u of %u bytes",
				 (unsigned)ota_written_bytes, (unsigned)ota_expected_size);
		ota_result = ESP_FAIL;
	}
	if (ota_result != ESP_OK)
	{
		err = esp_ota_abort(update_handle);
		if (err != ESP_OK)
			ESP_LOGE(TAG, "esp_ota_abort failed (0x%x)", err);
	}
	else
	{
		ota_result = esp_ota_end(update_handle);
		if (ota_result != ESP_OK)
			ESP_LOGE(TAG, "esp_ota_end failed (0x%x)", ota_result);
		else
		{
			ota_result = esp_ota_set_boot_partition(update_partition);
			if (ota_result != ESP_OK)
				ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (0x%x)", ota_result);
		}
	}
	xSemaphoreGive(ota_done);
	vTaskDelete(NULL);
}

/******************************************************************************
 * FunctionName : ota_update_from_socket_start
 * Description  : Start the OTA writer and wait for partition initialization.
 * Parameters   : expected_size - total firmware size
 * Returns      : ESP_OK if the partition is ready
 *******************************************************************************/
static void ota_release(void)
{
	vQueueDelete(ota_queue);
	vSemaphoreDelete(ota_done);
	ota_queue = NULL;
	ota_done = NULL;
	setOtaProgress(-1);
	portENTER_CRITICAL(&ota_lock);
	ota_active = false;
	portEXIT_CRITICAL(&ota_lock);
}

esp_err_t ota_update_from_socket_start(size_t expected_size)
{
	if (expected_size == 0)
		return ESP_ERR_INVALID_SIZE;
	portENTER_CRITICAL(&ota_lock);
	if (ota_active)
	{
		portEXIT_CRITICAL(&ota_lock);
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}
	ota_active = true;
	portEXIT_CRITICAL(&ota_lock);

	clientDisconnect("OTA update");

	ota_queue = xQueueCreate(OTA_QUEUE_LEN, sizeof(ota_chunk_t));
	ota_done = xSemaphoreCreateBinary();
	if (!ota_queue || !ota_done)
	{
		ESP_LOGE(TAG, "OTA: queue or semaphore create failed");
		if (ota_queue)
			vQueueDelete(ota_queue);
		if (ota_done)
			vSemaphoreDelete(ota_done);
		portENTER_CRITICAL(&ota_lock);
		ota_active = false;
		portEXIT_CRITICAL(&ota_lock);
		return ESP_ERR_NO_MEM;
	}

	ota_written_bytes = 0;
	ota_expected_size = expected_size;
	last_reported_percent = -1;
	BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota_task", 8192, NULL, PRIO_OTA, NULL, CPU_OTA);

	if (ok != pdPASS)
	{
		ESP_LOGE(TAG, "OTA: task create failed");
		ota_release();
		return ESP_FAIL;
	}
	xSemaphoreTake(ota_done, portMAX_DELAY);
	if (ota_result != ESP_OK)
	{
		esp_err_t err = ota_result;
		ota_release();
		return err;
	}

	if (lcd_type != LCD_NONE && event_lcd != NULL)
	{
		// Clear the screen fully before showing OTA UI to avoid artifacts
		event_lcd_t clevt = {.lcmd = eclrs, .lline = NULL};
		if (xQueueSend(event_lcd, &clevt, 0) != pdTRUE)
		{
			// ignore failure to send clear
		}

		// Post a META message for upload
		const char *msg = "META#: - uploading firmware...";
		char *s4 = kmalloc(strlen(msg) + 1);
		if (s4)
		{
			strcpy(s4, msg);
			event_lcd_t evt = {.lcmd = lmeta, .lline = s4};
			if (xQueueSend(event_lcd, &evt, 0) != pdTRUE)
				free(s4);
		}
	}
	wsUpgrade("Starting OTA update...", 0, 0);

	return ESP_OK;
}
esp_err_t ota_update_from_socket_write(const uint8_t *data, size_t len)
{
	if (!ota_active || !ota_queue || len == 0)
		return ESP_ERR_INVALID_STATE;

	ota_chunk_t chunk;
	while (len > 0)
	{
		size_t part = len > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : len;
		memcpy(chunk.data, data, part);
		chunk.len = part;
		if (xQueueSend(ota_queue, &chunk, portMAX_DELAY) != pdPASS)
			return ESP_FAIL;
		data += part;
		len -= part;
	}

	return ESP_OK;
}
static esp_err_t ota_complete(bool abort)
{
	if (!ota_active || !ota_queue)
		return ESP_ERR_INVALID_STATE;

	ota_chunk_t chunk = {.len = abort ? OTA_ABORT_MARKER : 0};
	if (xQueueSend(ota_queue, &chunk, portMAX_DELAY) != pdPASS)
		return ESP_FAIL;
	xSemaphoreTake(ota_done, portMAX_DELAY);
	esp_err_t result = ota_result;
	if (result != ESP_OK)
		wsUpgrade("OTA update failed", 0, 0);
	ota_release();
	return result;
}

esp_err_t ota_update_from_socket_finish(void)
{
	return ota_complete(false);
}

void ota_update_from_socket_abort(void)
{
	esp_err_t err = ota_complete(true);
	if (err != ESP_FAIL)
		ESP_LOGE(TAG, "OTA abort returned 0x%x", err);
}

/******************************************************************************
 * FunctionName : ota_is_running
 * Description  : Check if OTA update task is currently running
 * Parameters   : none
 * Returns      : true if running, false otherwise
 *******************************************************************************/
bool ota_is_running(void)
{
	return ota_active;
}

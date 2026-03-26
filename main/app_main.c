/*
  KaRadio 32
  A WiFi webradio player

Copyright (C) 2017  KaraWin

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include <nvs.h>
#include "KaRadio_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "driver/i2s.h"
#include "driver/uart.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "esp_system.h" //#debug
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_netif.h"
#include "esp_mac.h"
#endif
#include "mdns.h"
#include "app_main.h"
#include "audio_renderer.h"
#if CONFIG_BT_SPEAKER_MODE && !CONFIG_IDF_TARGET_ESP32S3
#include "bt_speaker.h"
#endif
#include "audio_player.h"
// #if defined(CONFIG_DISPLAY_TYPE_MONO) || defined(CONFIG_DISPLAY_TYPE_ALL)
#include "u8g2_esp32_hal.h"
// #endif
#include "addon.h"
#include "ClickButtons.h"
#include "eeprom.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "nvs_flash.h"
#include "gpio.h"
#include "servers.h"
#include "webclient.h"
#include "webserver.h"
#include "interface.h"
#ifndef CONFIG_NO_VS1053
#include "vs1053.h"
#endif
#include "ClickEncoder.h"
#include "addon.h"
#include "spiram_fifo.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "driver/gptimer.h"
#else
#include "driver/timer.h"
#endif
#if CONFIG_BLINK_LED_RMT
#include "driver/rmt.h"
#include "led_strip.h"
static led_strip_t *led_strip = NULL; // Declare as a global variable
void setLedColor(const uint8_t color[3], uint8_t brightness);
#else
#include "driver/ledc.h"
#endif

// Predefined LED colors
static const uint8_t led_lgreen[3] = {24, 108, 24};	 // Light Green
static const uint8_t led_white[3] = {255, 255, 255}; // Light Green
static const uint8_t led_dgreen[3] = {0, 100, 0};	 // Dark Green
static const uint8_t led_red[3] = {255, 0, 0};		 // Red
static const uint8_t led_yellow[3] = {255, 255, 0};	 // Yellow
static const uint8_t led_orange[3] = {255, 165, 0};	 // Orange
// Global LED color
static uint8_t led_color[3] = {255, 165, 0}; // Default color (Orange for AP mode)
uint8_t led_brightness = 100;				 // Default brightness
uint8_t last_brightness = 0;
#define LEDC_TIMER LEDC_TIMER_1
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO g_device->led_gpio // Use your configured GPIO
#define LEDC_CHANNEL LEDC_CHANNEL_1
#define LEDC_DUTY_RES LEDC_TIMER_8_BIT // 8-bit resolution (0-255)
#define LEDC_FREQUENCY 5000			   // 5 kHz PWM frequency
// #include "esp_heap_trace.h"
// #include "esp_heap_caps.h"
/* The event group allows multiple bits for each event*/
//   are we connected  to the AP with an IP? */
const int CONNECTED_BIT = 0x00000001;
//
const int CONNECTED_AP = 0x00000010;

#define TAG "main"

// Priorities of the reader and the decoder thread. bigger number = higher prio
#define PRIO_READER configMAX_PRIORITIES - 3
#define PRIO_MQTT configMAX_PRIORITIES - 3
#define PRIO_CONNECT configMAX_PRIORITIES - 1
#define striWATERMARK "watermark: %d  heap: %d"

void start_network();
void autoPlay();
#if CONFIG_BT_SPEAKER_MODE
void bt_speaker_start();
#endif
/* */
static bool wifiInitDone = false;
static EventGroupHandle_t wifi_event_group;
xQueueHandle event_queue;
// static heap_trace_record_t trace_buffer[100];
//  xSemaphoreHandle print_mux;

bool ledStatus;				   // true: normal blink, false: led on when playing
bool ledPolarity;			   // true: normal false: reverse
bool logTel;				   // true = log also on telnet
uint16_t cycleDuration = 1000; // duration of the led cycle in ms
uint8_t onRatio = 50;
player_t *player_config;
static output_mode_t audio_output_mode;
static uint8_t clientIvol = 0;
// ip
static char localIp[20];
// 4MB sram?
bool bigRam = false;
// timeout to save volume in flash
// static uint32_t ctimeVol = 0;
static uint32_t ctimeMs = 0;
static bool divide = false;
esp_netif_t *ap;
esp_netif_t *sta;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
gptimer_handle_t mstimer = NULL;
gptimer_handle_t sleeptimer = NULL;
gptimer_handle_t waketimer = NULL;

IRAM_ATTR void noInterrupt1Ms() {}
// enable 1MS timer interrupt
IRAM_ATTR void interrupt1Ms() {}
#else
// disable 1MS timer interrupt
IRAM_ATTR void noInterrupt1Ms() { timer_disable_intr(TIMERGROUP1MS, msTimer); }
// enable 1MS timer interrupt
IRAM_ATTR void interrupt1Ms() { timer_enable_intr(TIMERGROUP1MS, msTimer); }
// IRAM_ATTR void noInterrupts() {noInterrupt1Ms();}
// IRAM_ATTR void interrupts() {interrupt1Ms();}
#endif
void ledBlinkTask(void *p);
char *getIp() { return (localIp); }
IRAM_ATTR uint8_t getIvol() { return clientIvol; }
IRAM_ATTR void setIvol(uint8_t vol) { clientIvol = vol; }; // ctimeVol = 0;}
IRAM_ATTR output_mode_t get_audio_output_mode() { return audio_output_mode; }

bool bigSram() { return bigRam; }
void *kmalloc(size_t memorySize)
{
	if (bigRam)
		return heap_caps_malloc(memorySize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	else
		return heap_caps_malloc(memorySize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void *kcalloc(size_t elementCount, size_t elementSize)
{
	if (bigRam)
		return heap_caps_calloc(elementCount, elementSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	else
		return heap_caps_calloc(elementCount, elementSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static bool msCallback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
	BaseType_t high_task_awoken = pdFALSE;
	xQueueHandle event_qu = (xQueueHandle)user_ctx;
	queue_event_t evt;
	evt.type = TIMER_1MS;
	evt.i1 = 0;
	evt.i2 = 0;
	xQueueSendFromISR(event_qu, &evt, NULL);
	if (serviceAddon != NULL)
		serviceAddon(); // for the encoders and buttons
	return high_task_awoken == pdTRUE;
}
#else
//-----------------------------------
// every 500µs
IRAM_ATTR void msCallback(void *pArg)
{
	int timer_idx = (int)pArg;
	queue_event_t evt;

#ifndef CONFIG_IDF_TARGET_ESP32S3
	TIMERG1.hw_timer[timer_idx].update = 1;
	TIMERG1.int_clr_timers.t0 = 1; // isr ack
	evt.type = TIMER_1MS;
	evt.i1 = TIMERGROUP1MS;
	evt.i2 = timer_idx;
	xQueueSendFromISR(event_queue, &evt, NULL);
	if (serviceAddon != NULL)
		serviceAddon(); // for the encoders and buttons
	TIMERG1.hw_timer[timer_idx].config.alarm_en = 1;
#else
	TIMERG1.hw_timer[timer_idx].update.val = 1;
	TIMERG1.int_clr_timers.t0_int_clr = 1; // isr ack
	evt.type = TIMER_1MS;
	evt.i1 = TIMERGROUP1MS;
	evt.i2 = timer_idx;
	xQueueSendFromISR(event_queue, &evt, NULL);
	if (serviceAddon != NULL)
		serviceAddon(); // for the encoders and buttons
	TIMERG1.hw_timer[timer_idx].config.tn_alarm_en = 1;
#endif
}
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static bool sleepCallback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
	BaseType_t high_task_awoken = pdFALSE;
	gptimer_stop(timer);
	xQueueHandle event_qu = (xQueueHandle)user_ctx;
	queue_event_t evt;
	evt.type = TIMER_SLEEP;
	evt.i1 = 0;
	evt.i2 = 0;
	xQueueSendFromISR(event_qu, &evt, NULL);
	return high_task_awoken == pdTRUE;
}
#else
void sleepCallback(void *pArg)
{
	int timer_idx = (int)pArg;
	queue_event_t evt;
#ifndef CONFIG_IDF_TARGET_ESP32S3
	TIMERG0.int_clr_timers.t0 = 1; // isr ack
	evt.type = TIMER_SLEEP;
	evt.i1 = TIMERGROUP;
	evt.i2 = timer_idx;
	xQueueSendFromISR(event_queue, &evt, NULL);
	TIMERG0.hw_timer[timer_idx].config.alarm_en = 0;
#else
	TIMERG0.int_clr_timers.t0_int_clr = 1; // isr ack
	evt.type = TIMER_SLEEP;
	evt.i1 = TIMERGROUP;
	evt.i2 = timer_idx;
	xQueueSendFromISR(event_queue, &evt, NULL);
	TIMERG0.hw_timer[timer_idx].config.tn_alarm_en = 0;
#endif
}
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static bool wakeCallback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
	BaseType_t high_task_awoken = pdFALSE;
	gptimer_stop(timer);
	xQueueHandle event_qu = (xQueueHandle)user_ctx;
	queue_event_t evt;
	evt.type = TIMER_WAKE;
	evt.i1 = 0;
	evt.i2 = 0;
	xQueueSendFromISR(event_qu, &evt, NULL);
	return high_task_awoken == pdTRUE;
}
#else
void wakeCallback(void *pArg)
{
	int timer_idx = (int)pArg;
	queue_event_t evt;
#ifndef CONFIG_IDF_TARGET_ESP32S3
	TIMERG0.int_clr_timers.t1 = 1;
	evt.i1 = TIMERGROUP;
	evt.i2 = timer_idx;
	evt.type = TIMER_WAKE;
	xQueueSendFromISR(event_queue, &evt, NULL);
	TIMERG0.hw_timer[timer_idx].config.alarm_en = 0;
#else
	TIMERG0.int_clr_timers.t1_int_clr = 1;
	evt.i1 = TIMERGROUP;
	evt.i2 = timer_idx;
	evt.type = TIMER_WAKE;
	xQueueSendFromISR(event_queue, &evt, NULL);
	TIMERG0.hw_timer[timer_idx].config.tn_alarm_en = 0;
#endif
}
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
uint64_t getSleep()
{
	uint64_t ret = 0;
	ESP_ERROR_CHECK(gptimer_get_raw_count(sleeptimer, &ret));
	ESP_LOGD(TAG, "getSleep: ret: %lld", ret);
	return ret / 10000ll;
}
#else
// return the current timer value in sec
uint64_t getSleep()
{
	uint64_t ret = 0;
	uint64_t tot = 0;
	timer_get_alarm_value(TIMERGROUP, sleepTimer, &tot);
	timer_get_counter_value(TIMERGROUP, sleepTimer, &ret);
	ESP_LOGD(TAG, "getSleep: ret: %lld, tot: %lld, return %lld", ret, tot, tot - ret);
	return ((tot)-ret) / 5000000;
}
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
uint64_t getWake()
{
	uint64_t ret = 0;
	ESP_ERROR_CHECK(gptimer_get_raw_count(waketimer, &ret));
	ESP_LOGD(TAG, "getWake: ret: %lld", ret);
	return ret / 10000ll;
}
#else
uint64_t getWake()
{
	uint64_t ret = 0;
	uint64_t tot = 0;
	timer_get_alarm_value(TIMERGROUP, wakeTimer, &tot);
	timer_get_counter_value(TIMERGROUP, wakeTimer, &ret);
	ESP_LOGD(TAG, "getWake: ret: %lld, tot: %lld  return %lld", ret, (tot), tot - ret);
	return ((tot)-ret) / 5000000;
}
#endif

#if CONFIG_BLINK_LED_RMT
void setLedColorWrapper(const uint8_t color[3], uint8_t brightness)
{
	setLedColor(color, brightness); // Call the actual function
}
#else
void initPwmLed()
{
	if (g_device->led_gpio == GPIO_NONE)
	{
		ESP_LOGW(TAG, "Status LED GPIO not set, skipping PWM init");
		return;
	}
	ledc_timer_config_t ledc_timer = {
		.speed_mode = LEDC_MODE,
		.timer_num = LEDC_TIMER,
		.duty_resolution = LEDC_DUTY_RES,
		.freq_hz = LEDC_FREQUENCY,
		.clk_cfg = LEDC_AUTO_CLK};
	ledc_timer_config(&ledc_timer);

	ledc_channel_config_t ledc_channel = {
		.speed_mode = LEDC_MODE,
		.channel = LEDC_CHANNEL,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = LEDC_OUTPUT_IO,
		.duty = 0, // Start off
		.hpoint = 0};
	ledc_channel_config(&ledc_channel);
}

void setLedColorWrapper(const uint8_t color[3], uint8_t brightness)
{
	// No-op for non-ESP32S3 targets
	(void)color; // Suppress unused parameter warning
				 // For single-color LED, ignore color and use brightness for PWM
				 // Initialize PWM if not already done (call initPwmLed() in your init)
	if (last_brightness != brightness)
	{
		last_brightness = brightness;

		uint32_t duty;
		if (ledPolarity)
		{
			duty = (brightness * 255) / 100;
		}
		else
		{
			duty = 255 - ((brightness * 255) / 100);
		}
		ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
		ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
		// ESP_LOGI(TAG, "LED duty set to %d (brightness %d)", duty, brightness);
	}
}
#endif
void setLedOnOff(bool on)
{
	// on=true: LED ON, on=false: LED OFF
	if (on)
	{
		setLedColorWrapper(NULL, led_brightness);
	}
	else
	{
		// For OFF, set brightness to 0 for active high, 100 for active low
		setLedColorWrapper(NULL, ledPolarity ? 0 : 100);
	}
}
void setLedColor(const uint8_t color[3], uint8_t brightness)
{
	// Update the global LED color
	memcpy(led_color, color, sizeof(led_color));

#ifdef CONFIG_BLINK_LED_RMT
	// Scale brightness to 0-255
	uint8_t scaled_brightness = (brightness * 255) / 100;
	// Update the LED strip
	if (led_strip)
	{
		ESP_ERROR_CHECK(led_strip->set_pixel(led_strip, 0,
											 (color[0] * scaled_brightness) / 255,
											 (color[1] * scaled_brightness) / 255,
											 (color[2] * scaled_brightness) / 255));
		ESP_ERROR_CHECK(led_strip->refresh(led_strip, 100));
	}
#endif
}
void initStatusLED()
{
	gpio_num_t led_gpio = g_device->led_gpio;
	xTaskHandle pxCreatedTask;
	if (g_device->led_brightness == 0xFF)
	{
		g_device->led_brightness = led_brightness;
		saveDeviceSettings(g_device);
	}
	else
	{
		led_brightness = g_device->led_brightness;
	}

	// Check if the LED GPIO is valid
	if (led_gpio == GPIO_NONE)
	{
		ESP_LOGW(TAG, "P_LED_GPIO is not configured (GPIO_NONE). Skipping LED initialization.");
		return;
	}

#if CONFIG_BLINK_LED_RMT
	ESP_LOGI(TAG, "Initializing addressable LED on GPIO %d, channel %d", led_gpio, CONFIG_BLINK_LED_RMT_CHANNEL);
	led_strip = led_strip_init(CONFIG_BLINK_LED_RMT_CHANNEL, led_gpio, 1);
	if (!led_strip)
	{
		ESP_LOGE(TAG, "Failed to initialize addressable LED on GPIO %d", led_gpio);
		return;
	}
	/* Set all LED off to clear all pixels */
	led_strip->clear(led_strip, 50);
	// Set initial brightness and color
	setLedColor(led_color, led_brightness);
	xTaskCreatePinnedToCore(ledBlinkTask, "ledBlinkTask", 1280, NULL, PRIO_UART, &pxCreatedTask, CPU_ST_LED);

#else
	// For other targets, initialize the standard GPIO-based LED
	initPwmLed();
	setLedOnOff(false);																						 // LED OFF at init
	xTaskCreatePinnedToCore(ledBlinkTask, "ledBlinkTask", 756, NULL, PRIO_UART, &pxCreatedTask, CPU_ST_LED); // #memchange 756 | 2048 if esp_log used

#endif
	ESP_LOGI(TAG, "%s task: %x", "uartIfaceTask", (unsigned int)pxCreatedTask);
}
void tsocket(const char *lab, uint32_t cnt)
{
	char *title = kmalloc(strlen(lab) + 50);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	sprintf(title, "{\"%s\":\"%lu\"}", lab, cnt * 60);
#else
	sprintf(title, "{\"%s\":\"%d\"}", lab, cnt * 60);
#endif
	websocketbroadcast(title, strlen(title));
	free(title);
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void stopSleep()
{
	ESP_LOGD(TAG, "stopSleep");
	ESP_ERROR_CHECK(gptimer_stop(sleeptimer));
	tsocket("lsleep", 0);
}
gptimer_event_callbacks_t cbss = {
	.on_alarm = sleepCallback, // register user callback
};
gptimer_alarm_config_t alarm_config = {
	.alarm_count = 0,
	.flags.auto_reload_on_alarm = false, // enable auto-reload
};
void startSleep(uint32_t delay)
{
	ESP_LOGD(TAG, "startSleep: %lu min.", delay);
	ESP_ERROR_CHECK(gptimer_stop(sleeptimer));
	vTaskDelay(pdMS_TO_TICKS(10));
	if (delay == 0)
		return;
	ESP_ERROR_CHECK(gptimer_set_raw_count(sleeptimer, delay * 600000));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(sleeptimer, &alarm_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(sleeptimer, &cbss, event_queue));
	//	ESP_ERROR_CHECK(gptimer_enable(sleeptimer)); // for IDF > 5
	ESP_ERROR_CHECK(gptimer_start(sleeptimer));
	tsocket("lsleep", delay);
}
void stopWake()
{
	ESP_LOGD(TAG, "stopWake");
	ESP_ERROR_CHECK(gptimer_stop(waketimer));
	tsocket("lwake", 0);
}
gptimer_event_callbacks_t cbsw = {
	.on_alarm = wakeCallback, // register user callback
};
void startWake(uint32_t delay)
{
	ESP_LOGD(TAG, "startWake: %lu min.", delay);
	ESP_ERROR_CHECK(gptimer_stop(waketimer));
	vTaskDelay(pdMS_TO_TICKS(10));
	if (delay == 0)
		return;
	ESP_ERROR_CHECK(gptimer_set_raw_count(waketimer, delay * 600000ll));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(waketimer, &alarm_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(waketimer, &cbsw, event_queue));
	//	ESP_ERROR_CHECK(gptimer_enable(waketimer));  // for IDF > 5
	ESP_ERROR_CHECK(gptimer_start(waketimer));
	tsocket("lwake", delay);
}
#else
void stopSleep()
{
	ESP_LOGD(TAG, "stopSleep");
	ESP_ERROR_CHECK(timer_pause(TIMERGROUP, sleepTimer));
	ESP_ERROR_CHECK(timer_set_alarm_value(TIMERGROUP, sleepTimer, 0x00000000ULL));
	ESP_ERROR_CHECK(timer_set_counter_value(TIMERGROUP, sleepTimer, 0x00000000ULL));
	tsocket("lsleep", 0);
}
void startSleep(uint32_t delay)
{
	ESP_LOGD(TAG, "startSleep: %d min.", delay);
	if (delay == 0)
		return;
	stopSleep();
	ESP_ERROR_CHECK(timer_set_counter_value(TIMERGROUP, sleepTimer, 0x00000000ULL));
	ESP_ERROR_CHECK(timer_set_alarm_value(TIMERGROUP, sleepTimer, TIMERVALUE(delay * 60)));
	ESP_ERROR_CHECK(timer_enable_intr(TIMERGROUP, sleepTimer));
	ESP_ERROR_CHECK(timer_set_alarm(TIMERGROUP, sleepTimer, TIMER_ALARM_EN));
	ESP_ERROR_CHECK(timer_start(TIMERGROUP, sleepTimer));
	tsocket("lsleep", delay);
}

void stopWake()
{
	ESP_LOGD(TAG, "stopWake");
	ESP_ERROR_CHECK(timer_pause(TIMERGROUP, wakeTimer));
	ESP_ERROR_CHECK(timer_set_counter_value(TIMERGROUP, wakeTimer, 0x00000000ULL));
	ESP_ERROR_CHECK(timer_set_alarm_value(TIMERGROUP, wakeTimer, 0x00000000ULL));
	tsocket("lwake", 0);
}
void startWake(uint32_t delay)
{
	ESP_LOGD(TAG, "startWake: %d min.", delay);
	if (delay == 0)
		return;
	stopWake();
	ESP_ERROR_CHECK(timer_set_counter_value(TIMERGROUP, wakeTimer, 0x00000000ULL));
	ESP_ERROR_CHECK(timer_set_alarm_value(TIMERGROUP, wakeTimer, TIMERVALUE(delay * 60)));
	ESP_ERROR_CHECK(timer_enable_intr(TIMERGROUP, wakeTimer));
	ESP_ERROR_CHECK(timer_set_alarm(TIMERGROUP, wakeTimer, TIMER_ALARM_EN));
	ESP_ERROR_CHECK(timer_start(TIMERGROUP, wakeTimer));
	tsocket("lwake", delay);
}
#endif

void initTimers()
{
	event_queue = xQueueCreate(10, sizeof(queue_event_t));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	gptimer_config_t timer_config = {
		.clk_src = GPTIMER_CLK_SRC_APB,
		.direction = GPTIMER_COUNT_DOWN,
		.resolution_hz = 10 * 1000, // 100 kHz resolution (1 tick = 10 µs)
	};
	gptimer_alarm_config_t alarm_config = {
		.reload_count = 5,					// counter will reload with 0 on alarm event
		.alarm_count = 0,					// period = 1000µs
		.flags.auto_reload_on_alarm = true, // Enable auto-reload
	};

	/*Configure timer 1MS*/
	//////////////////////
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &mstimer));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(mstimer, &alarm_config));
	gptimer_event_callbacks_t cbs = {
		.on_alarm = msCallback, // Register user callback
	};
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(mstimer, &cbs, event_queue));
	ESP_ERROR_CHECK(gptimer_enable(mstimer)); // Enable the timer
	ESP_ERROR_CHECK(gptimer_start(mstimer));  // Start the timer

	// Configure sleep and wake timers (if needed)
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &sleeptimer));
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &waketimer));

#else
	// Timer configuration for ESP-IDF < 5.0
	timer_config_t config = {
		.alarm_en = 1,
		.auto_reload = TIMER_AUTORELOAD_EN,
		.counter_dir = TIMER_COUNT_UP,
		.divider = TIMER_DIVIDER1MS, // Divider for 1 ms resolution
		.intr_type = TIMER_INTR_LEVEL,
		.counter_en = TIMER_PAUSE,
	};

	// Configure sleep timer
	ESP_ERROR_CHECK(timer_init(TIMERGROUP, sleepTimer, &config));
	ESP_ERROR_CHECK(timer_pause(TIMERGROUP, sleepTimer));
	ESP_ERROR_CHECK(timer_isr_register(TIMERGROUP, sleepTimer, sleepCallback, (void *)sleepTimer, 0, NULL));

	// Configure wake timer
	ESP_ERROR_CHECK(timer_init(TIMERGROUP, wakeTimer, &config));
	ESP_ERROR_CHECK(timer_pause(TIMERGROUP, wakeTimer));
	ESP_ERROR_CHECK(timer_isr_register(TIMERGROUP, wakeTimer, wakeCallback, (void *)wakeTimer, 0, NULL));
	// Configure 1 ms timer
	ESP_ERROR_CHECK(timer_init(TIMERGROUP1MS, msTimer, &config));
	ESP_ERROR_CHECK(timer_pause(TIMERGROUP1MS, msTimer));
	ESP_ERROR_CHECK(timer_isr_register(TIMERGROUP1MS, msTimer, msCallback, (void *)msTimer, 0, NULL));
	/* start 1MS timer*/
	ESP_ERROR_CHECK(timer_set_counter_value(TIMERGROUP1MS, msTimer, 0x00000000ULL));
	ESP_ERROR_CHECK(timer_set_alarm_value(TIMERGROUP1MS, msTimer, TIMERVALUE1MS(1))); // 10 ms alarm
	ESP_ERROR_CHECK(timer_enable_intr(TIMERGROUP1MS, msTimer));
	ESP_ERROR_CHECK(timer_set_alarm(TIMERGROUP1MS, msTimer, TIMER_ALARM_EN));
	ESP_ERROR_CHECK(timer_start(TIMERGROUP1MS, msTimer));
#endif
}
//////////////////////////////////////////////////////////////////

// Renderer config creation
static renderer_config_t *create_renderer_config()
{
	renderer_config_t *renderer_config = kcalloc(1, sizeof(renderer_config_t));
	if (!renderer_config)
	{
		ESP_LOGE(TAG, "Failed to allocate memory for renderer config");
		return NULL;
	}

	if (renderer_config->output_mode == I2S_MERUS || renderer_config->output_mode == I2S_32BIT)
	{
		renderer_config->bit_depth = I2S_BITS_PER_SAMPLE_32BIT;
	}

	if (renderer_config->output_mode == DAC_BUILT_IN)
	{
		renderer_config->bit_depth = I2S_BITS_PER_SAMPLE_16BIT;
	}
	return renderer_config;
}

/******************************************************************************
 * FunctionName : checkUart
 * Description  : Check for a valid uart baudrate
 * Parameters   : baud
 * Returns      : baud
 *******************************************************************************/
uint32_t checkUart(uint32_t speed)
{
	uint32_t valid[] = {1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 76880, 115200, 230400};
	int i;
	for (i = 0; i < 12; i++)
	{
		if (speed == valid[i])
			return speed;
	}
	return 115200; // default
}

/******************************************************************************
 * FunctionName : init_hardware
 * Description  : Init all hardware, partitions etc
 * Parameters   :
 * Returns      :
 *******************************************************************************/
static void init_hardware()
{
#ifndef CONFIG_NO_VS1053
	if (VS1053_HW_init()) // init spi
		VS1053_Start();
#endif
	ESP_LOGV(TAG, "hardware initialized");
}
void ledBlinkTask(void *p)
{
	uint32_t FlashOn = 0;
	uint32_t FlashOff = 0;
	uint32_t cCur = 0;
	bool stateLed = false;
	bool recalculate = true; // Flag to trigger recalculation
	uint8_t last_brightness = led_brightness;
	while (1)
	{
		// Recalculate FlashOn and FlashOff only at the start of a new cycle
		if (recalculate)
		{
			FlashOn = (cycleDuration * onRatio) / 100;					 // Time LED is ON (in ms)
			FlashOff = (onRatio == 100) ? 0 : (cycleDuration - FlashOn); // Time LED is OFF (in ms)
			cCur = (stateLed ? FlashOn : FlashOff);						 // Set the current duration
			recalculate = false;										 // Reset the flag
		}
#if CONFIG_BLINK_LED_RMT
		// Handle addressable LED blinking
		if (led_strip && (ctimeMs >= cCur))
		{
			if (onRatio == 100)
			{
				// Keep the LED ON continuously
				setLedColor(led_color, led_brightness);
				stateLed = false;	// Keep the LED ON
				recalculate = true; // Recalculate in case onRatio changes
				ctimeMs = 0;		// Reset the millisecond counter
				continue;
			}
			if (stateLed)
			{
				// Turn LED OFF
				ESP_ERROR_CHECK(led_strip->set_pixel(led_strip, 0, 0, 0, 0)); // RGB(0, 0, 0)
				ESP_ERROR_CHECK(led_strip->refresh(led_strip, 100));
				stateLed = false;
				cCur = FlashOff;
			}
			else
			{
				// Turn LED ON with the current color
				setLedColor(led_color, led_brightness);
				stateLed = true;
				cCur = FlashOn;
			}
			recalculate = true;
			ctimeMs = 0;
		}
#else
		// Handle standard GPIO LED blinking with PWM and polarity
		if (last_brightness != g_device->led_brightness)
		{
			setLedColorWrapper(NULL, led_brightness); // Update LED brightness
			last_brightness = led_brightness;
		}
		if (onRatio == 100)
		{
			// Keep the LED ON continuously
			if (!stateLed)
			{
				stateLed = true;
				setLedOnOff(true);
			}
			recalculate = true;
			ctimeMs = 0;
			vTaskDelay(pdMS_TO_TICKS(25));
			continue;
		}
		if (ctimeMs >= cCur)
		{
			if (stateLed)
			{
				// Turn LED OFF
				stateLed = false;
				setLedOnOff(false);
				cCur = FlashOff;
			}
			else
			{
				// Turn LED ON
				stateLed = true;
				setLedOnOff(true);
				cCur = FlashOn;
			}
			recalculate = true;
			ctimeMs = 0;
		}
#endif

		vTaskDelay(pdMS_TO_TICKS(25)); // Delay to allow other tasks to run
	}

	vTaskDelete(NULL); // Stop the task (never reached)
}
/* event handler for pre-defined wifi events */
// static esp_err_t event_handler(void *ctx, system_event_t *event)
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
							   int32_t event_id, void *event_data)
{
	extern bool _reboot; // reboot requested status
	switch (event_id)
	{
	case WIFI_EVENT_STA_START:
		esp_wifi_connect();
		break;

	case WIFI_EVENT_STA_CONNECTED:
		xEventGroupSetBits(wifi_event_group, CONNECTED_AP);
		ESP_LOGV(TAG, "Wifi connected");
		setLedColorWrapper(led_yellow, led_brightness);
		if (wifiInitDone)
		{
			clientSaveOneHeader("Wifi Connected.", 18, METANAME);
			vTaskDelay(pdMS_TO_TICKS(2000));
			autoPlay();
		} // retry
		/*else */ wifiInitDone = true;
		break;

	case IP_EVENT_STA_GOT_IP:
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		setLedColorWrapper(led_lgreen, led_brightness);
		onRatio = 20;
		break;

	case WIFI_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently
		   auto-reassociate. */
		xEventGroupClearBits(wifi_event_group, CONNECTED_AP);
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		ESP_LOGE(TAG, "Wifi Disconnected.");
		setLedColorWrapper(led_red, led_brightness);
		onRatio = 80;
		vTaskDelay(pdMS_TO_TICKS(500));
		if (!getAutoWifi() && (wifiInitDone))
		{
			if (_isRadio)
			{
				ESP_LOGE(TAG, "reboot");
				vTaskDelay(pdMS_TO_TICKS(500));
				i2s_driver_uninstall(I2S_NUM_0); // Uninstall I2S driver
				gpio_set_level(PIN_I2S_DATA, 0); // Set data pin low
				vTaskDelay(pdMS_TO_TICKS(50));	 // Wait for output to settle
				esp_restart();
			}
		}
		else if (!_reboot)
		{
			if (wifiInitDone) // a completed init done
			{
				ESP_LOGE(TAG, "Connection tried again");
				//				clientDisconnect("Wifi Disconnected.");
				clientSilentDisconnect();
				vTaskDelay(pdMS_TO_TICKS(500));
				clientSaveOneHeader("Wifi Disconnected.", 18, METANAME);
				vTaskDelay(pdMS_TO_TICKS(500));
				while (esp_wifi_connect() == ESP_ERR_WIFI_SSID)
					vTaskDelay(pdMS_TO_TICKS(50));
			}
			else
			{
				ESP_LOGE(TAG, "Try next AP");
				vTaskDelay(pdMS_TO_TICKS(500));
			} // init failed?
		}
		break;

	case WIFI_EVENT_AP_START:
		xEventGroupSetBits(wifi_event_group, CONNECTED_AP);
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		wifiInitDone = true;
		setLedColorWrapper(led_orange, led_brightness);
		onRatio = 30;
		break;

	case WIFI_EVENT_AP_STADISCONNECTED:
		break;

	default:
		break;
	}
	//   return ESP_OK;
}

static void unParse(char *str)
{
	int i;
	if (str == NULL)
		return;
	for (i = 0; i < strlen(str); i++)
	{
		if (str[i] == '\\')
		{
			str[i] = str[i + 1];
			str[i + 1] = 0;
			if (str[i + 2] != 0)
				strcat(str, str + i + 2);
		}
	}
}

static void start_wifi()
{
	ESP_LOGV(TAG, "starting wifi");
	setAutoWifi();
	//	wifi_mode_t mode;
	char ssid[SSIDLEN];
	char pass[PASSLEN];

	static bool first_pass = false;
	static bool initialized = false;
	if (!initialized)
	{
		esp_netif_init();
		wifi_event_group = xEventGroupCreate();
		ESP_ERROR_CHECK(esp_event_loop_create_default());
		ap = esp_netif_create_default_wifi_ap();
		assert(ap);
		sta = esp_netif_create_default_wifi_sta();
		assert(sta);
		wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
		ESP_ERROR_CHECK(esp_wifi_init(&cfg));
		ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
		ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
		ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
		initialized = true;
	}
	ESP_LOGI(TAG, "WiFi init done!");

	if (g_device->current_ap == APMODE)
	{
		if (strlen(g_device->ssid1) != 0)
		{
			g_device->current_ap = STA1;
		}
		else
		{
			if (strlen(g_device->ssid2) != 0)
				g_device->current_ap = STA2;
			else
				g_device->current_ap = APMODE;
		}
		saveDeviceSettings(g_device);
	}

	while (1)
	{
		if (first_pass)
		{
			ESP_ERROR_CHECK(esp_wifi_stop());
			vTaskDelay(pdMS_TO_TICKS(50));
		}

		switch (g_device->current_ap)
		{
		case STA1: // ssid1 used
			strcpy(ssid, g_device->ssid1);
			strcpy(pass, g_device->pass1);
			esp_wifi_set_mode(WIFI_MODE_STA);
			break;
		case STA2: // ssid2 used
			strcpy(ssid, g_device->ssid2);
			strcpy(pass, g_device->pass2);
			esp_wifi_set_mode(WIFI_MODE_STA);
			break;

		default: // other: AP mode
			g_device->current_ap = APMODE;
			ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
		}

		if (g_device->current_ap == APMODE)
		{
			printf("WIFI GO TO AP MODE\n");
			wifi_config_t ap_config = {
				.ap = {
					.ssid = "WifiKaradio",
					.authmode = WIFI_AUTH_OPEN,
					.max_connection = 2,
					.beacon_interval = 200},
			};
			ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
			ESP_LOGE(TAG, "The default AP is  WifiKaRadio. Connect your wifi to it.\nThen connect a webbrowser to 192.168.4.1 and go to Setting\nMay be long to load the first time.Be patient.");

			vTaskDelay(pdMS_TO_TICKS(10));
			ESP_ERROR_CHECK(esp_wifi_start());

			//			audio_output_mode = I2S;
			option_get_audio_output(&audio_output_mode);
		}
		else
		{
			printf("WIFI TRYING TO CONNECT TO SSID %d\n", g_device->current_ap);
			wifi_config_t wifi_config = {
				.sta = {
					.bssid_set = 0,
					.scan_method = WIFI_ALL_CHANNEL_SCAN,
					.sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
				},
			};
			strcpy((char *)wifi_config.sta.ssid, ssid);
			strcpy((char *)wifi_config.sta.password, pass);
			unParse((char *)(wifi_config.sta.ssid));
			unParse((char *)(wifi_config.sta.password));
			if (strlen(ssid) /*&&strlen(pass)*/)
			{
				if (CONNECTED_BIT > 1)
				{
					esp_wifi_disconnect();
				}
				ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

				ESP_LOGI(TAG, "connecting %s", ssid);
				ESP_ERROR_CHECK(esp_wifi_start());
			}
			else
			{
				g_device->current_ap++;
				g_device->current_ap %= 3;

				if (getAutoWifi() && (g_device->current_ap == APMODE))
				{
					if (fgetc(stdin) == 0xFF)		 // if a char read, stop the autowifi
						g_device->current_ap = STA1; // if autoWifi then wait for a reconnection to an AP
					ESP_LOGI(TAG, "Wait for the AP");
				}
				else
					ESP_LOGI(TAG, "Empty AP. Try next one");

				saveDeviceSettings(g_device);
				continue;
			}
		}

		/* Wait for the callback to set the CONNECTED_BIT in the event group. */
		if ((xEventGroupWaitBits(wifi_event_group, CONNECTED_AP, false, true, 8000) & CONNECTED_AP) == 0)
		// timeout . Try the next AP
		{
			g_device->current_ap++;
			g_device->current_ap %= 3;
			if (getAutoWifi() && (g_device->current_ap == APMODE))
			{
				char inp = fgetc(stdin);
				printf("\nfgetc : %x\n", inp);
				if (inp == 0xFF)				 //
					g_device->current_ap = STA1; // if a char read, stop the autowifi
			}
			saveDeviceSettings(g_device);
			ESP_LOGI(TAG, "device->current_ap: %d", g_device->current_ap);
		}
		else
			break; //
		first_pass = true;
	}
}

void start_network()
{
	//	struct device_settings *g_device;
	esp_netif_ip_info_t info;
	wifi_mode_t mode;
	ip4_addr_t ipAddr;
	ip4_addr_t mask;
	ip4_addr_t gate;
	uint8_t dhcpEn = 0;

	IP4_ADDR(&ipAddr, 192, 168, 4, 1);
	IP4_ADDR(&gate, 192, 168, 4, 1);
	IP4_ADDR(&mask, 255, 255, 255, 0);

	esp_netif_dhcpc_stop(sta); // Don't run a DHCP client

	switch (g_device->current_ap)
	{
	case STA1: // ssid1 used
		IP4_ADDR(&ipAddr, g_device->ipAddr1[0], g_device->ipAddr1[1], g_device->ipAddr1[2], g_device->ipAddr1[3]);
		IP4_ADDR(&gate, g_device->gate1[0], g_device->gate1[1], g_device->gate1[2], g_device->gate1[3]);
		IP4_ADDR(&mask, g_device->mask1[0], g_device->mask1[1], g_device->mask1[2], g_device->mask1[3]);
		dhcpEn = g_device->dhcpEn1;
		break;
	case STA2: // ssid2 used
		IP4_ADDR(&ipAddr, g_device->ipAddr2[0], g_device->ipAddr2[1], g_device->ipAddr2[2], g_device->ipAddr2[3]);
		IP4_ADDR(&gate, g_device->gate2[0], g_device->gate2[1], g_device->gate2[2], g_device->gate2[3]);
		IP4_ADDR(&mask, g_device->mask2[0], g_device->mask2[1], g_device->mask2[2], g_device->mask2[3]);
		dhcpEn = g_device->dhcpEn2;
		break;

	default: // other: AP mode
		IP4_ADDR(&ipAddr, 192, 168, 4, 1);
		IP4_ADDR(&gate, 192, 168, 4, 1);
		IP4_ADDR(&mask, 255, 255, 255, 0);
	}

	ip4_addr_copy(info.ip, ipAddr);
	ip4_addr_copy(info.gw, gate);
	ip4_addr_copy(info.netmask, mask);

	ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
	if (mode == WIFI_MODE_AP)
	{
		xEventGroupWaitBits(wifi_event_group, CONNECTED_AP, false, true, 3000);
		ip4_addr_copy(info.ip, ipAddr);

		esp_netif_set_ip_info(ap, &info);
		esp_netif_ip_info_t ap_ip_info;
		ap_ip_info.ip.addr = 0;
		while (ap_ip_info.ip.addr == 0)
		{
			esp_netif_get_ip_info(ap, &ap_ip_info);
		}
	}
	else // mode STA
	{
		if (dhcpEn)						// check if ip is valid without dhcp
			esp_netif_dhcpc_start(sta); //  run a DHCP client
		else
		{
			ESP_ERROR_CHECK(esp_netif_set_ip_info(sta, &info));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			dns_clear_cache();
#else
			dns_clear_servers(false);
#endif
#ifndef CONFIG_IDF_TARGET_ESP32S3

			ip_addr_t temp_gw;
			memcpy(&temp_gw, &info.gw, sizeof(ip4_addr_t)); // Copy the gateway address
			IP_SET_TYPE(&temp_gw, IPADDR_TYPE_V4);			// Set the type safely
			memcpy(&info.gw, &temp_gw, sizeof(ip4_addr_t)); // Copy it back
															// IP_SET_TYPE(((ip_addr_t *)&info.gw), IPADDR_TYPE_V4); // mandatory
															//			(( ip_addr_t* )&info.gw)->type = IPADDR_TYPE_V4;

#else // ## its ESP32S3
			ip_addr_t temp_gw;
			memcpy(&temp_gw, &info.gw, sizeof(ip4_addr_t)); // Copy the gateway address
			IP_SET_TYPE(&temp_gw, IPADDR_TYPE_V4);			// Set the type safely
			memcpy(&info.gw, &temp_gw, sizeof(ip4_addr_t)); // Copy it back
#endif
			dns_setserver(0, (ip_addr_t *)&info.gw);
			dns_setserver(1, (ip_addr_t *)&info.gw); // if static ip	check dns
		}

		// wait for ip
		if ((xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 8000) & CONNECTED_BIT) == 0) // timeout
		{																									// enable dhcp and restart
			if (g_device->current_ap == 1)
				g_device->dhcpEn1 = 1;
			else
				g_device->dhcpEn2 = 1;
			saveDeviceSettings(g_device);
			i2s_driver_uninstall(I2S_NUM_0); // Uninstall I2S driver (if using I2S_NUM_0)
			gpio_set_level(PIN_I2S_DATA, 0); // Set data pin low
			vTaskDelay(pdMS_TO_TICKS(50));	 // Wait for output to settle
			esp_restart();
		}

		vTaskDelay(pdMS_TO_TICKS(50));
		// retrieve the current ip
		esp_netif_ip_info_t sta_ip_info;
		sta_ip_info.ip.addr = 0;
		while (sta_ip_info.ip.addr == 0)
		{
			esp_netif_get_ip_info(sta, &sta_ip_info);
		}

		ip_addr_t *ipdns0 = (ip_addr_t *)dns_getserver(0);
		//		ip_addr_t ipdns1 = dns_getserver(1);
		ESP_LOGW(TAG, "DNS: %s  \n", ip4addr_ntoa((struct ip4_addr *)&ipdns0));

		if (dhcpEn) // if dhcp enabled update fields
		{
			esp_netif_get_ip_info(sta, &info);
			switch (g_device->current_ap)
			{
			case STA1: // ssid1 used
				ip4addr_aton((const char *)&g_device->ipAddr1, (ip4_addr_t *)&info.ip);
				ip4addr_aton((const char *)&g_device->gate1, (ip4_addr_t *)&info.gw);
				ip4addr_aton((const char *)&g_device->mask1, (ip4_addr_t *)&info.netmask);
				break;

			case STA2: // ssid2 used
				ip4addr_aton((const char *)&g_device->ipAddr2, (ip4_addr_t *)&info.ip);
				ip4addr_aton((const char *)&g_device->gate2, (ip4_addr_t *)&info.gw);
				ip4addr_aton((const char *)&g_device->mask2, (ip4_addr_t *)&info.netmask);
				break;
			}
		}
		saveDeviceSettings(g_device);
		esp_netif_set_hostname(sta, "karadio32");
	}
	ip4_addr_copy(ipAddr, info.ip);
	strcpy(localIp, ip4addr_ntoa(&ipAddr));
	ESP_LOGW(TAG, "IP: %s\n\n", localIp);

	lcd_welcome(localIp, "IP found");
	vTaskDelay(pdMS_TO_TICKS(100));
}
// blinking led and timer isr
IRAM_ATTR void timerTask(void *p)
{
	bool isEsplay;
	isEsplay = option_get_esplay();
	initTimers();
	queue_event_t evt;
	while (1)
	{
		while (xQueueReceive(event_queue, &evt, 0))
		{
			if (evt.type != TIMER_1MS)
				printf("evt.type: %d\n", evt.type);
			switch (evt.type)
			{
			case TIMER_1MS:
				if (isEsplay)				  // esplay board only
					rexp = i2c_keypad_read(); // read the expansion
				if (divide)
					ctimeMs++; // For LED timing
				divide = !divide;
				ServiceAddon();
				break;
			case TIMER_SLEEP:
				clientDisconnect("Timer"); // Stop the player
				break;
			case TIMER_WAKE:
				clientConnect(); // Start the player
				break;
			default:
				break;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(10)); // Delay to allow other tasks to run
	}

	vTaskDelete(NULL); // Stop the task (never reached)
}

void uartIfaceTask(void *pvParameters)
{
	char tmp[255];
	if (tmp == NULL)
	{
		ESP_LOGE("uartIfaceTask", "Failed to allocate memory for tmp buffer");
		vTaskDelete(NULL);
		return;
	}
	int d;
	uint8_t c;
	int t;
	esp_err_t err;
	//	struct device_settings *device;
	uint32_t uspeed;
	int uxHighWaterMark;
	uspeed = g_device->uartspeed;
	uart_config_t uart_config0 = {
		.baud_rate = uspeed,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE, // UART_HW_FLOWCTRL_CTS_RTS,
		.rx_flow_ctrl_thresh = 0,
	};
	err = uart_param_config(UART_NUM_0, &uart_config0);
	if (err != ESP_OK)
		ESP_LOGE("uartIfaceTask", "uart_param_config err: %d", err);

	err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, ESP_INTR_FLAG_IRAM);
	if (err != ESP_OK)
	{
		ESP_LOGE("uartIfaceTask", "uart_driver_install err: %d", err);
		vTaskDelete(NULL);
	}

	for (t = 0; t < sizeof(tmp); t++)
		tmp[t] = 0;
	t = 0;

	while (1)
	{
		while (1)
		{
			d = uart_read_bytes(UART_NUM_0, &c, 1, 100);
			if (d > 0)
			{
				if ((char)c == '\r')
					break;
				if ((char)c == '\n')
					break;
				tmp[t] = (char)c;
				t++;
				if (t == sizeof(tmp) - 1)
					t = 0;
			}
			// else printf("uart d: %d, T= %d\n",d,t);
			// switchCommand() ;  // hardware panel of command
		}
		checkCommand(t, tmp);
		uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
		ESP_LOGD("uartIfaceTask", striWATERMARK, uxHighWaterMark, xPortGetFreeHeapSize());

		for (t = 0; t < sizeof(tmp); t++)
			tmp[t] = 0;
		t = 0;
	}
}

// In STA mode start a station or start in pause mode.
// Show ip on AP mode.
void autoPlay()
{

	char apmode[50];
	sprintf(apmode, "at IP %s", localIp);
	if (g_device->current_ap == APMODE)
	{
		clientSaveOneHeader("Configure the AP with the web page", 34, METANAME);
		clientSaveOneHeader(apmode, strlen(apmode), METAGENRE);
	}
	else
	{
		clientSaveOneHeader(apmode, strlen(apmode), METANAME);
#ifndef CONFIG_NO_VS1053
		if ((audio_output_mode == VS1053) && (getVsVersion() < 3))
		{
			clientSaveOneHeader("Invalid audio output. VS1053 not found", 38, METAGENRE);
			ESP_LOGE(TAG, "Invalid audio output. VS1053 not found");
			vTaskDelay(pdMS_TO_TICKS(200));
		}
#endif

		setCurrentStation(g_device->currentstation);
		// if (g_device->autostart > 1)
		// 	g_device->autostart = 1;
		if ((g_device->autostart == 1) && (g_device->currentstation != 0xFFFF))
		{
			if (getLogLevel() >= ESP_LOG_DEBUG)
				kprintf("autostart setting playing:%d, currentstation:%d\n", g_device->autostart, g_device->currentstation);
			vTaskDelay(pdMS_TO_TICKS(10)); // wait a bit
			playStationInt(g_device->currentstation);
		}
		else
		{
			kprintf("autostart: playing:%d, currentstation:%d\n", g_device->autostart, g_device->currentstation);
			clientSaveOneHeader("Ready", 5, METANAME);
		}
	}
}
#if CONFIG_BT_SPEAKER_MODE
void BTstart()
{
	// init player config
	player_config = (player_t *)kcalloc(1, sizeof(player_t));
	player_config->command = CMD_NONE;
	player_config->decoder_status = UNINITIALIZED;
	player_config->decoder_command = CMD_NONE;
	player_config->buffer_pref = BUF_PREF_SAFE;
	player_config->media_stream = kcalloc(1, sizeof(media_stream_t));
	player_config->media_stream->content_type = AUDIO_BT_PCM; // default content type for BT

	audio_player_init(player_config);
	extern player_t *player;
	player = player_config;
	// Start Bluetooth
	bt_speaker_start(create_renderer_config());
}
#endif // CONFIG_BT_SPEAKER_MODE
/**
 * Main entry point
 */
void app_main()
{
	uint32_t uspeed;
	xTaskHandle pxCreatedTask;
	esp_err_t err;
	vTaskDelay(pdMS_TO_TICKS(200));
	ESP_LOGI(TAG, "starting app_main()");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	ESP_LOGD(TAG, "RAM left: %lu", esp_get_free_heap_size());
#else
	ESP_LOGI(TAG, "RAM left: %d", esp_get_free_heap_size());
#endif

	const esp_partition_t *running = esp_ota_get_running_partition();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	ESP_LOGD(TAG, "Running partition type %d subtype %d (offset 0x%lx)",
			 running->type, running->subtype, running->address);
#else
	ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%x)",
			 running->type, running->subtype, running->address);
#endif
	// Initialize NVS.
	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES)
	{
		// OTA app partition table has a smaller NVS partition size than the non-OTA
		// partition table. This size mismatch may cause NVS initialization to fail.
		// If this happens, we erase NVS partition and initialize NVS again.
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	/* Log wakeup reason at boot; do NOT store into NVS (RTC/NVS mismatch and not needed). */
	{
		esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
		char reason[128];
		gpio_num_t sleep_pin = GPIO_NONE;
		bool sleep_level = 0;
		gpio_get_pinSleep(&sleep_pin, &sleep_level); // read configured wake pin

		switch (cause)
		{
		case ESP_SLEEP_WAKEUP_EXT0:
			snprintf(reason, sizeof(reason), "EXT0: pin=%d level=%d", (int)sleep_pin, sleep_level ? 1 : 0);
			break;
		case ESP_SLEEP_WAKEUP_EXT1:
			snprintf(reason, sizeof(reason), "EXT1: mask (check ext1 status)");
			break;
		case ESP_SLEEP_WAKEUP_TIMER:
			snprintf(reason, sizeof(reason), "TIMER");
			break;
		case ESP_SLEEP_WAKEUP_TOUCHPAD:
			snprintf(reason, sizeof(reason), "TOUCHPAD");
			break;
		case ESP_SLEEP_WAKEUP_ULP:
			snprintf(reason, sizeof(reason), "ULP");
			break;
		case ESP_SLEEP_WAKEUP_UNDEFINED:
		default:
			snprintf(reason, sizeof(reason), "RESET or POWER ON or UNKNOWN (%d)", (int)cause);
			break;
		}

		ESP_LOGI(TAG, "Wake reason: %s", reason);
	}

	// Check if we are in large Sram config
	if (xPortGetFreeHeapSize() > 0x80000)
		bigRam = true;
	// init hardware
	partitions_init();
	ESP_LOGI(TAG, "Partition init done...PortGetFreeheap %d, bigRam %d", xPortGetFreeHeapSize(), bigRam);

	if (g_device->cleared != 0xAABB)
	{
		ESP_LOGE(TAG, "Device config not ok. Try to restore");
		free(g_device);
		restoreDeviceSettings(); // try to restore the config from the saved one
		g_device = getDeviceSettings();
		if (g_device->cleared != 0xAABB)
		{
			ESP_LOGE(TAG, "Device config not cleared. Clear it.");
			free(g_device);
			eeEraseAll();
			g_device = getDeviceSettings();
			g_device->cleared = 0xAABB;	  // marker init done
			g_device->uartspeed = 115200; // default
										  //			g_device->audio_output_mode = I2S; // default
			option_get_audio_output(&(g_device->audio_output_mode));
			g_device->trace_level = ESP_LOG_ERROR; // default
			g_device->vol = 100;				   // default
			g_device->led_gpio = GPIO_NONE;
			g_device->led_brightness = 0xFF; // uninitialized
			saveDeviceSettings(g_device);
		}
		else
			ESP_LOGE(TAG, "Device config restored");
	}

	copyDeviceSettings(); // copy in the safe partion

	ESP_LOGI(TAG, "Initializing status LED on GPIO %d", g_device->led_gpio);
	initStatusLED();
	// Configure Deep Sleep start and wakeup options
	deepSleepConf(); // also called in addon.c
	// Hardware-triggered deep sleep disabled; deep sleep will be entered only by command or encoder long-press
	// if (checkDeepSleepInput())
	// 	esp_deep_sleep_start();

	// led mode
	if (g_device->options & T_LED)
		ledStatus = false; // play mode
	else
		ledStatus = true; // blink mode

	if (g_device->options & T_LEDPOL)
		ledPolarity = true;
	else
		ledPolarity = false;
	if (g_device->led_brightness < 1 || g_device->led_brightness > 100)
	{
		ESP_LOGW(TAG, "LED brightness not set. Set to default");
		g_device->led_brightness = led_brightnes; // default
		saveDeviceSettings(g_device);
	}
	led_brightness = g_device->led_brightness;
	// log on telnet
	if (g_device->options & T_LOGTEL)
		logTel = true; //
	else
		logTel = false; //
						// init softwares
#ifndef CONFIG_NO_TELNET
	telnetinit();
#endif
	websocketinit();

	// log level
	setLogLevel(g_device->trace_level);
	// time display
	uint8_t ddmm;
	option_get_ddmm(&ddmm);
	setDdmm(ddmm ? 1 : 0);
	ESP_LOGW("RESET", "Reset reason: %d", esp_reset_reason());
	// ESP_LOGI(TAG, "Hardware init starting... Volume: %d Brightness: %d", g_device->vol, g_device->led_brightness);
	// vTaskDelay(pdMS_TO_TICKS(10));
	// SPI init for the vs1053 and lcd if spi.
	spi_init();

	init_hardware();

	// the esplay board needs I2C for gpio extension
	if (option_get_esplay())
	{
		gpio_num_t scl;
		gpio_num_t sda;
		gpio_num_t rsti2c;
		gpio_get_i2c(&scl, &sda, &rsti2c);
		ESP_LOGD(TAG, "I2C GPIO SDA: %d, SCL: %d", sda, scl);
		i2c_config_t conf;
		conf.mode = I2C_MODE_MASTER;
		conf.sda_io_num = sda;
		conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
		conf.scl_io_num = scl;
		conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
		conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
		esp_err_t res = i2c_param_config(I2C_MASTER_NUM, &conf);
		ESP_LOGD(TAG, "I2C setup : %d\n", res);
		res = i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
		if (res != 0)
			ESP_LOGD(TAG, "I2C already installed. No problem");
		else
			ESP_LOGD(TAG, "I2C installed: %d", res);

		// init the amp shutdown gpio4 as output level 1
		gpio_output_conf(PIN_AUDIO_SHDN);
	}

	// output mode
	// I2S, I2S_MERUS, DAC_BUILT_IN, PDM, VS1053
	audio_output_mode = g_device->audio_output_mode;
	ESP_LOGI(TAG, "audio_output_mode %d\nOne of I2S, I2S_MERUS, DAC_BUILT_IN, PDM, VS1053, SPDIF, I2S32BIT", audio_output_mode);

	// uart speed
	uspeed = g_device->uartspeed;
	uspeed = checkUart(uspeed);
	uart_set_baudrate(UART_NUM_0, uspeed);
	ESP_LOGI(TAG, "Set baudrate at %d", uspeed);
	if (g_device->uartspeed != uspeed)
	{
		g_device->uartspeed = uspeed;
		saveDeviceSettings(g_device);
	}
#if CONFIG_BT_SPEAKER_MODE
	char *dev_name = g_device->BTname;
	if (dev_name == NULL || strlen(dev_name) == 0)
	{
		dev_name = CONFIG_BT_NAME; // default name from Kconfig
		strcpy(g_device->BTname, dev_name);
		saveDeviceSettings(g_device); // save default name
	}
	if ((g_device->BTpass == NULL || strlen(g_device->BTpass) == 0))
	{
		strcpy(g_device->BTpass, "1234"); // default pass
		saveDeviceSettings(g_device);	  // save default pass
	}
#endif // CONFIG_BT_SPEAKER_MODE
	// Version infos
	ESP_LOGI(TAG, "\n");
	ESP_LOGI(TAG, "Project name: %s", esp_ota_get_app_description()->project_name);
	ESP_LOGI(TAG, "Version: %s", esp_ota_get_app_description()->version);
	ESP_LOGI(TAG, "Release %s, Revision %s", RELEASE, REVISION);
	ESP_LOGI(TAG, "Date: %s,  Time: %s", esp_ota_get_app_description()->date, esp_ota_get_app_description()->time);
	ESP_LOGI(TAG, "SDK %s", esp_get_idf_version());
	ESP_LOGI(TAG, " Date %s, Time: %s\n", __DATE__, __TIME__);
	// lcd init
	uint8_t rt;
	option_get_lcd_info(&g_device->lcd_type, &rt);
	ESP_LOGI(TAG, "LCD Type %d", g_device->lcd_type);
	// lcd rotation
	setRotat(rt);
	lcd_init(g_device->lcd_type);
	ESP_LOGI(TAG, "Hardware init done...");

	lcd_welcome("", "");
	lcd_welcome("", "STARTING");
	// volume
	setIvol(g_device->vol);
	ESP_LOGI(TAG, "Volume set to %d", g_device->vol);

	xTaskCreatePinnedToCore(timerTask, "timerTask", 1920, NULL, PRIO_TIMER, &pxCreatedTask, CPU_TIMER); // #memchange 2176
	ESP_LOGI(TAG, "%s task: %x", "t0", (unsigned int)pxCreatedTask);

	xTaskCreatePinnedToCore(uartIfaceTask, "uartIfaceTask", 3072, NULL, PRIO_UART, &pxCreatedTask, CPU_UART); // #memchange 3072
	ESP_LOGI(TAG, "%s task: %x", "uartIfaceTask", (unsigned int)pxCreatedTask);

	// init player config
	player_config = (player_t *)kcalloc(1, sizeof(player_t));
	player_config->command = CMD_NONE;
	player_config->decoder_status = UNINITIALIZED;
	player_config->decoder_command = CMD_NONE;
	player_config->buffer_pref = BUF_PREF_SAFE;
	player_config->media_stream = kcalloc(1, sizeof(media_stream_t));

	audio_player_init(player_config);
	renderer_init(create_renderer_config());

Option_test:
	ESP_LOGI(TAG, "Option test: %d", g_device->options);
	if (IS_RADIO(g_device->options))
	{
		ESP_LOGI(TAG, "Running in radio mode");
		_isRadio = true;

		//-----------------------------
		// start the network
		//-----------------------------
		/* init wifi & network*/
		start_wifi();
		start_network();

		//-----------------------------------------------------
		// init softwares
		//-----------------------------------------------------
		clientInit();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());
#else
		ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());
#endif
		// initialize mDNS service
		err = mdns_init();
		if (err)
			ESP_LOGE(TAG, "mDNS Init failed: %d", err);
		else
			ESP_LOGI(TAG, "mDNS Init ok");

		// set hostname and instance name
		if ((strlen(g_device->hostname) == 0) || (strlen(g_device->hostname) > HOSTLEN))
		{
			strcpy(g_device->hostname, "karadio32");
		}
		ESP_LOGI(TAG, "mDNS Hostname: %s", g_device->hostname);
		err = mdns_hostname_set(g_device->hostname);
		if (err)
			ESP_LOGE(TAG, "Hostname Init failed: %d", err);

		ESP_ERROR_CHECK(mdns_instance_name_set(g_device->hostname));
		ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
		ESP_ERROR_CHECK(mdns_service_add(NULL, "_telnet", "_tcp", 23, NULL, 0));

		// LCD Display infos
		lcd_welcome(localIp, "STARTED");
		vTaskDelay(pdMS_TO_TICKS(50));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());
#else
		ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());
#endif
		// start tasks of KaRadio32
		vTaskDelay(pdMS_TO_TICKS(10));
#if CONFIG_IDF_TARGET_ESP32S3
		xTaskCreatePinnedToCore(clientTask, "clientTask", 9216, NULL, PRIO_CLIENT, &pxCreatedTask, CPU_CLIENT); // #memchange 3452
#else
		xTaskCreatePinnedToCore(clientTask, "clientTask", 4096, NULL, PRIO_CLIENT, &pxCreatedTask, CPU_CLIENT); // #memchange 3452
#endif
		ESP_LOGI(TAG, "%s task: %x", "clientTask", (unsigned int)pxCreatedTask);
		vTaskDelay(pdMS_TO_TICKS(10));

		xTaskCreatePinnedToCore(serversTask, "serversTask", 3072, NULL, PRIO_SERVER, &pxCreatedTask, CPU_SERVER); // #memchange 2560
		ESP_LOGI(TAG, "%s task: %x", "serversTask", (unsigned int)pxCreatedTask);
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	else
	{
		_isRadio = false;
		if (!(CONFIG_BT_SPEAKER_MODE == 1))
		{
			SET_RADIO(g_device->options);
			goto Option_test; // try again with radio mode if BT not enabled
		}

		// xTaskCreatePinnedToCore(serversTask, "serversTask", 3072, NULL, PRIO_SERVER, &pxCreatedTask, CPU_SERVER); // #memchange 2560
		ESP_LOGI(TAG, "%s task: %x", "serversTask", (unsigned int)pxCreatedTask);
		vTaskDelay(pdMS_TO_TICKS(10));

		//	We're in bluetooth mode, block here
		ESP_LOGI(TAG, "Running in BT mode");
		vTaskDelay(pdMS_TO_TICKS(10));
		ESP_LOGI(TAG, "RAM left: %u", esp_get_free_heap_size());
		start_wifi();
		start_network();
	}
	// #memchange
	xTaskCreatePinnedToCore(task_addon, "task_addon", 2560, NULL, PRIO_ADDON, &pxCreatedTask, CPU_ADDON); // #memchange 2816
	ESP_LOGI(TAG, "%s task: %x", "task_addon", (unsigned int)pxCreatedTask);

	vTaskDelay(pdMS_TO_TICKS(1000)); // wait tasks init
	ESP_LOGI(TAG, " Init Done");

	setIvol(g_device->vol);
	kprintf("READY. Type help for a list of commands\n");
	// error log on telnet
	esp_log_set_vprintf((vprintf_like_t)lkprintf);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());
#else
	ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());
#endif
	if (_isRadio == true)
	{
		// autostart
		autoPlay();
	}
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		BTstart();
		// There is no offset Volume in BT mode to init volume
		char volstr[8];
		sprintf(volstr, "%d", g_device->vol);
		setVolume(volstr); // Set the volume to make initialization
	}
#endif // All done.
}

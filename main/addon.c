/******************************************************************************
 *
 * Copyright 2017 karawin (http://www.karawin.fr)
 *
 *******************************************************************************/

// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "nvs.h"
#include "ClickEncoder.h"
#include "ClickButtons.h"
#include "ClickJoystick.h"
#include "app_main.h"
#include "gpio.h"
#include "webclient.h"
#include "webserver.h"
#include "interface.h"
#include "audio_renderer.h"
#include "audio_player.h"
#include "addon.h"
#include "custom.h"
#include "esp_wifi.h"
#if defined(CONFIG_DISPLAY_TYPE_ALL)
#include "u8g2_esp32_hal.h"
#include "addonu8g2.h"
#include "ucg_esp32_hal.h"
#include "addonucg.h"
#elif defined(CONFIG_DISPLAY_TYPE_MONO)
#include "u8g2_esp32_hal.h"
#include "addonu8g2.h"
#elif defined(CONFIG_DISPLAY_TYPE_COLOR)
#include "ucg_esp32_hal.h"
#include "addonucg.h"
#endif
#include "ntp.h"

#include "eeprom.h"
#ifndef CONFIG_NO_XPT2046
#include "xpt2046.h"
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <driver/adc.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/pulse_cnt.h"
#else
#include "esp_adc_cal.h"
#include "driver/pcnt.h"
#endif
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_BT_SPEAKER_MODE
#include "bt_app_av.h"
#endif
#define TAG "addon"
#define PCNT_UNIT_ENCODER0 PCNT_UNIT_0 // PCNT unit for encoder 0
#define PCNT_UNIT_ENCODER1 PCNT_UNIT_1 // PCNT unit for encoder 1

static void evtClearScreen();
static void evtScreen(typelcmd value);
static void toggletime();
// second before time display in stop state
#define DTIDLE 60

bool _isRadio = true;
bool _reboot = false;
#define isColor (lcd_type & LCD_COLOR)
const char *stopped = "STOPPED";

// IR / transient string buffer used for numeric displays (PIN, numbers)
char irStr[16];
// Dedicated buffer to show OTA PIN without interfering with IR numeric input
char otaPinStr[16];
xQueueHandle event_ir = NULL;
xQueueHandle event_lcd = NULL;
static uint32_t ir_event_count = 0; // Counter for IR events received
static bool ir_loop_called = false; // Flag to track if irLoop has been called

// Store last 3 IR events for diagnostics
#define IR_EVENT_LOG_SIZE 3
static ir_event_log_t ir_event_log[IR_EVENT_LOG_SIZE] = {0};
static uint8_t ir_event_log_idx = 0;
#if defined(CONFIG_DISPLAY_TYPE_ALL) || defined(CONFIG_DISPLAY_TYPE_MONO)
u8g2_t u8g2; // a structure which will contain all the data for one display
#endif
#if defined(CONFIG_DISPLAY_TYPE_ALL) || defined(CONFIG_DISPLAY_TYPE_COLOR)
ucg_t ucg;
#endif
uint8_t lcd_type;
static xTaskHandle pxTaskLcd;
// list of screen
typedef enum typeScreen
{
	smain,
	svolume,
	sstation,
	snumber,
	sdisplay,
	sprogress,
	stime,
	snull
} typeScreen;
static typeScreen stateScreen = snull;
static typeScreen defaultStateScreen = smain;
// state of the transient screen
static uint8_t mTscreen = MTNEW; // 0 dont display, 1 display full, 2 display variable part

static bool playable = true;
static uint16_t volume;
static int16_t futurNum = 0; // the number of the wanted station

static unsigned timerScreen = 0;
static unsigned timerScroll = 0;
static unsigned timerLcdOut = 0;
static unsigned timer1s = 0;

static unsigned timein = 0;
static struct tm *dt;
time_t timestamp = 0;
static bool syncTime = false;
static bool itAskTime = true;	// update time with ntp if true
static bool itAskStime = false; // start the time display
static uint8_t itLcdOut = 0;
// static bool itAskSsecond = false; // start the time display
static bool state = false; // start stop on Ok key

// OTA progress state
static int ota_progress_percent = -1;

// set OTA progress: percent 0..100, -1 to clear
void setOtaProgress(int percent)
{
	if (percent < 0)
	{
		ota_progress_percent = -1;
		mTscreen = MTNEW;
		evtScreen(defaultStateScreen);
		return;
	}
	if (percent > 100)
		percent = 100;
	ota_progress_percent = percent;
	// request progress screen via event queue and wake LCD
	mTscreen = MTNEW;
	evtScreen(sprogress);
	wakeLcd();
}

static int16_t currentValue = 0;
static bool dvolume = true; // display volume screen

// custom ir code init from hardware nvs
typedef enum
{
	KEY_UP,
	KEY_LEFT,
	KEY_OK,
	KEY_RIGHT,
	KEY_DOWN,
	KEY_0,
	KEY_1,
	KEY_2,
	KEY_3,
	KEY_4,
	KEY_5,
	KEY_6,
	KEY_7,
	KEY_8,
	KEY_9,
	KEY_STAR,
	KEY_DIESE,
	KEY_INFO,
	KEY_MAX
} customKey_t;

static uint32_t customKey[KEY_MAX][2];
static bool isCustomKey = false;

static bool isEncoder0 = true;
static bool isEncoder1 = true;
static bool isButton0 = true;
static bool isButton1 = true;
static bool isJoystick0 = true;
static bool isJoystick1 = true;
static bool isEsplay = false;
static bool isAdcKeyboard = false;
static bool isAdcBatt = false;
static esp_adc_cal_characteristics_t characteristics;
static uint32_t adc_value = 0;
battery_state out_state;

// backlight value
static int blv = 100;
#if CONFIG_BT_SPEAKER_MODE
static int8_t bt_sdir = 0; // 0 = none, +1 = next, -1 = prev
#endif

void Screen(typeScreen st);
void drawScreen();

Encoder_t *encoder0 = NULL;
Encoder_t *encoder1 = NULL;
Button_t *button0 = NULL;
Button_t *button1 = NULL;

Joystick_t *joystick1 = NULL;
Joystick_t *joystick0 = NULL;

Button_t *expButton0 = NULL;
Button_t *expButton1 = NULL;
Button_t *expButton2 = NULL;

struct tm *getDt() { return dt; }

// Deep Sleep Power Save Input. https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html
gpio_num_t deepSleep_io; /** Enter Deep Sleep if pin is set to level defined in P_LEVELPINSLEEP. */
bool deepSleepLevel;	 /** Level to enter Deep Sleep / Wakeup if level is the opposite. */

void setBlv(int val) { blv = val; }
int getBlv() { return blv; }
void bt_pause_resume(void);

int getBatPercent()
{
	if (isAdcBatt)
	{
		if (out_state.percentage > 105)
			return -1; //
		if (out_state.percentage > 100)
			return 100; //
		return (out_state.percentage);
	}
	return -1;
}

void *getEncoder(int num)
{
	if (num == 0)
		return (void *)encoder0;
	if (num == 1)
		return (void *)encoder1;
	return NULL;
}

/* Mute toggle implementation
 * - first call stores current internal volume and sets volume to 0
 * - second call restores stored volume
 * This does not persist volume to NVS; it only updates runtime volume via setVolumei().
 */
void mute(void)
{
	static int16_t prev_vol = -1;
	int16_t cur = getIvol();

	if (cur != 0 && prev_vol == -1)
	{
		prev_vol = cur;
		setVolumei(0);
		kprintf("##MUTE: muted (prev %d)##\r\n", prev_vol);
		ESP_LOGI(TAG, "Muted (prev %d)", prev_vol);
	}
	else if (cur == 0 && prev_vol != -1)
	{
		setVolumei(prev_vol);
		kprintf("##MUTE: restored %d##\r\n", prev_vol);
		ESP_LOGI(TAG, "Mute restore %d", prev_vol);
		prev_vol = -1;
	}
	else
	{
		// If cur!=0 but prev_vol set, or cur==0 and prev_vol==-1, just toggle based on state
		if (cur == 0)
		{
			// nothing remembered, restore to default device value
			int devv = g_device->vol;
			setVolumei(devv);
			kprintf("##MUTE: restored device vol %d##\r\n", devv);
		}
		else
		{
			prev_vol = cur;
			setVolumei(0);
			kprintf("##MUTE: muted (prev %d)##\r\n", prev_vol);
		}
	}
}

static void ClearBuffer()
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		ucg_ClearScreen(&ucg);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		u8g2_ClearBuffer(&u8g2);
#endif
}
#ifndef CONFIG_DISPLAY_TYPE_NONE
static int16_t DrawString(int16_t x, int16_t y, const char *str)
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		return ucg_DrawString(&ucg, x, y, 0, str);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		return u8g2_DrawUTF8(&u8g2, x, y, str);
#endif
	return 0;
}

static void DrawColor(uint8_t color, uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		ucg_SetColor(&ucg, 0, r, g, b);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		u8g2_SetDrawColor(&u8g2, color);
#endif
}
static void DrawBox(ucg_int_t x, ucg_int_t y, ucg_int_t w, ucg_int_t h)
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		ucg_DrawBox(&ucg, x, y, w, h);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		u8g2_DrawBox(&u8g2, x, y, w, h);
#endif
}
#endif // CONFIG_DISPLAY_TYPE_NONE
uint16_t GetWidth()
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		return ucg_GetWidth(&ucg);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	return u8g2.width;
#else
	return 0;
#endif
}
uint16_t GetHeight()
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		return ucg_GetHeight(&ucg);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	return u8g2.height;
#else
	return 0;
#endif
}
void wakeLcd()
{
	if ((getLcdStop() != 0) && (!state))
		timerLcdOut = getLcdStop(); // rearm the tempo
	else
		timerLcdOut = getLcdOut(); // rearm the tempo
	if (itLcdOut == 2)
	{
		LedBacklightOn(blv);
		mTscreen = MTNEW;
		evtScreen(stateScreen);
		itLcdOut = 0; // 0 not activated, 1 sleep requested, 2 in sleep ;
	}
}

void sleepLcd()
{
	itLcdOut = 2; // in sleep
	if (!LedBacklightOff())
		evtClearScreen();
}

void lcd_init(uint8_t Type)
{
	lcd_type = Type;

	if (lcd_type == LCD_NONE)
		return;

#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (lcd_type & LCD_COLOR) // Color LCD
	{
		lcd_initUcg(&lcd_type);
	}
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!(lcd_type & LCD_COLOR))
	{
		lcd_initU8g2(&lcd_type);
	}
#endif
	vTaskDelay(pdMS_TO_TICKS(10));
	// init the gpio for backlight
	LedBacklightInit();
}

void in_welcome(const char *ip, const char *state, int y, char *Version)
{
	if (lcd_type == LCD_NONE)
		return;
#ifndef CONFIG_DISPLAY_TYPE_NONE
	DrawString(2, 2 * y, Version);
	DrawColor(0, 0, 0, 0);
	DrawBox(2, 4 * y, GetWidth() - 2, y);
	DrawColor(1, 255, 255, 255);
	DrawString(2, 4 * y, state);
	DrawString(DrawString(2, 5 * y, "IP:") + 18, 5 * y, ip);
#endif
}

void lcd_welcome(const char *ip, const char *state)
{
	char Version[20];
	sprintf(Version, "Version %s R%s\n", RELEASE, REVISION);
	if (lcd_type == LCD_NONE)
		return;
	if ((strlen(ip) == 0) && (strlen(state) == 0))
		ClearBuffer();
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
	{
		setfont(2);
		int y = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg) + 3; // interline
		DrawString(GetWidth() / 4, 2, "KaRadio32");
		setfont(1);
		in_welcome(ip, state, y, Version);
	}
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
	{
		u8g2_FirstPage(&u8g2);
		do
		{
			setfont8(2);
			int y = (u8g2_GetAscent(&u8g2) - u8g2_GetDescent(&u8g2));
			DrawString(GetWidth() / 4, 2, "KaRadio32");
			setfont8(1);
			in_welcome(ip, state, y, Version);
		} while (u8g2_NextPage(&u8g2));
	}
#endif
}

// ----------------------------------------------------------------------------
// call this every 1 millisecond via timer ISR
//
void (*serviceAddon)() = NULL;

IRAM_ATTR void ServiceAddon(void)
{
	timer1s++;
	timerScroll++;
	if (timer1s >= 1000)
	{
		// Time compute
		timestamp++; // time update
		if (timerLcdOut > 0)
			timerLcdOut--; //
		timein++;
		if ((timestamp % (120 * DTIDLE)) == 0)
		{
			itAskTime = true;
		} // synchronise with ntp every x*DTIDLE

		if (((timein % DTIDLE) == 0) && (!state))
		{
			{
				itAskStime = true;
				timein = 0;
			} // start the time display when paused
		}
		if (timerLcdOut == 1)
			itLcdOut = 1; // ask to go to sleep
		if (!syncTime)
			itAskTime = true; // first synchro if not done

		timer1s = 0;
		// Other slow timers
		timerScreen++;
	}
}

////////////////////////////////////////
// futurNum
void setFuturNum(int16_t new)
{
	futurNum = new;
}
int16_t getFuturNum()
{
	return futurNum;
}

////////////////////////////////////////
// scroll each line
void scroll()
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		scrollUcg();
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		scrollU8g2();
#endif
}

////////////////////////////
// Change the current screen
////////////////////////////
void Screen(typeScreen st)
{
	// printf("Screen: st: %d, stateScreen: %d, mTscreen: %d, default: %d\n",st,stateScreen,mTscreen,defaultStateScreen);
	if (stateScreen != st)
	{
		mTscreen = MTNEW;
		wakeLcd();
	}
	else
	{
		if (mTscreen == MTNODISPLAY)
			mTscreen = MTREFRESH;
	}

	//  printf("Screenout: st: %d, stateScreen: %d, mTscreen: %d, default: %d, timerScreen: %d \n",st,stateScreen,mTscreen,defaultStateScreen,timerScreen);

	stateScreen = st;
	timein = 0;
	timerScreen = 0;
	drawScreen();
	// printf("Screendis: st: %d, stateScreen: %d, mTscreen: %d, default: %d\n",st,stateScreen,mTscreen,defaultStateScreen);
	//   vTaskDelay(1);
}

////////////////////////////////////////
// draw all lines
void drawFrame()
{
	dt = localtime(&timestamp);
	if (lcd_type == LCD_NONE)
		return;
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		drawFrameUcg(mTscreen);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		drawFrameU8g2(mTscreen);
#endif
}

//////////////////////////
void drawTTitle(char *ttitle)
{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		drawTTitleUcg(ttitle);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		drawTTitleU8g2(ttitle);
#endif
}

////////////////////
// draw the number entered from IR
void drawNumber()
{
	if (strlen(irStr) > 0)
	{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
		if (isColor)
			drawNumberUcg(mTscreen, irStr);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
		if (!isColor)
			drawNumberU8g2(mTscreen, irStr);
#endif
	}
}

////////////////////
// draw the station screen
void drawStation()
{
	char sNum[7];
	char *ddot;
	char *ptl;
	struct shoutcast_info *si = NULL;

	// ClearBuffer();
	if (_isRadio)
	{
		do
		{
			si = getStation(futurNum);
			sprintf(sNum, "%d", futurNum);
			ddot = si->name;
			ptl = ddot;
			while (*ptl == 0x20)
			{
				ddot++;
				ptl++;
			}
			if (strlen(ddot) == 0) // don't start an undefined station
			{
				playable = false;
				free(si);
				if (currentValue < 0)
				{
					futurNum--;
					if (futurNum < 0)
						futurNum = 254;
				}
				else
				{
					futurNum++;
					if (futurNum > 254)
						futurNum = 0;
				}
			}
			else
				playable = true;
		} while (playable == false);
	}
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		// Show static text for transient display after encoder action
		extern int8_t bt_sdir;
		if (bt_sdir > 0)
			ddot = "Next Track";
		else if (bt_sdir < 0)
			ddot = "Previous Track";
		else
			ddot = g_device->BTname;
		sprintf(sNum, "%d", bt_sdir);
	}
#endif
	// drawTTitle(ststr);
	// printf ("drawStation: %s\n",sNum  );
	if (lcd_type != LCD_NONE)
	{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
		if (isColor)
			drawStationUcg(mTscreen, sNum, ddot);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
		if (!isColor)
			drawStationU8g2(mTscreen, sNum, ddot);
#endif
	}
	if (_isRadio && si)
		free(si); // Only free if si is not NULL
}

////////////////////
// draw the volume screen
void drawVolume()
{
	if (lcd_type == LCD_NONE)
		return;
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		drawVolumeUcg(mTscreen);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		drawVolumeU8g2(mTscreen);
#endif
}

void drawTime()
{
	dt = localtime(&timestamp);
	if (lcd_type == LCD_NONE)
		return;
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
	if (isColor)
		drawTimeUcg(mTscreen, timein);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
	if (!isColor)
		drawTimeU8g2(mTscreen, timein);
#endif
}

////////////////////
// Display a screen on the lcd
void drawScreen()
{
	if (lcd_type == LCD_NONE)
		return;
	//  ESP_LOGD(TAG,"stateScreen: %d,defaultStateScreen: %d, mTscreen: %d, itLcdOut: %d",stateScreen,defaultStateScreen,mTscreen,itLcdOut);
	if ((mTscreen != MTNODISPLAY) && (!itLcdOut))
	{
		switch (stateScreen)
		{
		case smain: //
			drawFrame();
			break;
		case svolume:
			drawVolume();
			break;
		case sstation:
			drawStation();
			break;
		case stime:
			drawTime();
			break;
		case snumber:
			drawNumber();
			break;
		case sdisplay:
			// draw OTA PIN
			if (lcd_type != LCD_NONE)
			{
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
				if (isColor)
					drawNumberUcg(mTscreen, otaPinStr);
				else
#endif
					drawNumberU8g2(mTscreen, otaPinStr);
			}
			break;
		case sprogress:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
			if (lcd_type != LCD_NONE)
			{
				if (isColor)
					drawProgressUcg(ota_progress_percent < 0 ? 0 : ota_progress_percent);
				else
#endif
					drawProgressU8g2(ota_progress_percent < 0 ? 0 : ota_progress_percent);
				break;
			}
		default:
			Screen(defaultStateScreen);
			//	  drawFrame();
		}
		//	if (mTscreen == MTREFRESH)
		mTscreen = MTNODISPLAY;
	}
}

// Show a short numeric string (PIN) on the device LCD for a few seconds
void showPin(const char *pin, int seconds)
{
	if (lcd_type == LCD_NONE)
		return;
	if (!pin)
		return;
	// Build META string: "META#: - upload pin - <pin>"
	event_lcd_t evt;
	size_t need = strlen("META#: - upload pin - ") + strlen(pin) + 1;
	char *meta = kmalloc(need);
	if (meta != NULL)
	{
		snprintf(meta, need, "META#: - upload pin - %s", pin);
		evt.lcmd = lmeta;
		evt.lline = meta;
		if (event_lcd != NULL)
		{
			if (xQueueSend(event_lcd, &evt, 0) != pdTRUE)
			{
				free(meta);
				meta = NULL;
			}
		}
		else
		{
			free(meta);
			meta = NULL;
		}
	}

	// Request station screen transiently via event queue (will revert on timer)
	// If time screen is active, toggle time so station screen becomes visible
	if (stateScreen == stime)
		toggletime();
	timerScreen = seconds;
	evtScreen(sstation);
	// wake up LCD to show new lines
	wakeLcd();
}

void stopStation()
{
	clientDisconnect("addon stop");
}
void startStation()
{
	playStationInt(futurNum);
	;
}
void startStop()
{
	if (_isRadio)
	{
		ESP_LOGD(TAG, "START/STOP State: %d", state);
		state ? stopStation() : startStation();
		return;
	}
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		bt_pause_resume();
		return;
	}
#endif
}
void stationOk()
{
	ESP_LOGD(TAG, "STATION OK");
	if (strlen(irStr) > 0)
	{
		futurNum = atoi(irStr);
		playStationInt(futurNum);
	}
	else
	{
		startStop();
	}
	irStr[0] = 0;
}
void changeStation(int16_t value)
{
	currentValue = value;
	ESP_LOGD(TAG, "changeStation val: %d, futurnum: %d", value, futurNum);
	if (_isRadio)
	{
#if USE_PCNT_ENCODER
		futurNum += value;
		if (futurNum > 254)
			futurNum -= 254;
		else if (futurNum < 0)
			futurNum += 254;
#else
		if (value > 0)
			futurNum++;
		else if (value < 0)
			futurNum--;

		if (futurNum > 254)
			futurNum = 0;
		else if (futurNum < 0)
			futurNum = 254;
#endif
	}
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		// Remember last direction for BT mode
		extern int8_t bt_sdir;
		if (value > 0)
			bt_sdir = 1;
		else if (value < 0)
			bt_sdir = -1;
		else
			bt_sdir = 0;
	}
#endif
	ESP_LOGD(TAG, "futurnum: %d", futurNum);
	// else if (value != 0) mTscreen = MTREFRESH;
}
// IR
// a number of station in progress...
void nbStation(char nb)
{
	if (strlen(irStr) >= 3)
		irStr[0] = 0;
	uint8_t id = strlen(irStr);
	irStr[id] = nb;
	irStr[id + 1] = 0;
	evtScreen(snumber);
}

//
static void evtClearScreen()
{
	//	isColor?ucg_ClearScreen(&ucg):u8g2_ClearDisplay(&u8g2);
	event_lcd_t evt;
	evt.lcmd = eclrs;
	evt.lline = NULL;
	if (lcd_type != LCD_NONE)
		xQueueSend(event_lcd, &evt, 0);
}

static void evtScreen(typelcmd value)
{
	event_lcd_t evt;
	evt.lcmd = escreen;
	ESP_LOGI("ADDON_evtScreen", "evtScreen value: %d", value);
	if (_isRadio)
		evt.lline = (char *)((uint32_t)value);
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		// evt.lline = (char *)((uint32_t)value);
	}
#endif
	if (lcd_type != LCD_NONE)
		xQueueSend(event_lcd, &evt, 0);
}

static void evtStation(int16_t value)
{ // value +1 or -1
	event_lcd_t evt;
	evt.lcmd = estation;
	ESP_LOGI("ADDON_evtStation", "evtStation value: %d", value);
	if (_isRadio)
	{
		evt.lline = (char *)((uint32_t)value);
		if (lcd_type != LCD_NONE)
			xQueueSend(event_lcd, &evt, 0);
	}
#if CONFIG_BT_SPEAKER_MODE
	else
	{
		evt.lline = (char *)((uint32_t)value);
		if (lcd_type != LCD_NONE)
			xQueueSend(event_lcd, &evt, 0);
		// BT mode: next/previous track
		if (value > 0)
			bt_next_track();
		else if (value < 0)
			bt_prev_track();
	}
#endif
}

// toggle main / time
static void toggletime()
{
	event_lcd_t evt;
	evt.lcmd = etoggle;
	evt.lline = NULL;
	if (lcd_type != LCD_NONE)
		xQueueSend(event_lcd, &evt, 0);
}

//----------------------------
// Adc read: keyboard buttons
//----------------------------

static adc1_channel_t channel = GPIO_NONE;
static adc1_channel_t chanBat = GPIO_NONE;
static bool inside = false;
#define DEFAULT_VREF 1100
void adcInit()
{
	gpio_get_adc(&channel, &chanBat);
	ESP_LOGD(TAG, "ADC Channel: %i, %i", channel, chanBat);
	if ((channel & chanBat) != GPIO_NONE)
	{
		adc1_config_width(ADC_WIDTH_BIT_12);
		if (channel != GPIO_NONE)
		{
			isAdcKeyboard = true;
			adc1_config_channel_atten(channel, ADC_ATTEN_DB_0);
		}
		if (chanBat != GPIO_NONE)
		{
			isAdcBatt = true;
			adc1_config_channel_atten(chanBat, ADC_ATTEN_DB_12);
			esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, DEFAULT_VREF, &characteristics);
			if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP)
			{
				ESP_LOGI(TAG, "ADC: Characterized using Two Point Value");
			}
			else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF)
			{
				ESP_LOGI(TAG, "ADC: Characterized using eFuse Vref");
			}
			else
			{
				ESP_LOGI(TAG, "ADC: Characterized using Default Vref");
			}
		}
	}
}

void adcBatLoop()
{
	if (isAdcBatt)
	{
		const int sampleCount = 8;

		uint32_t adcSample = 0;
		for (int i = 0; i < sampleCount; ++i)
		{
			// adcSample += adc1_to_voltage(ADC1_CHANNEL_0, &characteristics) * 0.001f;
			adcSample += esp_adc_cal_raw_to_voltage(adc1_get_raw(chanBat), &characteristics);
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		adcSample /= sampleCount;

		if (adc_value == 0)
		{
			adc_value = adcSample;
		}
		else
		{
			adc_value += adcSample;
			adc_value /= 2;
		}

		const uint32_t Vs = adc_value * 2;

		const uint32_t FullVoltage = 4200;
		const uint32_t EmptyVoltage = 3050;

		out_state.millivolts = (int)(Vs * 1000);
		out_state.percentage = (int)((Vs - EmptyVoltage) / (FullVoltage - EmptyVoltage) * 100);
		ESP_LOGD(TAG, "ADC Batt: %d%%, millivolt: %d, Sample: %d, Value: %d ", out_state.percentage, out_state.millivolts, adcSample, adc_value);

		if (out_state.percentage > 100)
			out_state.percentage = 100;
		if (out_state.percentage < 0)
			out_state.percentage = 0;
	}
}
void adcLoop()
{
	if (isAdcKeyboard)
	{
		uint32_t voltage, voltage0, voltage1;
		bool wasVol = false;
		if (channel == GPIO_NONE)
			return; // no gpio specified
		voltage0 = (adc1_get_raw(channel) + adc1_get_raw(channel) + adc1_get_raw(channel) + adc1_get_raw(channel)) / 4;
		vTaskDelay(pdMS_TO_TICKS(10));
		voltage1 = (adc1_get_raw(channel) + adc1_get_raw(channel) + adc1_get_raw(channel) + adc1_get_raw(channel)) / 4;
		//		printf ("Volt0: %d, Volt1: %d\n",voltage0,voltage1);
		voltage = (voltage0 + voltage1) * 105 / (819);
		if (voltage < 40)
			return; // no panel
					//		printf("Voltage: %d\n",voltage);

		if (inside && (voltage0 > 3700))
		{
			inside = false;
			wasVol = false;
			return;
		}
		if (voltage0 > 3700)
		{
			wasVol = false;
		}
		if ((voltage0 > 3700) || (voltage1 > 3700))
			return; // must be two valid voltage

		if (voltage < 985)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			ESP_LOGD(TAG, "Voltage: %lu", voltage);
#else
			ESP_LOGD(TAG, "Voltage: %i", voltage);
#endif
		//			printf("VOLTAGE: %d\n",voltage);
		if ((voltage > 400) && (voltage < 590)) // volume +
		{
			setRelVolume(+1);
			wasVol = true;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			ESP_LOGD(TAG, "Volume+ : %lu", voltage);
#else
			ESP_LOGD(TAG, "Volume+ : %i", voltage);
#endif
		}
		else if ((voltage > 730) && (voltage < 830)) // volume -
		{
			setRelVolume(-1);
			wasVol = true;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			ESP_LOGD(TAG, "Volume- : %lu", voltage);
#else
			ESP_LOGD(TAG, "Volume- : %i", voltage);
#endif
		}
		else if ((voltage > 900) && (voltage < 985)) // station+
		{
			if (!wasVol)
			{
				evtStation(1);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				ESP_LOGD(TAG, "station+: %lu", voltage);
#else
				ESP_LOGD(TAG, "station+: %i", voltage);
#endif
			}
		}
		else if ((voltage > 620) && (voltage < 710)) // station-
		{
			if (!wasVol)
			{
				evtStation(-1);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				ESP_LOGD(TAG, "station-: %lu", voltage);
#else
				ESP_LOGD(TAG, "station-: %i", voltage);
#endif
			}
		}
		if (!inside)
		{
			if ((voltage > 100) && (voltage < 220)) // toggle time/info  old stop
			{
				inside = true;
				toggletime();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				ESP_LOGD(TAG, "toggle time: %lu", voltage);
#else
				ESP_LOGD(TAG, "toggle time: %i", voltage);
#endif
			}
			else if ((voltage > 278) && (voltage < 380)) // start stop toggle   old start
			{
				inside = true;
				startStop();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
				ESP_LOGD(TAG, "start stop: %lu", voltage);
#else
				ESP_LOGD(TAG, "start stop: %i", voltage);
#endif
			}
		}
	}
}

//-----------------------
// Compute the Joystick
//----------------------
void joystickCompute(Joystick_t *enc, bool role)
{
	int16_t newValue = 0;
	{
		Button state1 = getJoystick(enc, 0);
		Button state2 = getJoystick(enc, 1);
		//		ESP_LOGD(TAG,"Button1: %i, Button2: %i",state1,state2);
		newValue = ((state1 != Open) ? 5 : 0) + ((state2 != Open) ? -5 : 0); // sstation take + or - in any value
		typeScreen estate;
		if (role)
			estate = sstation;
		else
			estate = svolume;
		if ((stateScreen != estate) && (newValue != 0))
		{
			if (role)
				setRelVolume(newValue);
			else
				evtStation(-newValue);
			ESP_LOGD(TAG, "Button1: %i, Button2: %i, value: %i", state1, state2, newValue);
		}
		if ((stateScreen == estate) && (newValue != 0))
		{
			if (role)
				evtStation(-newValue);
			else
				setRelVolume(newValue);
			ESP_LOGD(TAG, "Button1: %i, Button2: %i, value: %i", state1, state2, newValue);
		}
	}
}

//-----------------------
// Compute the Buttons
//----------------------
void buttonCompute(Button_t *enc, uint8_t role)
{
	int16_t newValue = 0;
	Button state0;
	if (role != ECTRL)
	{
		state0 = getButtons(enc, 0);
		if (state0 != Open)
		{
			ESP_LOGD(TAG, "Button0 state: %d", state0);
			if (state0 == Clicked)
				startStop();
			// double click = toggle time
			if (state0 == DoubleClicked)
			{
				toggletime();
			}
			if (state0 == Held)
			{
				if (stateScreen != ((role) ? sstation : svolume))
				{
					(role) ? evtStation(newValue) : setRelVolume(newValue);
				}
			}
		}
		else
		{
			Button state1 = getButtons(enc, 1);
			Button state2 = getButtons(enc, 2);
			newValue = ((state1 != Open) ? 5 : 0) + ((state2 != Open) ? -5 : 0);
			typeScreen estate = snull;
			if ((isButton0 ^ isButton1) && (!isEsplay)) // one button and not Esplay
			{
				if (role)
					estate = sstation;
				else
					estate = svolume;
			}
			if ((state1 != Open) || (state2 != Open))
				ESP_LOGD(TAG, "Button1 state: %d, Button2 state: %d, newValue: %d, estate: %d, stateScreen: %d",
						 state1, state2, newValue, estate, stateScreen);
			if ((stateScreen != estate) && (newValue != 0))
			{
				if (role)
					setRelVolume(newValue);
				else
					evtStation(newValue);
			}
			if ((stateScreen == estate) && (newValue != 0))
			{
				if (role)
					evtStation(newValue);
				else
					setRelVolume(newValue);
			}
		}
	}
	else // Third control for Esplay
	{
		Button state1;
		state0 = getButtons(enc, 0);
		if (state0 == Clicked)
			toggletime();

		state0 = getButtons(enc, 1);
		state1 = getButtons(enc, 2);
		if (state0 != Open)
			blv -= 2;
		else if (state1 != Open)
			blv += 2;
		else
			return;
		if (blv > 100)
			blv = 100;
		if (blv < 2)
			blv = 2;
		wakeLcd();
		backlight_percentage_set(blv);
		option_set_lcd_blv(blv);
	}
}

void radioToggle(const bool restart)
{
	TOGGLE_RADIO(g_device->options);
	saveDeviceSettings(g_device);
	if (restart)
	{
		_reboot = true;
		onRatio = 50;
		cycleDuration = 100; // fast blinking
		if (_isRadio)
		{
			clientSilentDisconnect();
			renderer_stop();
			esp_err_t err = esp_wifi_stop(); // stop wifi before restart
			ESP_LOGI(TAG, "esp_wifi_stop returned: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(200));
			err = esp_wifi_deinit(); // restart we do not need wifi anymore
			ESP_LOGI(TAG, "esp_wifi_deinit returned: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
#if CONFIG_BT_SPEAKER_MODE
		else
		{
			extern component_status_t player_status;

			if (player_status == RUNNING)
				bt_pause_resume();
			renderer_stop();
			// i2s_driver_uninstall(I2S_NUM_0); // Uninstall I2S driver (if using I2S_NUM_0)
			vTaskDelay(pdMS_TO_TICKS(500));

		}
#endif
		esp_restart();
	}

}

//-----------------------
// Compute the encoder
//----------------------
void encoderCompute(Encoder_t *enc, bool role)
{
	Button newButton = getButton(enc);

	// Handle encoder button actions
	if (newButton != Open)
	{
		if (newButton == Clicked)
		{
			startStop();
		}
		if (newButton == DoubleClicked)
		{
			toggletime();
		}
		if ((newButton == Held_Long))
		{
			cycleDuration = 100; // fast blinking
			onRatio = 50;
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
			logAllTaskHighWaterMarks();
#endif
			vTaskDelay(pdMS_TO_TICKS(100)); // lets give chance ledTask to set Status LED

			// Stop playback and restart
			if (_isRadio)
			{
				clientSilentDisconnect();
			}
#if CONFIG_BT_SPEAKER_MODE
			else
			{

				extern esp_a2d_audio_state_t m_audio_state;
				if (m_audio_state == ESP_A2D_AUDIO_STATE_STARTED)
				{
					esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
					vTaskDelay(pdMS_TO_TICKS(10));
					esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
					vTaskDelay(pdMS_TO_TICKS(5));
				}
				extern player_t *player;
				if (player)
				{
					audio_player_stop();
					if (player->media_stream)
					{
						free(player->media_stream);
						player->media_stream = NULL;
					}
					free(player);
					player = NULL;
				}
				if (init_i2s())
				{
					i2s_zero_dma_buffer(I2S_NUM_0);	 // Clear I2S DMA buffer (if using I2S_NUM_0)
					i2s_driver_uninstall(I2S_NUM_0); // Uninstall I2S driver (if using I2S_NUM_0)
				}
				if (PIN_I2S_DATA >= 0 && PIN_I2S_DATA != GPIO_NONE)
				{
					gpio_set_level(PIN_I2S_DATA, 0); // Set data pin low
					vTaskDelay(pdMS_TO_TICKS(50));	 // Wait for output to settle
				}
			}
#endif
			// save the volume if needed on stop state
			if (g_device->vol != getIvol())
			{
				g_device->vol = getIvol();
				saveDeviceSettingsVolume(g_device);
			}

			if ((CONFIG_BT_SPEAKER_MODE == 1) && (enc == encoder1))
			{
				radioToggle(true);
			}

			// Encoder 0 long press: trigger deep sleep (no restart needed)
			if (enc == encoder0)
			{
				kprintf("Encoder 0 long press: triggering deep sleep\r\n");
				audio_player_stop(); // Stop the audio player before sleeping
				vTaskDelay(pdMS_TO_TICKS(50));
				deepSleepEnableWakeup(); // Configure wakeup sources
				deepSleepStart();		 // Enter deep sleep - this function does not return
				return;					 // Safety return (deepSleepStart should not return)
			}

			// Encoder 1 or other: normal restart (for BT/WebRadio mode switching)
			_reboot = true;					 // Set reboot flag to avoid reconnecting to WiFi in app_main
			esp_err_t err = esp_wifi_stop(); // stop wifi before restart
			ESP_LOGI(TAG, "esp_wifi_stop returned: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(200));
			err = esp_wifi_deinit(); // restart we do not need wifi anymore
			ESP_LOGI(TAG, "esp_wifi_deinit returned: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(1000));
			// esp_deep_sleep_start(); // like restart, but need button to start again.
			esp_restart(); // Restart the ESP32
		}
	}

	typeScreen estate = snull;
#if USE_PCNT_ENCODER
	// Process encoder movement every ENC_CYCLES
	if (enc->ReadCycleCount++ == ENC_CYCLES)
	{
		int16_t newValue = getValue(enc);
		if (newValue != 0) // no need to process if encoder was not moved
		{
			if (enc->button == Held)
				enc->keyDownTicks = 0;					  // reset long press detection counter (single encoder secondary function )
			if ((isEncoder0 ^ isEncoder1) && (!isEsplay)) // one encoder and not esplay
			{
				if (role)
					estate = sstation;
				else
					estate = svolume;
			}
			// held and rotate logic - secondary function
			if (((newButton == Held) || (getPinState(enc) == getpinsActive(enc))))
			{
				// If two encoders are present and this is the volume encoder (encoder0),
				// and a backlight GPIO is configured, use held+rotate to control LCD backlight
				// instead of station/volume swap. Otherwise fall back to original behavior.
				if ((enc == encoder0) && isEncoder0 && isEncoder1)
				{
					gpio_num_t bl_gpio = GPIO_NONE;
					gpio_get_lcd_backlightl(&bl_gpio);
					if (bl_gpio != GPIO_NONE)
					{
						int cur = getBlv();
						int step = newValue * 2; // 2% per encoder tick
						int nb = cur + step;
						if (nb > 100)
							nb = 100;
						if (nb < 2)
							nb = 2;
						backlight_percentage_set(nb);
						option_set_lcd_blv(nb);
						setBlv(nb);
					}
					else
					{
						if (stateScreen != (role ? sstation : svolume))
							role ? evtStation(newValue) : setRelVolume(newValue);
					}
				}
				else
				{
					if (stateScreen != (role ? sstation : svolume))
						role ? evtStation(newValue) : setRelVolume(newValue);
				}
			}
			else // if not Held - main ecnoder function
			{
				if ((stateScreen != estate) && (newValue != 0))
				{
					// ESP_LOGI(TAG, "Debug NOT Held Encoder: %d, newValue: %d, stateScreen: %d, estate: %d", role, newValue, stateScreen, estate);

					if (role)
						setRelVolume(newValue);
					else
						evtStation(newValue);
				}
				else if ((stateScreen == estate) && (newValue != 0))
				{
					// ESP_LOGI(TAG, "Debug NOT Held Encoder: %d, newValue: %d, stateScreen: %d, estate: %d", role, newValue, stateScreen, estate);
					if (role)
						evtStation(newValue);
					else
						setRelVolume(newValue);
				}
			}
		} // encoder was moved
		enc->ReadCycleCount = 0; // Encoder value processed, we can clear it.
	} // end of PCNT read cycle
#else
	int16_t newValue = -getValue(enc);
	if (newValue != 0)
		ESP_LOGD(TAG, "encoder value: %d, stateScreen: %d", newValue, stateScreen);
	if ((newButton == Held) && (getPinState(enc) == getpinsActive(enc)))
	{
		// Two-encoder case: held+rotate on volume encoder adjusts backlight if backlight GPIO configured
		if ((enc == encoder0) && isEncoder0 && isEncoder1)
		{
			gpio_num_t bl_gpio = GPIO_NONE;
			gpio_get_lcd_backlightl(&bl_gpio);
			if (bl_gpio != GPIO_NONE)
			{
				int cur = getBlv();
				int step = newValue * 2;
				int nb = cur + step;
				if (nb > 100)
					nb = 100;
				if (nb < 2)
					nb = 2;
				backlight_percentage_set(nb);
				option_set_lcd_blv(nb);
				setBlv(nb);
			}
			else
			{
				if (stateScreen != (role ? sstation : svolume))
					role ? evtStation(newValue) : setRelVolume(newValue);
			}
		}
		else
		{
			if (stateScreen != (role ? sstation : svolume))
				role ? evtStation(newValue) : setRelVolume(newValue);
		}
	}
	else
	//  no event on button switch
	{
		typeScreen estate = snull;
		if ((isEncoder0 ^ isEncoder1) && (!isEsplay)) // one button and not esplay
		{
			if (role)
				estate = sstation;
			else
				estate = svolume;
		}

		if ((stateScreen != estate) && (newValue != 0))
		{
			if (role)
				setRelVolume(newValue);
			else
				evtStation(newValue);
		}
		if ((stateScreen == estate) && (newValue != 0))
		{
			if (role)
				evtStation(newValue);
			else
				setRelVolume(newValue);
		}
	}

#endif
}
void periphLoop()
{
	// encoder0 = volume control or station when pushed
	// encoder1 = station control or volume when pushed
	// button0 = volume control or station when pushed
	// button1 = station control or volume when pushed
	if (isButton0)
		buttonCompute(button0, VCTRL);
	if (isButton1)
		buttonCompute(button1, SCTRL);
	if (!isEsplay)
	{
		if (isEncoder0)
			encoderCompute(encoder0, VCTRL);
		if (isEncoder1)
			encoderCompute(encoder1, SCTRL);
		if (isJoystick0)
			joystickCompute(joystick0, VCTRL);
		if (isJoystick1)
			joystickCompute(joystick1, SCTRL);
	}
	else
	{
		//		// ESP_LOGI(TAG,"rexp: 0x%x",rexp);
		buttonCompute(expButton0, VCTRL);
		buttonCompute(expButton1, SCTRL);
		buttonCompute(expButton2, ECTRL);
	}
}

// compute custom IR
bool irCustom(uint32_t evtir, bool repeat)
{
	int i;
	for (i = KEY_UP; i < KEY_MAX; i++)
	{
		if ((evtir == customKey[i][0]) || (evtir == customKey[i][1]))
			break;
	}
	if (i < KEY_MAX)
	{
		switch (i)
		{
		case KEY_UP:
			evtStation(+1);
			break;
		case KEY_LEFT:
			setRelVolume(-5);
			break;
		case KEY_OK:
			if (!repeat)
				stationOk();
			break;
		case KEY_RIGHT:
			setRelVolume(+5);
			break;
		case KEY_DOWN:
			evtStation(-1);
			break;
		case KEY_0:
			if (!repeat)
				nbStation('0');
			break;
		case KEY_1:
			if (!repeat)
				nbStation('1');
			break;
		case KEY_2:
			if (!repeat)
				nbStation('2');
			break;
		case KEY_3:
			if (!repeat)
				nbStation('3');
			break;
		case KEY_4:
			if (!repeat)
				nbStation('4');
			break;
		case KEY_5:
			if (!repeat)
				nbStation('5');
			break;
		case KEY_6:
			if (!repeat)
				nbStation('6');
			break;
		case KEY_7:
			if (!repeat)
				nbStation('7');
			break;
		case KEY_8:
			if (!repeat)
				nbStation('8');
			break;
		case KEY_9:
			if (!repeat)
				nbStation('9');
			break;
		case KEY_STAR:
			if (!repeat)
			{
#if CONFIG_BT_SPEAKER_MODE
				if (!_isRadio)
					radioToggle(true);
				else
#endif
					playStationInt(futurNum);
			}
			break;
		case KEY_DIESE:
			if (!repeat)
				stopStation();
			break;
		case KEY_INFO:
			if (!repeat)
				toggletime();
			break;
		default:;
		}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGV(TAG, "irCustom success, evtir %lx, i: %d", evtir, i);
#else
		ESP_LOGV(TAG, "irCustom success, evtir %x, i: %d", evtir, i);
#endif
		return true;
	}
	return false;
}
static bool isIR()
{
	gpio_num_t ir;
	gpio_get_ir_signal(&ir);
	if (ir != GPIO_NONE)
	{
		ESP_LOGI(TAG, "IR GPIO: %d", ir);
		return true;
	}
	else
	{
		ESP_LOGI(TAG, "No IR GPIO defined");
		return false;
	}
}
//-----------------------
// Compute the ir code
//----------------------

void irLoop()
{
	// IR
	event_ir_t evt;
	ir_loop_called = true; // Mark that irLoop has been called
	ESP_LOGI(TAG, "Checking IR events...%d", isCustomKey);
	while (xQueueReceive(event_ir, &evt, 0))
	{
		ir_event_count++; // Increment event counter
		uint32_t evtir = ((evt.addr) << 8) | (evt.cmd & 0xFF);

		// Store event in log buffer
		ir_event_log[ir_event_log_idx].code = evtir;
		ir_event_log[ir_event_log_idx].addr = evt.addr;
		ir_event_log[ir_event_log_idx].cmd = evt.cmd;
		ir_event_log[ir_event_log_idx].repeat = evt.repeat_flag;
		ir_event_log[ir_event_log_idx].timestamp = xTaskGetTickCount();
		ir_event_log_idx = (ir_event_log_idx + 1) % IR_EVENT_LOG_SIZE;

		wakeLcd();
		if (getLogLevel() >= ESP_LOG_INFO)
		{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			kprintf("IR event: Channel: %x, ADDR: 0x%04lx, CMD: 0x%04lx Code: 0x%lX, REPEAT: %d\r\n",
					evt.channel, (unsigned long)evt.addr, (unsigned long)evt.cmd, (unsigned long)evtir, evt.repeat_flag);
#else
			kprintf("IR event: Channel: %x, ADDR: 0x%04x, CMD: 0x%04x Code: 0x%X, REPEAT: %d\r\n",
					evt.channel, evt.addr, evt.cmd, evtir, evt.repeat_flag);
#endif
		}
		if (isCustomKey)
		{
			if (irCustom(evtir, evt.repeat_flag))
				continue;
		}
		else
		{ // no predefined keys
			switch (evtir)
			{
			case 0xDF2047:
			case 0xDF2002:
			case 0xFF0046:
			case 0xBF4019:
			case 0xBF401B:
			case 0xF70812: /*(" UP");*/
				evtStation(+1);
				break;
			case 0xDF2049:
			case 0xDF2041:
			case 0xFF0044:
			case 0xF70842:
			case 0xBF4083:
			case 0xF70815: /*(" LEFT");*/
				setRelVolume(-5);
				break;
			case 0xDF204A:
			case 0xFF0040:
			case 0xBF4089:
			case 0xF7081E: /*(" OK");*/
				if (!evt.repeat_flag)
					stationOk();
				break;
			case 0xDF204B:
			case 0xDF2003:
			case 0xFF0043:
			case 0xF70841:
			case 0xBF4082:
			case 0xF70814: /*(" RIGHT");*/
				setRelVolume(+5);
				break;
			case 0xDF204D:
			case 0xDF2009:
			case 0xFF0015:
			case 0xBF401D:
			case 0xBF401F:
			case 0xF70813: /*(" DOWN");*/
				evtStation(-1);
				break;
			case 0xDF2000:
			case 0xFF0016:
			case 0xBF4001:
			case 0xF70801: /*(" 1");*/
				if (!evt.repeat_flag)
					nbStation('1');
				break;
			case 0xDF2010:
			case 0xFF0019:
			case 0xBF4002:
			case 0xF70802: /*(" 2");*/
				if (!evt.repeat_flag)
					nbStation('2');
				break;
			case 0xDF2011:
			case 0xFF000D:
			case 0xBF4003:
			case 0xF70803: /*(" 3");*/
				if (!evt.repeat_flag)
					nbStation('3');
				break;
			case 0xDF2013:
			case 0xFF000C:
			case 0xBF4004:
			case 0xF70804: /*(" 4");*/
				if (!evt.repeat_flag)
					nbStation('4');
				break;
			case 0xDF2014:
			case 0xFF0018:
			case 0xBF4005:
			case 0xF70805: /*(" 5");*/
				if (!evt.repeat_flag)
					nbStation('5');
				break;
			case 0xDF2015:
			case 0xFF005E:
			case 0xBF4006:
			case 0xF70806: /*(" 6");*/
				if (!evt.repeat_flag)
					nbStation('6');
				break;
			case 0xDF2017:
			case 0xFF0008:
			case 0xBF4007:
			case 0xF70807: /*(" 7");*/
				if (!evt.repeat_flag)
					nbStation('7');
				break;
			case 0xDF2018:
			case 0xFF001C:
			case 0xBF4008:
			case 0xF70808: /*(" 8");*/
				if (!evt.repeat_flag)
					nbStation('8');
				break;
			case 0xDF2019:
			case 0xFF005A:
			case 0xBF4009:
			case 0xF70809: /*(" 9");*/
				if (!evt.repeat_flag)
					nbStation('9');
				break;
			case 0xDF2045:
			case 0xFF0042:
			case 0xF70817: /*(" *");*/
			case 0xBF405D: /*FAV*/
			case 0xBF4030: /*(" Play")*/ 
				if (!evt.repeat_flag)
					playStationInt(futurNum);
				break;
			case 0xDF201B:
			case 0xFF0052:
			case 0xBF4000:
			case 0xF70800: /*(" 0");*/
				if (!evt.repeat_flag)
					nbStation('0');
				break;
			case 0xDF205B:
			case 0xFF004A:
			case 0xBF400A:
			case 0xBF4031: /*(" Pause")*/
			case 0xF7081D: /*(" #");*/
				if (!evt.repeat_flag)
					stopStation();
				break;
			case 0xBF4084:
			case 0xDF2007: /*(" Info")*/
				if (!evt.repeat_flag)
					toggletime();
				break;
			case 0xBF4012: /*(" Power");*/
			case 0xBF4015: /*(" Sleep");*/
				if (!evt.repeat_flag)
					kprintf("sys.sleep\r\n");
				break;
			case 0xBF400F: /*(" Input");*/
				if (!evt.repeat_flag)
					radioToggle(true);
				break;
			case 0xBF401A: /*(" Volume+");*/
						   // if (!evt.repeat_flag)
				setRelVolume(+1);
				;
				break;
			case 0xBF401E: /*(" Volume-");*/
						   // if (!evt.repeat_flag)
				setRelVolume(-1);
				break;
			case 0xBF4010: /*(" MUTE");*/
				if (!evt.repeat_flag)
					mute();
				break;
			case 0xBF4037: /*("Eject");*/
				if (!evt.repeat_flag)
					kprintf("sys.boot\r\n");
				break;
			default:;
				/*SERIALX.println(F(" other button   "));*/
			} // End Case
		}
	}
}

void initButtonDevices()
{
	//	struct device_settings *device;
	gpio_num_t enca0;
	gpio_num_t encb0;
	gpio_num_t encbtn0;
	gpio_num_t enca1;
	gpio_num_t encb1;
	gpio_num_t encbtn1;
	bool abtn0, abtn1;
	gpio_get_encoders(&enca0, &encb0, &encbtn0, &enca1, &encb1, &encbtn1);
	if (enca1 == GPIO_NONE)
		isEncoder1 = false; // no encoder
	else
#if USE_PCNT_ENCODER
		encoder1 = ClickEncoderInit(enca1, encb1, encbtn1, ((g_device->options32 & T_ENC1) == 0) ? false : true, PCNT_UNIT_1);
#else
		encoder1 = ClickEncoderInit(enca1, encb1, encbtn1, ((g_device->options32 & T_ENC1) == 0) ? false : true);
#endif
	// Initialize encoder 0
	if (enca0 == GPIO_NONE)
		isEncoder0 = false; // no encoder
	else
#if USE_PCNT_ENCODER
		encoder0 = ClickEncoderInit(enca0, encb0, encbtn0, ((g_device->options32 & T_ENC0) == 0) ? false : true, PCNT_UNIT_0);
#else
		encoder0 = ClickEncoderInit(enca0, encb0, encbtn0, ((g_device->options32 & T_ENC0) == 0) ? false : true);
#endif
	// Get button GPIO pins
	gpio_get_buttons(&enca0, &encb0, &encbtn0, &enca1, &encb1, &encbtn1);
	gpio_get_active_buttons(&abtn0, &abtn1);
	// Initialize button 1
	if (enca1 == GPIO_NONE)
		isButton1 = false; // no buttons
	else
		button1 = ClickButtonsInit(enca1, encb1, encbtn1, abtn1);
	// Initialize button 0
	if (enca0 == GPIO_NONE)
		isButton0 = false; // No button 0
	else
		button0 = ClickButtonsInit(enca0, encb0, encbtn0, abtn0);

	if (!option_get_esplay())
	{
		gpio_get_joysticks(&enca0, &enca1);
		if (enca0 == GPIO_NONE)
			isJoystick0 = false; // no joystick
		else
			joystick0 = ClickJoystickInit(enca0);
		if (enca1 == GPIO_NONE)
			isJoystick1 = false; // no joystick
		else
			joystick1 = ClickJoystickInit(enca1);
	}
	else // Esplay joystick on simple expanded GPIO buttons
	{
		// PCF8574 GPIO expander
		// 1: start, 2: select, 3: up, 4: down, 5: left, 6: right, 7: A, 8: B
		isEsplay = true;
		expButton0 = ClickexpButtonsInit(1, 6, 5, 0);
		expButton1 = ClickexpButtonsInit((int8_t)GPIO_NONE, 3, 4, 0);
		expButton2 = ClickexpButtonsInit(2, 7, 8, 0);
	}
}

// custom ir code init from hardware nvs partition
#define hardware "hardware"
void customKeyInit()
{
	customKey_t index;
	nvs_handle handle;
	const char *klab[] = {"K_UP", "K_LEFT", "K_OK", "K_RIGHT", "K_DOWN", "K_0", "K_1", "K_2", "K_3", "K_4", "K_5", "K_6", "K_7", "K_8", "K_9", "K_STAR", "K_DIESE", "K_INFO"};

	memset(&customKey, 0, sizeof(uint32_t) * 2 * KEY_MAX); // clear custom
	if (open_partition(hardware, "custom_ir_space", NVS_READONLY, &handle) != ESP_OK)
		return;

	for (index = KEY_UP; index < KEY_MAX; index++)
	{
		// get the key in the nvs
		isCustomKey |= gpio_get_ir_key(handle, klab[index], (uint32_t *)&(customKey[index][0]), (uint32_t *)&(customKey[index][1]));
		ESP_LOGV(TAG, " isCustomKey is %d for %d", isCustomKey, index);
		taskYIELD();
	}
	close_partition(handle, hardware);
}

// touch loop
#ifndef CONFIG_NO_XPT2046
void touchLoop()
{
	int tx, ty;
	if (haveTouch())
	{
		if (xpt_read_touch(&tx, &ty, 0))
		{
			ESP_LOGD(TAG, "tx: %d, ty: %d", tx, ty);
			uint16_t width = GetWidth();
			uint16_t height = GetHeight();
			uint16_t xdiv2 = width / 2;
			uint16_t xdiv6 = width / 6;
			uint16_t ydiv2 = height / 2;
			uint16_t ydiv6 = height / 6;
			uint16_t xl = xdiv2 - xdiv6;
			uint16_t xh = xdiv2 + xdiv6;
			uint16_t yl = ydiv2 - ydiv6;
			uint16_t yh = ydiv2 + ydiv6;
			if ((ty > yl) && (ty < yh))
			{
				if ((tx > xl) && (tx < xh))
					startStop(); // center
				else
				{
					if (tx < xl)
						evtStation(-1); //
					else
						evtStation(+1); // evtStation(1);
				}
			}
			else if (ty < yl)
				setRelVolume(+5);
			else
				setRelVolume(-5);
		}
	}
}
#endif
static uint8_t divide = 0;
// indirect call to service
IRAM_ATTR void multiService() // every 1ms
{
#if USE_PCNT_ENCODER == 0
	// ISR need to check encoders evry 1ms
	if (isEncoder0)
		service(encoder0);
	if (isEncoder1)
		service(encoder1);
#endif
	if (divide++ == 10) // only every 10ms
	{
#if USE_PCNT_ENCODER
		// with PCNT dont't need to check encoders evry 1ms
		if (isEncoder0)
			service(encoder0);
		if (isEncoder1)
			service(encoder1);
#endif
		// Process buttons and joysticks
		if (isButton0)
			serviceBtn(button0);
		if (isButton1)
			serviceBtn(button1);
		if (!isEsplay)
		{
			if (isJoystick0)
				serviceJoystick(joystick0);
			if (isJoystick1)
				serviceJoystick(joystick1);
		}
		else
		{
			serviceBtn(expButton0);
			serviceBtn(expButton1);
			serviceBtn(expButton2);
		}
		divide = 0;
	}
}
//--------------------
// LCD display task
//--------------------

void task_lcd(void *pvParams)
{
	event_lcd_t evt;  // lcd event
	evt.lline = NULL; // no line
	event_lcd_t evt1; // lcd event
	ESP_LOGI(TAG, "task_lcd Started, LCD Type %d", lcd_type);
	defaultStateScreen = (g_device->options32 & T_TOGGLETIME) ? stime : smain;
	option_get_lcd_blv(&blv); // init backlight value;
	ESP_LOGI(TAG, "lcd backlight %d", blv);
	if (blv > 100)
	{
		blv = 100;
	}
	backlight_percentage_set(blv);

	if (lcd_type != LCD_NONE)
		drawFrame();

	while (1)
	{

		if (itLcdOut == 1) // switch off the lcd
		{
			sleepLcd();
		}

		if (timerScroll >= 500) // 500 ms
		{
			if (lcd_type != LCD_NONE)
			{
				if (stateScreen == smain)
				{
					scroll();
				}
				if ((stateScreen == stime) || (stateScreen == smain))
				{
					mTscreen = MTREFRESH;
				} // display time

				drawScreen();
			}
			timerScroll = 0;
		}
		if (event_lcd != NULL)
			while (xQueueReceive(event_lcd, &evt, 0))
			{
				//			if (lcd_type == LCD_NONE) continue;
				if (evt.lcmd != lmeta)
					ESP_LOGI(TAG, "event_lcd: %d, %d, mTscreen: %d", evt.lcmd, (int)evt.lline, mTscreen);
				else
					ESP_LOGI(TAG, "event_lcd: %d,  %s, mTscreen: %d", evt.lcmd, evt.lline, mTscreen);
				switch (evt.lcmd)
				{
				case lmeta:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						metaUcg(evt.lline);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						metaU8g2(evt.lline);
#endif
					wakeLcd();
					// Only free evt.lline if it was allocated
					if (evt.lline != NULL)
					{
						free(evt.lline);
						evt.lline = NULL;
					}
					break;
				case licy4:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						icy4Ucg(evt.lline);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						icy4U8g2(evt.lline);
#endif
					if (evt.lline != NULL)
					{
						free(evt.lline);
						evt.lline = NULL;
					}
					break;
				case licy0:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						icy0Ucg(evt.lline);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						icy0U8g2(evt.lline);
#endif
					if (evt.lline != NULL)
					{
						free(evt.lline);
						evt.lline = NULL;
					}
					break;
				case lnameset:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
					{
						namesetUcg(evt.lline);
						statusUcg("STARTING");
					}
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
					{
						namesetU8g2(evt.lline);
						statusU8g2("STARTING");
					}
#endif
					Screen(smain);
					wakeLcd();
					if (evt.lline != NULL && evt.lline != g_device->BTname)
					{
						free(evt.lline);
						evt.lline = NULL;
					}
					break;
				case lstop:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						statusUcg(stopped);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						statusU8g2(stopped);
#endif
					Screen(smain);
					wakeLcd();
					break;
				case lplay:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						playingUcg();
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						playingU8g2();
#endif
					break;
				case lvol:
					// ignore it if the next is a lvol
					if (xQueuePeek(event_lcd, &evt1, 0))
						if (evt1.lcmd == lvol)
							break;
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						setVolumeUcg(volume);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						setVolumeU8g2(volume);
#endif
					if (dvolume)
					{
						Screen(svolume);
						wakeLcd();
					}
					dvolume = true;
					break;
				case lovol:
					dvolume = false; // don't show volume on start station
					break;
				case estation:
					if (xQueuePeek(event_lcd, &evt1, 0))
						if (evt1.lcmd == estation)
						{
							evt.lline = NULL;
							break;
						}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
					ESP_LOGD(TAG, "estation val: %lu", (uint32_t)evt.lline);
#else
					ESP_LOGD(TAG, "estation val: %d", (uint32_t)evt.lline);
#endif
					changeStation((uint32_t)evt.lline);
					Screen(sstation);
					wakeLcd();
					evt.lline = NULL; // just a number
					break;
				case eclrs:
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
					if (isColor)
						ucg_ClearScreen(&ucg);
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
					if (!isColor)
						u8g2_ClearDisplay(&u8g2);
#endif
					break;
				case escreen:
					Screen((uint32_t)evt.lline);
					wakeLcd();
					evt.lline = NULL; // just a number Don't free
					break;
				case etoggle:
					defaultStateScreen = (stateScreen == smain) ? stime : smain;
					(stateScreen == smain) ? Screen(stime) : Screen(smain);
					g_device->options32 = (defaultStateScreen == smain) ? g_device->options32 & NT_TOGGLETIME : g_device->options32 | T_TOGGLETIME;
					wakeLcd();
					saveDeviceSettings(g_device);
					break;
				default:;
				}
				if (evt.lline != NULL)
					vTaskDelay(pdMS_TO_TICKS(10));
				// ESP_LOGI(TAG, "Freeing evt.lline: %s", evt.lline);
				// free(evt.lline);
				evt.lline = NULL;
				vTaskDelay(pdMS_TO_TICKS(10));
			}
		if ((event_lcd) && (!uxQueueMessagesWaiting(event_lcd)))
			vTaskDelay(pdMS_TO_TICKS(20));
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	vTaskDelete(NULL);
}

//-------------------
// Main task of addon
//-------------------
#ifndef CONFIG_NO_IRNEC
extern void rmt_nec_rx_task();
#endif
void task_addon(void *pvParams)
{
	xTaskHandle pxCreatedTask;
	customKeyInit();
	initButtonDevices();
	adcInit();
	serviceAddon = &multiService;
	; // connect the 1ms interruption
	futurNum = getCurrentStation();
	bool _isIR = isIR();
	if (_isIR) // don't start task if there is no IR
	{
		ESP_LOGI(TAG, "IR TASK STARTING");
		// ir
		//  queue for events of the IR nec rx
		event_ir = xQueueCreate(5, sizeof(event_ir_t));
		ESP_LOGD(TAG, "event_ir: %x", (int)event_ir);
#ifndef CONFIG_NO_IRNEC
		xTaskCreatePinnedToCore(rmt_nec_rx_task, "rmt_nec_rx_task", 2148, NULL, PRIO_RMT, &pxCreatedTask, CPU_RMT);
		ESP_LOGI(TAG, "%s task: %x", "rmt_nec_rx_task", (unsigned int)pxCreatedTask);
#endif
	}
	else
	{
		ESP_LOGI(TAG, "No IR GPIO defined, IR task will not start");
	}

#if !CONFIG_DISPLAY_TYPE_NONE
	if (g_device->lcd_type != LCD_NONE)
	{
		// queue for events of the lcd
		event_lcd = xQueueCreate(10, sizeof(event_lcd_t));
		ESP_LOGI(TAG, "event_lcd: %x", (int)event_lcd);
#if CONFIG_IDF_TARGET_ESP32S3
		xTaskCreatePinnedToCore(task_lcd, "task_lcd", 2800, NULL, PRIO_LCD, &pxTaskLcd, CPU_LCD); // #memchange 2304
#else
		xTaskCreatePinnedToCore(task_lcd, "task_lcd", 2048, NULL, PRIO_LCD, &pxTaskLcd, CPU_LCD); // #memchange 2304
#endif
		ESP_LOGI(TAG, "%s task: %x", "task_lcd", (unsigned int)pxTaskLcd);
#ifndef CONFIG_NO_XPT2046
		getTaskLcd(&pxTaskLcd); // give the handle to xpt
#endif
	}
#endif

	// Configure Deep Sleep start and wakeup options
	deepSleepConf(); // also called in app_main.c

	while (1)
	{
		adcLoop();	  // compute the adc keyboard and battery
		periphLoop(); // compute the encoder the buttons and joysticks
#ifndef CONFIG_NO_IRNEC
		if (_isIR)
			irLoop(); // compute the ir if task started
#endif
#ifndef CONFIG_NO_XPT2046
		touchLoop(); // compute the touch screen
#endif
		if (itAskTime) // time to ntp. Don't do that in interrupt.
		{
			if (ntp_get_time(&dt))
			{
				applyTZ(dt);
				timestamp = mktime(dt);
				syncTime = true;
			}
			itAskTime = false;
		}

		if (timerScreen >= 3) //  sec timeout transient screen
		{
			adcBatLoop(); // every 3 sec, check battery
			timerScreen = 0;

			// Only in BT mode
			if (!_isRadio)
			{
				// If current screen is not smain or stime, allow timer to switch back
				if (stateScreen != smain && stateScreen != stime && stateScreen != snull)
				{
					evtScreen(defaultStateScreen);
				}
				// Do NOT auto-switch between smain <-> stime in BT mode
			}
			else
			{
				// Original radio mode logic
				if ((stateScreen != defaultStateScreen) && (stateScreen != snull))
				{
					// Play the changed station on return to main screen
					if (strlen(irStr) > 0)
					{
						futurNum = atoi(irStr);
						if (futurNum > 254)
							futurNum = 0;
						playable = true;
						// clear the number
						irStr[0] = 0;
					}
					if ((strlen(
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
							 isColor ? getNameNumUcg() :
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
							 !isColor ? getNameNumU8g2()
									  :
#endif
									  "") != 0) &&
						playable
#if CONFIG_DISPLAY_TYPE_COLOR || CONFIG_DISPLAY_TYPE_ALL
						&& (!isColor || (futurNum != atoi(getNameNumUcg())))
#endif
#if CONFIG_DISPLAY_TYPE_MONO || CONFIG_DISPLAY_TYPE_ALL
						&& (isColor || (futurNum != atoi(getNameNumU8g2())))
#endif
					)
					{
						playStationInt(futurNum);
						vTaskDelay(pdMS_TO_TICKS(20));
					}
					if (!itAskStime)
					{
						if ((defaultStateScreen == stime) && (stateScreen != smain))
							evtScreen(smain);
						else if ((defaultStateScreen == stime) && (stateScreen == smain))
							evtScreen(stime);
						else if (stateScreen != defaultStateScreen)
							evtScreen(defaultStateScreen); // Back to the old screen
					}
				}
				if (itAskStime && (stateScreen != stime))
					evtScreen(stime);
			}
		}
		// Hardware-triggered deep sleep disabled — deep sleep now only via command or long-press
		// if (checkDeepSleepInput())
		// 	deepSleepStart();

		vTaskDelay(pdMS_TO_TICKS(20));
	}
	vTaskDelete(NULL);
}

// force a new dt ntp fetch
void addonDt() { itAskTime = true; }

////////////////////////////////////////
// parse the karadio received line and do the job
void addonParse(const char *fmt, ...)
{
	event_lcd_t evt;
	char *line = NULL;
	int rlen;
	line = (char *)kmalloc(1024);
	if (line == NULL)
		return;
	line[0] = 0;
	strcpy(line, "ok\n");

	va_list ap;
	va_start(ap, fmt);
	rlen = vsnprintf(line, 1024, fmt, ap);
	va_end(ap);
	if (rlen < 0 || rlen >= 1024)
	{
		free(line);
		return;
	}
	char *tmp = realloc(line, rlen + 1);
	if (tmp == NULL)
	{
		free(line);
		return;
	}
	line = tmp;
	ESP_LOGV(TAG, "LINE: %s", line);
	evt.lcmd = -1;
	char *ici;

	////// Meta title  ##CLI.META#:
	if ((ici = strstr(line, "META#: ")) != NULL)
	{
		evt.lcmd = lmeta;
		evt.lline = kmalloc(strlen(ici) + 1);
		strcpy(evt.lline, ici);
	}
	else
		////// ICY4 Description  ##CLI.ICY4#:
		if ((ici = strstr(line, "ICY4#: ")) != NULL)
		{
			evt.lcmd = licy4;
			evt.lline = kmalloc(strlen(ici) + 1);
			strcpy(evt.lline, ici);
		}
		else
			////// ICY0 station name   ##CLI.ICY0#:
			if ((ici = strstr(line, "ICY0#: ")) != NULL)
			{
				evt.lcmd = licy0;
				evt.lline = kmalloc(strlen(ici) + 1);
				strcpy(evt.lline, ici);
			}
			else
				////// STOPPED  ##CLI.STOPPED#
				if (((ici = strstr(line, "STOPPED")) != NULL) && (strstr(line, "C_HDER") == NULL) && (strstr(line, "C_PLIST") == NULL))
				{
					state = false;
					evt.lcmd = lstop;
					evt.lline = NULL;
				}
				else
					//////Nameset    ##CLI.NAMESET#:
					if ((ici = strstr(line, "MESET#: ")) != NULL)
					{
						evt.lcmd = lnameset;
						evt.lline = kmalloc(strlen(ici) + 1);
						strcpy(evt.lline, ici);
						ESP_LOGI(TAG, "Debug: Nameset: %s", evt.lline);
					}
					else
						//////Playing    ##CLI.PLAYING#
						if ((ici = strstr(line, "YING#")) != NULL)
						{
							state = true;
							itAskStime = false;
							evt.lcmd = lplay;
							evt.lline = NULL;
						}
						else
							//////Volume   ##CLI.VOL#:
							if ((ici = strstr(line, "VOL#:")) != NULL)
							{
								if (*(ici + 6) != 'x') // ignore help display.
								{
									volume = atoi(ici + 6);
									evt.lcmd = lvol;
									evt.lline = NULL; // atoi(ici+6);
								}
							}
							else
								//////Volume offset    ##CLI.OVOLSET#:
								if ((ici = strstr(line, "OVOLSET#:")) != NULL)
								{
									evt.lcmd = lovol;
									evt.lline = NULL;
								}

	if (evt.lcmd != -1 && lcd_type != LCD_NONE && event_lcd != NULL)
	{
		if (xQueueSend(event_lcd, &evt, 0) != pdTRUE)
		{
			// If send fails, free evt.lline to avoid memory leak
			if (evt.lline != NULL)
			{
				free(evt.lline);
				evt.lline = NULL;
			}
		}
		else if (pxTaskLcd == NULL) // evt.lline is cleaned in task_lcd but if task is not running
		{
			free(evt.lline);
			evt.lline = NULL;
		}
	}
	free(line);
}

/** Configure Deep Sleep: source and wakeup options. */
bool deepSleepConf(void)
{
	/** 1. get the pin number and trigger level from NVS configuration. */
	gpio_get_pinSleep(&deepSleep_io, &deepSleepLevel);

	if (GPIO_NONE != deepSleep_io)
	{
		/** 2. Initialize GPIO. */
		gpio_config_t gpio_conf;
		gpio_conf.mode = GPIO_MODE_INPUT;
		gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
		gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
		gpio_conf.intr_type = GPIO_INTR_DISABLE;
		gpio_conf.pin_bit_mask = ((uint64_t)(((uint64_t)1) << deepSleep_io));
		ESP_ERROR_CHECK(gpio_config(&gpio_conf));

		/** 3. Configure Deep Sleep External wakeup (ext0). */
		/** Wake up (EXT0) when GPIO deepSleep_io pin level is opposite to deepSleepLevel. */
		esp_sleep_enable_ext0_wakeup(deepSleep_io, !deepSleepLevel);
	}

	return true;
}

// Helper: return true if GPIO can be used as RTC IO (required for EXT1 wake)
static bool is_rtc_gpio(gpio_num_t g)
{
#if CONFIG_IDF_TARGET_ESP32S3
	return (g >= GPIO_NUM_0 && g <= GPIO_NUM_21);
#else
	switch (g)
	{
	case GPIO_NUM_0:
	case GPIO_NUM_2:
	case GPIO_NUM_4:
	case GPIO_NUM_12:
	case GPIO_NUM_13:
	case GPIO_NUM_14:
	case GPIO_NUM_15:
	case GPIO_NUM_25:
	case GPIO_NUM_26:
	case GPIO_NUM_27:
	case GPIO_NUM_32:
	case GPIO_NUM_33:
	case GPIO_NUM_34:
	case GPIO_NUM_35:
	case GPIO_NUM_36:
	case GPIO_NUM_37:
	case GPIO_NUM_38:
	case GPIO_NUM_39:
		return true;
	default:
		return false;
	}
#endif
}

/** Configure wakeup sources for deep sleep */
void deepSleepEnableWakeup(void)
{
	// Only enable single-pin EXT0 wake using the configured `deepSleep_io` (from NVS)
	if (deepSleep_io != GPIO_NONE)
	{
		if (is_rtc_gpio(deepSleep_io))
		{
			// ext0 wakes on a single RTC pin at the specified level
			esp_sleep_enable_ext0_wakeup(deepSleep_io, !deepSleepLevel);
			ESP_LOGI(TAG, "sleep: EXT0 wakeup enabled on GPIO%d (level %d)", deepSleep_io, !deepSleepLevel);
		}
		else
		{
			ESP_LOGW(TAG, "sleep: configured deepSleep_io GPIO%d is not RTC-capable; cannot enable EXT0 wake. No GPIO wake configured.", deepSleep_io);
		}
	}
	else
	{
		ESP_LOGI(TAG, "sleep: No deepSleep_io configured; GPIO wake disabled.");
	}
}

/** Check Deep Sleep GPIO input. */
/** If deepSleep_io pin is set to deepSleepLevel, then trigger Deep Sleep Power Saving mode */
bool checkDeepSleepInput(void)
{
	if (GPIO_NONE != deepSleep_io)
	{
		if (deepSleepLevel == gpio_get_level(deepSleep_io))
			// return true;
			return false; // Bypass gpio deep sleep, only from function call, gpio momentary switch only for wakeup
		else
			return false;
	}
	else
		return false;
}

/** Enter ESP32 Deep Sleep with the configured wakeup options, and powerdown peripherals, */
/** when P_SLEEP GPIO is set to P_LEVEL_SLEEP. */
/** https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html */
void deepSleepStart(void)
{
	/** 1. Enter peripherals sleep */
	sleepLcd(); // LCD
				/** note that PCM5102 also enters powerdown because pins P_I2S_LRCK and P_I2S_BCLK are low. */

	/** Diagnostic: report configured sleep/wakeup settings and current pin level */
	if (deepSleep_io != GPIO_NONE)
	{
		int configured_pin = (int)deepSleep_io;
		int p_level = deepSleepLevel ? 1 : 0;
		int wake_level = !deepSleepLevel ? 1 : 0;
		int cur_level = -1;
		// attempt to read current level (GPIO must be configured as input earlier)
		cur_level = gpio_get_level(deepSleep_io);
		kprintf("##Sleep config: P_SLEEP=%d, P_LEVEL_SLEEP=%d, wake_on=%d, cur_level=%d#\r\n", configured_pin, p_level, wake_level, cur_level);
		ESP_LOGI(TAG, "sleep: configured P_SLEEP=%d P_LEVEL_SLEEP=%d wake_on=%d cur_level=%d", configured_pin, p_level, wake_level, cur_level);
	}
	else
	{
		kprintf("##Sleep config: P_SLEEP=none#\r\n");
	}

	/** 2. Safety check: avoid immediate wake if the configured wake level is already present on the wake pin. */
	if (deepSleep_io != GPIO_NONE)
	{
		if (is_rtc_gpio(deepSleep_io))
		{
			int pin_level = gpio_get_level(deepSleep_io);
			int wake_level = !deepSleepLevel; // ext0 wakes on opposite of deepSleepLevel
			if (pin_level == wake_level)
			{
				ESP_LOGW(TAG, "sleep: aborting because GPIO%d already at wake level %d", deepSleep_io, wake_level);
				kprintf("##Sleep aborted: GPIO%d already at wake level %d#\r\n", deepSleep_io, wake_level);
				return; // do not enter deep sleep (would wake instantly)
			}
		}
		else
		{
			ESP_LOGW(TAG, "sleep: deepSleep_io GPIO%d not RTC-capable; no GPIO wake configured", deepSleep_io);
			kprintf("##Sleep aborted: GPIO%d not RTC-capable; no GPIO wake configured#\r\n", deepSleep_io);
			return;
		}
	}

	/** 3. Final resample to avoid transient wake sources: sample the pin a few times before committing to sleep. */
	if (deepSleep_io != GPIO_NONE)
	{
		int wake_level = !deepSleepLevel;
		const int samples = 5;
		const int delay_ms = 10; // 10ms between samples
		int hit = 0;
		for (int i = 0; i < samples; ++i)
		{
			int v = gpio_get_level(deepSleep_io);
			if (v == wake_level)
			{
				hit = 1;
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(delay_ms));
		}
		if (hit)
		{
			ESP_LOGW(TAG, "sleep: aborting because GPIO%d sampled as wake level during resample", deepSleep_io);
			kprintf("##Sleep aborted: GPIO%d sampled as wake level during resample#\r\n", deepSleep_io);
			return;
		}
	}

	/** 4. Enter ESP32 deep sleep with the configured wakeup options. */
	/*	YMMV: rtc_gpio_isolate(deepSleep_io); // disconnect GPIO from internal circuits in deep sleep, to minimize leakage current. */
	kprintf("##Calling esp_deep_sleep_start() now... flush uart then enter deep sleep##\r\n");
	ESP_LOGI(TAG, "sleep: calling esp_deep_sleep_start()");
	/* Short delay to allow UART output to flush so logs are visible before sleep */
	vTaskDelay(pdMS_TO_TICKS(50));
	esp_deep_sleep_start();
	/* If we return here, deep sleep did not start. Log and continue running. */
	kprintf("##esp_deep_sleep_start() returned unexpectedly; not entering deep sleep##\r\n");
	ESP_LOGE(TAG, "sleep: esp_deep_sleep_start() returned unexpectedly; deep sleep not entered");
}

// IR diagnostics
uint32_t ir_get_event_count(void)
{
	return ir_event_count;
}

bool ir_is_loop_called(void)
{
	return ir_loop_called;
}

bool ir_get_is_custom_key(void)
{
	return isCustomKey;
}

void ir_get_last_events(ir_event_log_t *events, uint8_t *count)
{
	*count = IR_EVENT_LOG_SIZE;
	for (uint8_t i = 0; i < IR_EVENT_LOG_SIZE; i++)
	{
		events[i] = ir_event_log[i];
	}
}

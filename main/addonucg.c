/******************************************************************************
 *
 * Copyright 2018 karawin (http://www.karawin.fr)
 *
 *******************************************************************************/

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_timer.h"
// #include "esp_heap_trace.h"
#include "gpio.h"
#include "addon.h"
#include "addonucg.h"
#include "ucg_esp32_hal.h"
#include "app_main.h"
#include <time.h>
#include "esp_log.h"
#include "logo.h"
#include "interface.h"
#include "eeprom.h"
#include "addoncommon.h"
#include "audio_player.h"
#include "xpt2046.h"
#include "ucg_karadio32_fonts.h"
#include "sdkconfig.h"

#define TAG "addonucg"

#define ucg_SetColori(a, b, c, d) ucg_SetColor(a, 0, b, c, d)

static uint8_t displayPaletteChannel(display_color_id_t color, uint8_t shift)
{
	return (uint8_t)((g_device->display_palette[color] >> shift) & 0xFF);
}

static uint8_t displayBatteryChannel(uint8_t percent, uint8_t shift)
{
	int low = displayPaletteChannel(DISPLAY_COLOR_BATTERY_LOW, shift);
	int high = displayPaletteChannel(DISPLAY_COLOR_BATTERY_HIGH, shift);
	return (uint8_t)(low + ((high - low) * percent) / 100);
}

#define DISPLAY_RGB(color) displayPaletteChannel(color, 16), displayPaletteChannel(color, 8), displayPaletteChannel(color, 0)
#define CTBACK DISPLAY_RGB(DISPLAY_COLOR_HEADER_BACKGROUND)
#define CTTFONT DISPLAY_RGB(DISPLAY_COLOR_HEADER_TEXT)
#define CBODY DISPLAY_RGB(DISPLAY_COLOR_BODY_TEXT)
#define CBLACK DISPLAY_RGB(DISPLAY_COLOR_BLACK)
#define CWHITE DISPLAY_RGB(DISPLAY_COLOR_WHITE)
#define CSTATUS DISPLAY_RGB(DISPLAY_COLOR_STATUS_TEXT)
#define CSTATION2 DISPLAY_RGB(DISPLAY_COLOR_STATION_SECOND)
#define CTITLE1 DISPLAY_RGB(DISPLAY_COLOR_TITLE_FIRST)
#define CTITLE2 DISPLAY_RGB(DISPLAY_COLOR_TITLE_SECOND)
#define CVOLUME_TEXT DISPLAY_RGB(DISPLAY_COLOR_VOLUME_TEXT)
#define CVOLUME_FRAME DISPLAY_RGB(DISPLAY_COLOR_VOLUME_FRAME)
#define CVOLUME_FILL DISPLAY_RGB(DISPLAY_COLOR_VOLUME_FILL)
#define CTIME_TEXT DISPLAY_RGB(DISPLAY_COLOR_TIME_TEXT)
#define CPROGRESS_FRAME DISPLAY_RGB(DISPLAY_COLOR_PROGRESS_FRAME)
#define CPROGRESS_FILL DISPLAY_RGB(DISPLAY_COLOR_PROGRESS_FILL)
#define CBATTERY_COLOR(percent) displayBatteryChannel(percent, 16), displayBatteryChannel(percent, 8), displayBatteryChannel(percent, 0)

// nams <--> num of line
#define STATIONNAME 0
#define STATION1 1
#define STATION2 2
#define IP 3
#define GENRE 2
#define TITLE1 3
#define TITLE11 4
#define TITLE2 5
#define TITLE21 6
#define VOLUME 7
#define TIME 8
#define LATIN 2 // choose Latin subset
static uint16_t HHeader = 40;

static char TTitleStr[15];
static char TTimeStr[15];
static audio_stream_info_t lastStreamInfo;
static bool streamInfoDrawn;
static bool transition_body_cleared;
static unsigned lastStreamInfoDrawTime;

static void drawTimeHeaderUcg(const char *date, unsigned timein);
static void drawBtHeaderUcg(void);
static void drawRadioMainHeaderUcg(void);

// BT main screen: top bar (device name + stream info) replaces the white name line
#define BT_HEADER_NAME_LEN 11
static bool btHeaderLayout(void)
{
	return !_isRadio && yy > 80;
}

static bool radioMainHeaderLayout(void)
{
	return _isRadio && x == 320 && yy == 240;
}

static uint16_t btBodyShift(void)
{
	if (radioMainHeaderLayout())
		return HHeader;
	return (btHeaderLayout() && HHeader > y) ? HHeader - y : 0;
}

static bool btNameIsMac(const char *s)
{
	if (strlen(s) != 17)
		return false;
	for (int i = 0; i < 17; i++)
	{
		if (i % 3 == 2)
		{
			if (s[i] != ':')
				return false;
		}
		else if (!isxdigit((unsigned char)s[i]))
			return false;
	}
	return true;
}

// Connected device name (when known) or the ESP32 BT name, cut to BT_HEADER_NAME_LEN
static void btHeaderTitle(char *out, size_t size)
{
	const char *src = nameset;
	if (src[0] == 0 || btNameIsMac(src))
		src = g_device->BTname;
	size_t n = strlen(src);
	if (n > BT_HEADER_NAME_LEN)
		n = BT_HEADER_NAME_LEN;
	if (n > size - 1)
		n = size - 1;
	memcpy(out, src, n);
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = 0;
}
typedef enum Lang
{
	Latin,
	Cyrillic,
	Greek
} LANG;
static LANG charset = Latin; // latin or other

////////////////////////////////////////
typedef enum sizefont
{
	small,
	text,
	middle,
	large
} sizefont;
void setfont(int size)
{
	int inX = x;
	int inY = 0;
	if (yy <= 80)
		inX = 96; // corrected for small yy
				  //	printf("setfont charset: %d, size: %d,yy: %d, x: %d,  inX: %d\n",charset,size,yy,x,inX);
	switch (size)
	{
	case small:
		switch (inX)
		{
		case 320:
			// fall through
		case 240:
			ucg_SetFont(&ucg, ucg_font_6x13_mf);
			break;
		case 128:
			ucg_SetFont(&ucg, ucg_font_4x6_mf);
			break;
		case 96:
			ucg_SetFont(&ucg, ucg_font_u8glib_4_hf);
			break;
		case 132:
		default: // 160
			ucg_SetFont(&ucg, ucg_font_5x8_mf);
			;
		}
		break;

	case text:
		if ((yy == 135) && (x == 240))
			inX = 160;
		switch (inX)
		{
		case 320:
			// fall through
		case 240:
			ucg_SetFont(&ucg, ucg_font_inr16_mf);
			inY = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg);
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox5h);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR18_gr);
				break;
			default:;
				// case Latin:ucg_SetFont(&ucg,ucg_font_inr16_mf );break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox5h ):ucg_SetFont(&ucg,ucg_font_inr16_mf ) ;
			break;
		case 128:
			ucg_SetFont(&ucg, ucg_font_5x7_mf);
			inY = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg);
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox1c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR14_gr);
				break;
			default:;
				// case Latin:ucg_SetFont(&ucg,ucg_font_5x7_mf );break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox1c ):ucg_SetFont(&ucg,ucg_font_5x7_mf) ;
			break;
		case 132:
			ucg_SetFont(&ucg, ucg_font_5x7_mf);
			inY = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg);
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox1c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR14_gr);
				break;
			default:;
				// case Latin:ucg_SetFont(&ucg,ucg_font_5x7_mf );break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox1c ):ucg_SetFont(&ucg,ucg_font_5x7_mf) ;
			break;
		case 96:
			ucg_SetFont(&ucg, ucg_font_5x7_mf);
			inY = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg);
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox1c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_5x7_gr);
				break;
			default:;
				// case Latin:ucg_SetFont(&ucg,ucg_font_5x7_mf );break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox1c ):ucg_SetFont(&ucg,ucg_font_4x6_mf) ;
			break;
		default: // 160
			ucg_SetFont(&ucg, ucg_font_6x13_mf);
			inY = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg);
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox1c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_6x13_gr);
				break;
			default:;
				// case Latin:ucg_SetFont(&ucg,ucg_font_6x13_mf );break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox1c ):ucg_SetFont(&ucg,ucg_font_6x13_mf) ;
			;
		}
		break;

	case middle:
		switch (inX)
		{
		case 320:
			// fall through
		case 240:
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox5h);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR24_gr);
				break;
			default:
			// case Latin:ucg_SetFont(&ucg,ucg_font_inr33_mf );break;
			case Latin:
				ucg_SetFont(&ucg, ucg_font_inb19_tf);
				break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox5h ):ucg_SetFont(&ucg,ucg_font_inr33_mf);
			break;
		case 128:
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox3c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR14_gr);
				break;
			default:
			case Latin:
				ucg_SetFont(&ucg, ucg_font_7x14_mf);
				break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox3c ):ucg_SetFont(&ucg,ucg_font_7x14_mf);
			break;
		case 96:
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox2h);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_5x7_gr);
				break;
			default:
			case Latin:
				ucg_SetFont(&ucg, ucg_font_6x12_mf);
				break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox2h ):ucg_SetFont(&ucg,ucg_font_6x12_mf);
			break;
		case 132:
		default: // 160
			switch (charset)
			{
			case Cyrillic:
				ucg_SetFont(&ucg, ucg_font_crox3c);
				break;
			case Greek:
				ucg_SetFont(&ucg, ucg_font_helvR14_gr);
				break;
			default:
			case Latin:
				ucg_SetFont(&ucg, ucg_font_fur14_tf);
				break;
			}
			//			charset?ucg_SetFont(&ucg,ucg_font_crox3c ):ucg_SetFont(&ucg,ucg_font_fur14_tf);

			;
		}
		break;
	case large:
		switch (inX)
		{
		case 320:
			// fall through
		case 240:
			//			ucg_SetFont(&ucg,ucg_font_inr53_mf);
			ucg_SetFont(&ucg, ucg_font_osr41_hn);
			break;
		case 128:
			//			ucg_SetFont(&ucg,ucg_font_helvR12_hf);
			ucg_SetFont(&ucg, ucg_font_osr21_hn);
			break;
		case 96:
			//			ucg_SetFont(&ucg,ucg_font_helvR12_hf);
			ucg_SetFont(&ucg, ucg_font_osr18_hn);
			break;
		case 132:
		default: // 160
				 //			ucg_SetFont(&ucg,ucg_font_inr38_mr);
			ucg_SetFont(&ucg, ucg_font_osr35_hn);
			// ucg_SetFont(&ucg, ucg_font_helvB18_tf);
			;
		}
		break;
	default:
		ESP_LOGE(TAG, "Default for size %d\n", size);
	}

	if (inY != 0)
	{
		if (yy <= 80)
			y = inY + 2;
		else
			y = inY + 3;
	}
	else
	{
		if (yy <= 80)
			y = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg) + 2; // interline
		else
			y = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg) + 3; // interline
	}
}

////////////////////////////////////////
char *getNameNumUcg()
{
	return nameNum;
}

void setVolumeUcg(uint16_t vol) { volume = vol; }

////////////////////////////////////////
// Clear all buffers and indexes
void clearAllUcg()
{
	title[0] = 0;
	station[0] = 0;
	for (int i = 1; i < LINES; i++)
	{
		lline[i] = NULL;
		iline[i] = 0;
		tline[i] = 0;
		;
		mline[i] = 1;
	}
}
////////////////////////////////////////
void cleartitleUcg(uint8_t froml)
{
	title[0] = 0;
	for (int i = froml; i < LINES; i++) // clear lines
	{
		lline[i] = NULL;
		iline[i] = 0;
		tline[i] = 0;
		mline[i] = 1;
	}
}

// Thanks to Max
void ucEraseSlashes(char *str)
{
	// Symbols: \" \' \\ \? \/
	char *sym = str, *sym1;
	if (str != NULL)
	{
		while (*sym != 0)
		{
			if (*sym == 0x5c)
			{
				sym1 = sym + 1;
				if (*sym1 == 0x22 || *sym1 == 0x27 || *sym1 == 0x5c || *sym1 == 0x3f || *sym1 == 0x2f)
				{
					*sym = 0x1f; // Erase \ to non-printable symbol
					sym++;
				}
			}
			sym++;
		}
	}
}
//-Max

// non linear cyrillic conversion
struct _utf8ToAscii_t
{
	uint16_t utf8;
	uint8_t ascii;
};
typedef struct _utf8ToAscii_t utf8ToAscii_t;
#define UTF8TO1251 30
const utf8ToAscii_t utf8To1251[UTF8TO1251] = {
	{0x401, 0x45 /*0xa8*/},
	{0x402, 0x80},
	{0x403, 0x81},
	{0x404, 0x45 /*0xaa*/},
	{0x405, 0xbd},
	{0x406, 0x49 /*0xb2*/},
	{0x407, 0x49 /*0xaf*/},
	{0x408, 0xa3},
	{0x409, 0x8a},
	{0x40a, 0x8c},
	{0x40b, 0x8e},
	{0x40c, 0x8d},
	{0x40e, 0xa1},
	{0x40f, 0x8f},
	{0x452, 0x90},
	{0x451, 0x65 /*0xb8*/},
	{0x453, 0x83},
	{0x454, 0xE5 /*0xba*/},
	{0x455, 0xbe},
	{0x456, 0x69 /*0xb3*/},
	{0x457, 0x69 /*0xbf*/},
	{0x458, 0x6a /*0xbc*/},
	{0x459, 0x9a},
	{0x45a, 0x9c},
	{0x45b, 0x9e},
	{0x45c, 0x9d},
	{0x45f, 0x9f},
	{0x490, 0xa5},
	{0x491, 0xb4},
	{0, 0}};

// Cyrillic
uint8_t to1251(uint16_t utf8)
{
	int i;
	if (utf8 > 0x491)
		return 0x1f;
	for (i = 0; i < UTF8TO1251; i++)
	{
		if (utf8 == utf8To1251[i].utf8)
		{
			//			printf("to1251: utf8: %x, ret: %x\n",utf8,utf8To1251[i].ascii);
			return utf8To1251[i].ascii;
		}
	}
	//	printf("to1251: utf8: %x, ret: %x\n",utf8,(utf8 - 0x350)& 0xff);
	return ((utf8 - 0x350) & 0xff);
}

// Greek
uint8_t to1253(uint16_t utf8)
{
	//	printf("to1253: utf8: %x, ret: %x\n",utf8,(utf8 - 0x300)& 0xff);
	return ((utf8 - 0x300) & 0xff);
}

#define UTF8TO8859 63
const utf8ToAscii_t utf8To8859[UTF8TO8859] = {
	{0x100, 0x41}, {0x102, 0x41}, {0x104, 0x41} /*C484*/, /*A*/
	{0x101, 0x61},
	{0x103, 0x61},
	{0x105, 0x61} /*C484*/, /*a*/
	{0x106, 0x43} /*c486*/,
	{0x108, 0x43} /*c488*/,
	{0x1a, 0x43} /*c48a*/,
	{0x10c, 0x43} /*c48c*/, /*C*/
	{0x107, 0x63} /*c487 c*/,
	{0x109, 0x63} /*89 c*/,
	{0x10b, 0x63} /*8b c*/,
	{0x10d, 0x63} /*8d c*/, /*c*/
	{0x10E, 0x44},
	{0x110, 0x44} /*C490*/, /*D*/
	{0x10F, 0x44},
	{0x111, 0x44} /*C491*/, /*d*/
	{0x11a, 0x45} /*c49A*/, /*E*/
	{0x11b, 0x65} /*c49B*/, /*e*/
	{0x11c, 0x47} /*c49C*/,
	{0x11e, 0x47} /*c49e*/,
	{0x120, 0x47} /*c4a0*/,
	{0x122, 0x47} /*c4a2*/, /*G*/
	{0x11d, 0x67} /*c49D*/,
	{0x11f, 0x67} /*c49f*/,
	{0x121, 0x67} /*c4a1*/,
	{0x123, 0x67} /*c4a3*/, /*g*/
	{0x124, 0x48},
	{0x126, 0x48} /*C4a6*/, /*H*/
	{0x125, 0x68},
	{0x127, 0x68} /*C4a7*/, /*h*/
	{0x130, 0x49},
	{0x132, 0x49} /*C4b0*/, /*I*/
	{0x131, 0x69},
	{0x133, 0x69} /*C4b1*/, /*i*/
	{0x147, 0x4e} /*c587*/, /*N*/
	{0x148, 0x6e} /*c588*/, /*n*/
	{0x158, 0x52} /*c598*/, /*R*/
	{0x159, 0x72} /*c599*/, /*r*/
	{0x160, 0x53} /*c5a0*/, /*S*/
	{0x161, 0x73} /*c5a1*/, /*s*/
	{0x166, 0x54} /*c5a6*/, /*T*/
	{0x167, 0x74} /*c5a7*/, /*t*/
	{0x168, 0x55},
	{0x16A, 0x55},
	{0x16C, 0x55},
	{0x16E, 0x55},
	{0x170, 0x55},
	{0x172, 0x55} /*c5AF*/, /*U*/
	{0x169, 0x75},
	{0x16B, 0x75},
	{0x16D, 0x75},
	{0x16F, 0x75},
	{0x171, 0x75},
	{0x173, 0x75} /*c5AF*/, /*u*/
	{0x179, 0x5a},
	{0x17b, 0x5a},
	{0x17d, 0x5a} /*C5BD*/, /*Z*/
	{0x17a, 0x7a},
	{0x17c, 0x7a},
	{0x17e, 0x7a} /*C5BE*/, /*z*/
	{0, 0}};

// Latin
uint8_t to8859(uint16_t utf8)
{
	int i;
	if (utf8 > 0x1ff)
		return 0x1f;
	for (i = 0; i < UTF8TO8859; i++)
	{
		if (utf8 == utf8To8859[i].utf8)
		{
			return utf8To8859[i].ascii;
		}
	}
	return ((utf8) & 0xff);
}
#define UTF8TO8859_2 19
const utf8ToAscii_t utf8To8859_2[UTF8TO8859_2] = {
	{0x0104, 0xA1}, // Ą
	{0x0105, 0xB1}, // ą
	{0x0106, 0xC6}, // Ć
	{0x0107, 0xE6}, // ć
	{0x0118, 0xCA}, // Ę
	{0x0119, 0xEA}, // ę
	{0x0141, 0xA3}, // Ł
	{0x0142, 0xB3}, // ł
	{0x0143, 0xD1}, // Ń
	{0x0144, 0xF1}, // ń
	{0x00D3, 0xD3}, // Ó
	{0x00F3, 0xF3}, // ó
	{0x015A, 0xA6}, // Ś
	{0x015B, 0xB6}, // ś
	{0x0179, 0xAC}, // Ź
	{0x017A, 0xBC}, // ź
	{0x017B, 0xAF}, // Ż
	{0x017C, 0xBF}, // ż
	{0, 0}};

// Latin2
uint8_t to8859_2(uint16_t utf8)
{
	int i;
	if (utf8 > 0x1ff)
		return 0x1f;
	for (i = 0; i < UTF8TO8859_2; i++)
	{
		if (utf8 == utf8To8859_2[i].utf8)
		{
			return utf8To8859_2[i].ascii;
		}
	}
	return to8859(utf8);
}
////////////////////////////////////////
uint16_t UtoC(uint8_t high, uint8_t low)
{
	uint16_t res = (((high & 0x01f) << 6) | (low & 0x3F)) & 0x7FF;
	ESP_LOGV(TAG, "UtoC: HL: %02x%02x,  res:0x%04x", high, low, res);
	return (res);
}

void removeUtf8(char *characters)
{
	int Rindex = 0;
	uint16_t utf8;
	ESP_LOGV(TAG, "removeUtf8 in : %s", characters);
	ucEraseSlashes(characters);
	while (characters[Rindex])
	{
		if ((characters[Rindex] >= 0xc2) && (characters[Rindex] <= 0xc5)) // only 0 to FF ascii char
		{
			utf8 = UtoC(characters[Rindex], characters[Rindex + 1]); // the utf8
			if (utf8 >= 0x100)
#if LATIN == 2
				characters[Rindex + 1] = to8859_2(utf8); //  (uint8_t)utf8 &0xff;
#elif LATIN == 1
				characters[Rindex + 1] = to8859(utf8); //  (uint8_t)utf8 &0xff;
#endif
			else
				characters[Rindex + 1] = (uint8_t)utf8 & 0xff;
			//		if (utf8>= 0x100) characters[Rindex+1] = 0x1f; //Erase to non-printable symbol
			int sind = Rindex + 1;
			while (characters[sind])
			{
				characters[sind - 1] = characters[sind];
				sind++;
			} // pad left
			characters[sind - 1] = 0;
		}
		else if ((characters[Rindex] >= 0xd0) && (characters[Rindex] <= 0xd3)) // only 0 to FF ascii char
		{
			utf8 = UtoC(characters[Rindex], characters[Rindex + 1]); // the utf8
			characters[Rindex + 1] = to1251(utf8);
			int sind = Rindex + 1;
			while (characters[sind])
			{
				characters[sind - 1] = characters[sind];
				sind++;
			}
			characters[sind - 1] = 0;
			charset = Cyrillic;
		}
		else if ((characters[Rindex] >= 0xcd) && (characters[Rindex] <= 0xcf)) // only 0 to FF ascii char
		{
			utf8 = UtoC(characters[Rindex], characters[Rindex + 1]); // the utf8
			characters[Rindex + 1] = to1253(utf8);
			int sind = Rindex + 1;
			while (characters[sind])
			{
				characters[sind - 1] = characters[sind];
				sind++;
			}
			characters[sind - 1] = 0;
			charset = Greek;
		}
		Rindex++;
	}

	ESP_LOGV(TAG, "removeUtf8 out: %s", characters);
}

// Mark the lines to draw
void markDrawResetUcg(int i)
{
	mline[i] = 1;
	iline[i] = 0;
	tline[i] = 0;
}
////////////////////////////////////////
// scroll each line
void scrollUcg()
{
	int16_t len;
	setfont(text);
	for (int i = 0; i < LINES; i++)
	{
		if (lline[i] != NULL)
		{
			if (tline[i] > 0)
			{
				len = (i == 0) ? ucg_GetStrWidth(&ucg, nameNum) + ucg_GetStrWidth(&ucg, lline[i]) : ucg_GetStrWidth(&ucg, lline[i]);
				if ((tline[i] == 4) && (len >= x))
				{
					iline[i] = 0;
					mline[i] = 1; // draw(i);
				}
				tline[i]--;
			}
			else
			{
				len = (i == 0) ? ucg_GetStrWidth(&ucg, nameNum) + ucg_GetStrWidth(&ucg, lline[i] + iline[i]) : ucg_GetStrWidth(&ucg, lline[i] + iline[i]);
				if (len > x)
				{
					len = iline[i];
					iline[i] += x / ucg_GetStrWidth(&ucg, "MM"); // x/6;
					while ((*(lline[i] + iline[i]) != ' ') && (*(lline[i] + iline[i]) != '-') && (iline[i] != 0))
						iline[i]--;
					if (iline[i] == len)
						iline[i] += (x / ucg_GetStrWidth(&ucg, "MM")); // x/6;
					mline[i] = 1;									   // draw(i);
				}
				else
				{
					tline[i] = 6;
				}
			}
		}
	}
}

//////////////////////////
// set color of font per line
void setColor(int i)
{
	switch (i)
	{
	case STATIONNAME:
		ucg_SetColor(&ucg, 0, CBLACK);
		break;
	case STATION1:
		ucg_SetColor(&ucg, 0, CWHITE);
		break;
	case STATION2:
		ucg_SetColor(&ucg, 0, CSTATION2);
		break;
	case TITLE1:
	case TITLE11:
		ucg_SetColor(&ucg, 0, CTITLE1);
		break;
	case TITLE2:
	case TITLE21:
		ucg_SetColor(&ucg, 0, CTITLE2);
		break;
	case VOLUME:
		ucg_SetColor(&ucg, 0, CVOLUME_TEXT);
		break;
	default:
		ucg_SetColor(&ucg, 0, CBODY);
	}
}

////////////////////
// draw one line
void draw(int i)
{
	uint16_t len, xpos, yyy, shift;

	if (mline[i])
		mline[i] = 0;
	if (i >= 3)
		z = (y / 2) + 1;
	else
		z = 0;
	switch (i)
	{
	case STATIONNAME:
		if (btHeaderLayout())
			break;
		shift = btBodyShift();
		setfont(text);
		if (radioMainHeaderLayout())
			ucg_SetColor(&ucg, 0, CBLACK);
		else
			ucg_SetColor(&ucg, 0, CWHITE);
		ucg_DrawBox(&ucg, 0, shift, x, y /*-ucg_GetFontDescent(&ucg)*/);
		if (radioMainHeaderLayout())
			ucg_SetColor(&ucg, 0, CBODY);
		else
			ucg_SetColor(&ucg, 0, CBLACK);
		if (lline[i] != NULL)
		{
			if (nameNum[0] == 0)
				ucg_DrawString(&ucg, 1, shift + 1, 0, lline[i] + iline[i]);
			else
			{
				ucg_DrawString(&ucg, 1, shift + 1, 0, nameNum);
				ucg_DrawString(&ucg, ucg_GetStrWidth(&ucg, nameNum) - 2, shift + 1, 0, lline[i] + iline[i]);
			}
		}
		break;
	case VOLUME:
		if ((yy > 80) || (lline[TITLE21] == NULL) || (strlen(lline[TITLE21]) == 0))
		{
			ucg_SetColor(&ucg, 0, CVOLUME_FRAME);
			if (yy <= 80)
			{
				ucg_DrawFrame(&ucg, 0, yy - 10, x / 3, 8);
				ucg_SetColor(&ucg, 0, CVOLUME_FILL);
				ucg_DrawBox(&ucg, 1, yy - 9, ((uint16_t)(x / 3 * volume) / 23), 6);
			}
			else
			{
				uint16_t bar_width = radioMainHeaderLayout() ? x : x / 2;
				ucg_DrawFrame(&ucg, 0, yy - 10, bar_width, 8);
				ucg_SetColor(&ucg, 0, CVOLUME_FILL);
				ucg_DrawBox(&ucg, 1, yy - 9, ((uint32_t)bar_width * volume / 23), 6);
			}
		}
		break;
	case TIME:
		if (radioMainHeaderLayout())
			break;
		if ((yy > 80) || (lline[TITLE21] == NULL) || (strlen(lline[TITLE21]) == 0))
		{
			setfont(small);
			char strsec[30];
			if (getDdmm())
				sprintf(strsec, "%02d-%02d  %02d:%02d:%02d", dt->tm_mday, dt->tm_mon + 1, dt->tm_hour, dt->tm_min, dt->tm_sec);
			else
				sprintf(strsec, "%02d-%02d  %02d:%02d:%02d", dt->tm_mon + 1, dt->tm_mday, dt->tm_hour, dt->tm_min, dt->tm_sec);
			len = ucg_GetStrWidth(&ucg, strsec);
			ucg_SetColor(&ucg, 0, CTIME_TEXT);
			ucg_SetColor(&ucg, 1, CBLACK);
			ucg_SetFontMode(&ucg, UCG_FONT_MODE_SOLID);
			if (yy <= 80)
			{
				xpos = (5 * x / 8) - (len / 2);
				yyy = yy - 10;
				ucg_DrawString(&ucg, xpos, yyy, 0, strsec);
			}
			else
			{
				xpos = (3 * x / 4) - (len / 2);
				yyy = yy - 10;
				ucg_DrawString(&ucg, xpos, yyy, 0, strsec);
			}
			ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);
		}
		break;
	default:
		ucg_SetColor(&ucg, 0, CBLACK);
		shift = btBodyShift();
		if (yy <= 80)
			ucg_DrawBox(&ucg, 0, y * i + z + shift, x, y + 1);
		else
			ucg_DrawBox(&ucg, 0, y * i + z - 1 + shift, x, y + 1);
		setColor(i);
		if (lline[i] != NULL)
			ucg_DrawString(&ucg, 0, y * i + z + shift, 0, lline[i] + iline[i]);
	}
}

////////////////////////////////////////
// draw the full screen
void drawLinesUcg()
{
	setfont(text);
	for (int i = 0; i < LINES; i++)
	{
		//		taskYIELD();
		if (mline[i])
			draw(i);
	}
}

////////////////////////////////////////
// draw all
void drawFrameUcg(uint8_t mTscreen)
{
	// printf("drawFrameUcg, mTscreen: %d\n",mTscreen);
	int i;
	if (dt == NULL)
	{
		dt = getDt();
	}
	switch (mTscreen)
	{
	case MTNEW:
		ucg_ClearScreen(&ucg);
		TTitleStr[0] = 0;
		if (radioMainHeaderLayout())
			streamInfoDrawn = false;
		if (btHeaderLayout())
			drawBtHeaderUcg();
		else if (radioMainHeaderLayout())
			drawRadioMainHeaderUcg();
		setfont(text);
		ucg_SetColor(&ucg, 0, CTITLE1);
		ucg_SetColor(&ucg, 1, CTITLE2);
		ucg_DrawGradientLine(&ucg, 0, (4 * y) - (y / 2) - 6 + btBodyShift(), x, 0);
		if (!btHeaderLayout() && !radioMainHeaderLayout())
		{
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, 0, 0, x - 1, 15);
		}
		for (i = 0; i < LINES; i++)
			draw(i);
		// fall through
	case MTREFRESH:
		if (transition_body_cleared)
		{
			setfont(text);
			ucg_SetColor(&ucg, 0, CTITLE1);
			ucg_SetColor(&ucg, 1, CTITLE2);
			ucg_DrawGradientLine(&ucg, 0, (4 * y) - (y / 2) - 6 + btBodyShift(), x, 0);
			for (i = 0; i < LINES; i++)
				markDrawResetUcg(i);
		}
		else
			markDrawResetUcg(TIME);
		if (btHeaderLayout())
			drawBtHeaderUcg();
		else if (radioMainHeaderLayout())
			drawRadioMainHeaderUcg();
		drawLinesUcg();
		break;
	default:;
	}

	// screenBottomUcg();
}

//////////////////////////
void drawTTitleUcg(char *ttitle)
{

	if (strcmp(ttitle, TTitleStr) != 0)
	{
		setfont(middle);
		uint16_t xxx = (x / 2) - (ucg_GetStrWidth(&ucg, ttitle) / 2);
		ucg_SetColor(&ucg, 0, CTBACK);
		ucg_DrawBox(&ucg, 0, 0, x, HHeader);
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_DrawString(&ucg, xxx, (HHeader - ucg_GetFontAscent(&ucg)) >> 1, 0, ttitle);
		strcpy(TTitleStr, ttitle);
	}
}
//////////////////////////
void drawNumberUcg(uint8_t mTscreen, char *irStr)
{
	uint16_t xxx;
	char ststr[] = {"Number"};
	switch (mTscreen)
	{
	case 1:
		TTitleStr[0] = 0;
		drawTTitleUcg(ststr);
	// fall through
	case 2:
		xxx = (x / 2) - (ucg_GetStrWidth(&ucg, irStr) / 2);
		if (!transition_body_cleared)
		{
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, 0, HHeader, x, yy - HHeader);
		}
		setfont(large);
		ucg_SetColor(&ucg, 0, CBODY);
		ucg_DrawString(&ucg, xxx, yy / 3, 0, irStr);
		break;
	default:;
	}

	//  screenBottomUcg();
}
//////////////////////////
void drawStationUcg(uint8_t mTscreen, char *snum, char *ddot)
{

	char ststr[] = {"Station"};
	int16_t len;
	LANG scharset;
	scharset = charset;
	charset = Latin;
	switch (mTscreen)
	{
	case 1:
		TTitleStr[0] = 0;
		drawTTitleUcg(ststr);
	// fall through
	case 2:
		if (!transition_body_cleared)
		{
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, 0, HHeader, x, yy);
		}
		//       setfont(middle);
		ucg_SetColor(&ucg, 0, CBODY);
		//        ddot = strstr(sline,":");
		if (ddot != NULL)
		{
			removeUtf8(ddot);
			setfont(middle);
			ucg_DrawString(&ucg, (x / 2) - (ucg_GetStrWidth(&ucg, snum) / 2), yy / 3, 0, snum);
			len = (x / 2) - (ucg_GetStrWidth(&ucg, ddot) / 2);
			if (len < 0)
				len = 0;
			ucg_DrawString(&ucg, len, yy / 3 + ucg_GetFontAscent(&ucg) + y, 0, ddot);
		}
		break;
	default:;
	}
	charset = scharset;

	//  screenBottomUcg();
}

// Draw OTA progress for color displays
void drawProgressUcg(uint8_t percent)
{
	// Draw static label and update only incremental progress area (minimize redraw)
	const char *label = "OTA upload in progress";
	ucg_SetFont(&ucg, ucg_font_inr16_mf);
	int len = ucg_GetStrWidth(&ucg, label);
	int xxx = (x / 2) - (len / 2);
	// draw label background and label (static)
	// ucg_SetColor(&ucg, 0, CBLACK);
	// ucg_DrawBox(&ucg, xxx - 4, yy / 3 - (ucg_GetFontAscent(&ucg) + 2), len + 8, ucg_GetFontAscent(&ucg) + 4);
	ucg_SetColor(&ucg, 0, CBODY);
	ucg_DrawString(&ucg, xxx, yy / 3, 0, label);

	int bw = x - 40;
	int bh = 14;
	int bx = 20;
	int by = yy / 2;

	static int last_fill_px = -1;
	int fillw = (bw * percent) / 100;
	if (percent == 0 ) //|| last_fill_px < 0
	{
		// initial draw: frame + clear inner
		ucg_SetColor(&ucg, 0, CPROGRESS_FRAME);
		ucg_DrawFrame(&ucg, bx, by, bw, bh);
		ucg_SetColor(&ucg, 0, CBLACK);
		ucg_DrawBox(&ucg, bx + 1, by + 1, bw - 2, bh - 2);
		last_fill_px = 0;
	}

	if (fillw > last_fill_px)
	{
		// draw only new segment (with small overlap)
		int add = fillw - last_fill_px;
		int draw_x = bx + 1 + last_fill_px;
		int draw_w = add + 2;
		if (draw_w + last_fill_px > bw - 2)
			draw_w = (bw - 2) - last_fill_px;
		if (draw_w > 0)
		{
			ucg_SetColor(&ucg, 0, CPROGRESS_FILL);
			ucg_DrawBox(&ucg, draw_x, by + 1, draw_w, bh - 2);
		}
		last_fill_px = fillw;
	}
	else if (fillw < last_fill_px)
	{
		// fallback: redraw inner area and current fill
		ucg_SetColor(&ucg, 0, CBLACK);
		ucg_DrawBox(&ucg, bx + 1, by + 1, bw - 2, bh - 2);
		if (fillw > 0)
		{
			ucg_SetColor(&ucg, 0, CPROGRESS_FILL);
			ucg_DrawBox(&ucg, bx + 1, by + 1, fillw - 1, bh - 2);
		}
		last_fill_px = fillw;
	}
}

void drawVolumeUcg(uint8_t mTscreen)
{
	char vlstr[] = {"Volume"};
	//  volume = atoi(aVolume);
	char aVolume[6];
	sprintf(aVolume, "%d", volume);
	switch (mTscreen)
	{
	case 1:
		if (!transition_body_cleared)
		{
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, 0, HHeader, x, yy);
		}
		drawTTitleUcg(vlstr);
		// fall through
	case 2:
		setfont(large);
		uint16_t xxx;
		xxx = (x / 2) - (ucg_GetStrWidth(&ucg, aVolume) / 2);
		if (!transition_body_cleared)
		{
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, 0, yy / 3, x, ucg_GetFontAscent(&ucg) + 2);
		}
		ucg_SetColor(&ucg, 0, CBODY);
		ucg_DrawString(&ucg, xxx, yy / 3, 0, aVolume);
		break;
	default:;
	}
}

static void drawInfo(unsigned timein)
{
	static unsigned inInfo;
	uint16_t len;
	if (inInfo != timein || transition_body_cleared)
	{
		char strinf[12];
		LANG scharset;
		scharset = charset;
		charset = Latin;
		setfont(text);
		ucg_SetFontMode(&ucg, UCG_FONT_MODE_SOLID);

		sprintf(strinf, ":%02d", dt->tm_sec);
		len = ucg_GetStrWidth(&ucg, "000");
		ucg_SetColor(&ucg, 1, CBLACK);
		ucg_SetColor(&ucg, 0, CBODY);
		ucg_DrawString(&ucg, x - len - 8, yy - (2 * y), 0, strinf);

		// RSSI
		sprintf(strinf, "%02d dBm", get_rssi());
		ucg_SetColor(&ucg, 1, CBLACK);
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_DrawString(&ucg, 4, yy - y, 0, strinf);

		// Battery
		int batPercent = getBatPercent();
		if (batPercent != -1)
		{
			sprintf(strinf, "Batt: %3d%%", batPercent);
			len = ucg_GetStrWidth(&ucg, strinf);
			ucg_SetColor(&ucg, 1, CBLACK);
			ucg_SetColor(&ucg, 0, CBATTERY_COLOR(batPercent));
			ucg_DrawString(&ucg, x - len - 8, yy - y, 0, strinf);
		}

		ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);
		inInfo = timein; // To avoid redisplay
		charset = scharset;
	}
}

static void drawTimeHeaderUcg(const char *date, unsigned timein)
{
	const uint16_t side_area_width = 80;
	const uint16_t sample_area_width = 68;
	const uint16_t badge_width = 52;
	bool date_changed = strcmp(date, TTitleStr) != 0;
	uint16_t date_width;
	uint16_t date_x;
	bool show_stream_info = x >= side_area_width * 3;

	setfont(middle);
	date_width = ucg_GetStrWidth(&ucg, (char *)date);
	date_x = (x > date_width) ? (x - date_width) / 2 : 0;
	if (date_changed)
	{
		ucg_SetColor(&ucg, 0, CTBACK);
		ucg_DrawBox(&ucg, 0, 0, x, HHeader);
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_DrawString(&ucg, date_x, (HHeader - ucg_GetFontAscent(&ucg)) >> 1, 0, (char *)date);
		strncpy(TTitleStr, date, sizeof(TTitleStr) - 1);
		TTitleStr[sizeof(TTitleStr) - 1] = 0;
		streamInfoDrawn = false;
	}

	if (!show_stream_info)
		return;

	audio_stream_info_t info;
	audio_player_get_stream_info(&info);
	bool codec_changed = !streamInfoDrawn || info.codec != lastStreamInfo.codec;
	bool sample_rate_changed = !streamInfoDrawn || info.sample_rate != lastStreamInfo.sample_rate;
	bool vbr_changed = !streamInfoDrawn || info.variable_bitrate != lastStreamInfo.variable_bitrate;
	bool bitrate_changed = !info.variable_bitrate &&
		(info.bitrate + 500) / 1000 != (lastStreamInfo.bitrate + 500) / 1000;
	if (!codec_changed && !sample_rate_changed && !vbr_changed && !bitrate_changed)
		return;
	if (!codec_changed && !sample_rate_changed && !vbr_changed && bitrate_changed &&
		(unsigned)(timein - lastStreamInfoDrawTime) < 2)
		return;

	uint16_t badge_x = x - badge_width - 4;
	char codec_glyph[2] = "-";
	char bitrate_text[16] = "--k";
	char sample_rate_text[16] = "--.-k";
	if (info.codec == AUDIO_STREAM_CODEC_AAC)
		codec_glyph[0] = 'A';
	else if (info.codec == AUDIO_STREAM_CODEC_MP3)
		codec_glyph[0] = 'M';
	else if (info.codec == AUDIO_STREAM_CODEC_SBC)
		codec_glyph[0] = 'S';
	else if (info.codec == AUDIO_STREAM_CODEC_LDAC)
		codec_glyph[0] = 'L';
	else if (info.codec == AUDIO_STREAM_CODEC_APTX)
		codec_glyph[0] = 'X';
	if (info.variable_bitrate)
		strcpy(bitrate_text, "VBR");
	else if (info.bitrate != 0)
		snprintf(bitrate_text, sizeof(bitrate_text), "%luk", (unsigned long)((info.bitrate + 500) / 1000));
	if (info.sample_rate != 0)
		snprintf(sample_rate_text, sizeof(sample_rate_text), "%u.%uk",
			 (unsigned)(info.sample_rate / 1000), (unsigned)((info.sample_rate % 1000) / 100));

	ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);
	ucg_SetColor(&ucg, 0, CTBACK);
	ucg_DrawBox(&ucg, 0, 0, sample_area_width, HHeader);
	ucg_DrawBox(&ucg, badge_x, 0, badge_width + 4, HHeader);
	if (info.codec != AUDIO_STREAM_CODEC_NONE)
	{
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_SetFont(&ucg, ucg_font_stream);
		uint16_t sample_rate_width = ucg_GetStrWidth(&ucg, sample_rate_text);
		uint16_t sample_y = (HHeader - ucg_GetFontAscent(&ucg)) / 2;
		if (sample_rate_width > sample_area_width - 6)
		{
			setfont(small);
			sample_rate_width = ucg_GetStrWidth(&ucg, sample_rate_text);
			sample_y = (HHeader - ucg_GetFontAscent(&ucg)) / 2;
		}
		ucg_DrawString(&ucg, (sample_area_width - sample_rate_width) / 2, sample_y, 0, sample_rate_text);

		ucg_SetFont(&ucg, ucg_font_stream);
		uint16_t line_height = ucg_GetFontAscent(&ucg) - ucg_GetFontDescent(&ucg);
		uint16_t codec_width = ucg_GetStrWidth(&ucg, codec_glyph);
		uint16_t bitrate_width;
		// if (info.variable_bitrate)
		// {
		// 	setfont(small);
		// 	bitrate_width = ucg_GetStrWidth(&ucg, bitrate_text);
		// }
		// else
		// {
			ucg_SetFont(&ucg, ucg_font_stream);
			bitrate_width = ucg_GetStrWidth(&ucg, bitrate_text);
		// }
		uint16_t codec_y = (HHeader - 2 * line_height) / 2;
		uint16_t bitrate_y = codec_y + line_height;
		uint16_t badge_height = (HHeader < 48) ? HHeader - 4 : 44;
		ucg_DrawRFrame(&ucg, badge_x + 2, (HHeader - badge_height) / 2, badge_width - 4, badge_height, 4);
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_SetFont(&ucg, ucg_font_stream);
		ucg_DrawString(&ucg, badge_x + (badge_width - codec_width) / 2, codec_y, 0, codec_glyph);
		// if (info.variable_bitrate)
		// 	setfont(small);
		// else
		ucg_SetFont(&ucg, ucg_font_stream);
		ucg_DrawString(&ucg, badge_x + (badge_width - bitrate_width) / 2, bitrate_y, 0, bitrate_text);
	}
	lastStreamInfo = info;
	streamInfoDrawn = true;
	lastStreamInfoDrawTime = timein;
}

static void drawBtHeaderUcg(void)
{
	char title[BT_HEADER_NAME_LEN + 1];
	btHeaderTitle(title, sizeof(title));
	drawTimeHeaderUcg(title, (unsigned)(esp_timer_get_time() / 1000000));
}

static void drawRadioMainHeaderUcg(void)
{
	char time_text[6];
	snprintf(time_text, sizeof(time_text), "%02d:%02d", dt->tm_hour, dt->tm_min);
	if (strcmp(time_text, TTitleStr) != 0 || transition_body_cleared)
	{
		const uint16_t sample_area_width = 68;
		const uint16_t badge_width = 52;
		uint16_t badge_x = x - badge_width - 4;
		uint16_t time_area_width = badge_x - sample_area_width;
		setfont(middle);
		uint16_t time_width = ucg_GetStrWidth(&ucg, time_text);
		ucg_SetColor(&ucg, 0, CTBACK);
		ucg_DrawBox(&ucg, sample_area_width, 0, time_area_width, HHeader);
		ucg_SetColor(&ucg, 0, CTTFONT);
		ucg_DrawString(&ucg, sample_area_width + (time_area_width - time_width) / 2,
				(HHeader - ucg_GetFontAscent(&ucg)) / 2, 0, time_text);
		strcpy(TTitleStr, time_text);
	}
	drawTimeHeaderUcg(time_text, (unsigned)(esp_timer_get_time() / 1000000));
}

void prepareScreenTransitionUcg(const char *title, uint8_t header_kind, unsigned timein)
{
	TTitleStr[0] = 0;
	if (header_kind == UCG_HEADER_BT)
	{
		drawBtHeaderUcg();
	}
	else if (header_kind == UCG_HEADER_DATE)
	{
		TTimeStr[0] = 0;
		drawTimeHeaderUcg(title, timein);
	}
	else if (header_kind == UCG_HEADER_MAIN && radioMainHeaderLayout())
	{
		streamInfoDrawn = false;
		drawRadioMainHeaderUcg();
	}
	else
	{
		drawTTitleUcg((char *)title);
	}

	ucg_SetColor(&ucg, 0, CBLACK);
	ucg_DrawBox(&ucg, 0, HHeader, x, yy - HHeader);
	transition_body_cleared = true;
}

void finishScreenTransitionUcg(void)
{
	transition_body_cleared = false;
}

void drawTimeUcg(uint8_t mTscreen, unsigned timein)
{
	char strdate[46];
	char strtime[40];
	LANG scharset;

	sprintf(strtime, "%02d:%02d", dt->tm_hour, dt->tm_min);
	switch (mTscreen)
	{
	case 1:
		scharset = charset;
		charset = Latin;
		setfont(text);
		sprintf(strdate, "IP: %s", getIp());
		ucg_ClearScreen(&ucg);
		ucg_SetColor(&ucg, 0, CSTATUS);
		TTitleStr[0] = 0;
		TTimeStr[0] = 0;
		ucg_DrawString(&ucg, 4, yy - (2 * y), 0, strdate);
		charset = scharset;
		// fall through
	case 2:
		if (getDdmm())
			sprintf(strdate, "%02d-%02d-%04d", dt->tm_mday, dt->tm_mon + 1, dt->tm_year + 1900);
		else
			sprintf(strdate, "%02d-%02d-%04d", dt->tm_mon + 1, dt->tm_mday, dt->tm_year + 1900);
		if (transition_body_cleared)
		{
			char ip_text[46];
			setfont(text);
			sprintf(ip_text, "IP: %s", getIp());
			ucg_SetColor(&ucg, 0, CSTATUS);
			ucg_DrawString(&ucg, 4, yy - (2 * y), 0, ip_text);
		}
		drawTimeHeaderUcg(strdate, timein);
		if (strcmp(TTimeStr, strtime) != 0)
		{
			setfont(large);
			uint16_t time_width = ucg_GetStrWidth(&ucg, strtime);
			ucg_SetColor(&ucg, 0, CBLACK);
			ucg_DrawBox(&ucg, (x - time_width) / 2, yy / 3, time_width,
					ucg_GetFontAscent(&ucg) - ucg_GetFontDescent(&ucg) + 1);
			ucg_SetColor(&ucg, 0, CBODY);
			ucg_SetColor(&ucg, 1, CBLACK);
			ucg_SetFontMode(&ucg, UCG_FONT_MODE_SOLID);
			ucg_DrawString(&ucg, (x / 2) - (ucg_GetStrWidth(&ucg, strtime) / 2), yy / 3, 0, strtime);
			strcpy(TTimeStr, strtime);
			ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);
		}

		break;
	default:;
	}

	drawInfo(timein);
}

////////////////////////////////////////
void separatorUcg(char *from)
{
	char *interp;
	//    len = strlen(from);
	// ucg_SetFont(&ucg,ucg_font_6x13_tf);
	setfont(text);
	while (from[strlen(from) - 1] == ' ')
		from[strlen(from) - 1] = 0; // avoid blank at end
	while ((from[0] == ' '))
	{
		strcpy(from, from + 1);
	}
	interp = strstr(from, " - ");
	if (from == nameset)
	{ /*lline[0] = nameset;*/
		lline[1] = NULL;
		lline[2] = NULL;
		return;
	}
	if (interp != NULL)
	{
		from[interp - from] = 0;
		lline[(from == station) ? STATION1 : TITLE1] = from;
		lline[(from == station) ? STATION2 : TITLE2] = interp + 3;
		mline[(from == station) ? STATION1 : TITLE1] = 1;
		mline[(from == station) ? STATION2 : TITLE2] = 1;
	}
	else
	{
		lline[(from == station) ? STATION1 : TITLE1] = from;
		mline[(from == station) ? STATION1 : TITLE1] = 1;
	}

	// 2 lines for Title
	if ((lline[TITLE1] != NULL) && (from == title) && (ucg_GetStrWidth(&ucg, lline[TITLE1]) > x))
	{
		int idx = strlen(lline[TITLE1]);
		*(lline[TITLE1] + idx) = ' ';
		*(lline[TITLE1] + idx + 1) = 0;
		while ((ucg_GetStrWidth(&ucg, lline[TITLE1]) > x) && (idx != 0))
		{
			*(lline[TITLE1] + idx--) = ' ';
			while ((*(lline[TITLE1] + idx) != ' ') && (idx != 0))
				idx--;
			if (idx != 0)
				*(lline[TITLE1] + idx) = 0;
		}
		lline[TITLE11] = lline[TITLE1] + idx + 1;
		mline[TITLE11] = 1;
	}

	if ((lline[TITLE2] != NULL) && (from == title) && (ucg_GetStrWidth(&ucg, lline[TITLE2]) > x))
	{
		int idx = strlen(lline[TITLE2]);
		*(lline[TITLE2] + idx) = ' ';
		*(lline[TITLE2] + idx + 1) = 0;
		while ((ucg_GetStrWidth(&ucg, lline[TITLE2]) > x) && (idx != 0))
		{
			*(lline[TITLE2] + idx--) = ' ';
			while ((*(lline[TITLE2] + idx) != ' ') && (idx != 0))
				idx--;
			if (idx != 0)
				*(lline[TITLE2] + idx) = 0;
		}
		lline[TITLE21] = lline[TITLE2] + idx + 1;
		mline[TITLE21] = 1;
	}
}

// cli.meta
void metaUcg(char *ici)
{
	cleartitleUcg(TITLE1);
	cleartitleUcg(TITLE11);
	cleartitleUcg(TITLE2);
	cleartitleUcg(TITLE21);
	strcpy(title, ici + 7);
	removeUtf8(title);
	separatorUcg(title);
}

// cli.icy4
void icy4Ucg(char *ici)
{
	char newstation[BUFLEN];
	// move the STATION2 to STATION1S
	if ((*station != 0) && (lline[STATION2] != NULL))
	{
		strcpy(newstation, lline[STATION1]);
		strcat(newstation, " - ");
		strcat(newstation, lline[STATION2]);
		strcpy(lline[STATION1], newstation);
		markDrawResetUcg(STATION1);
	}

	strcpy(genre, ici + 7);
	removeUtf8(genre);
	lline[GENRE] = genre;
	markDrawResetUcg(GENRE);
}
// cli.icy0
void icy0Ucg(char *ici)
{
	clearAllUcg();
	if (strlen(ici + 7) == 0)
		strcpy(station, nameset);
	else
		strcpy(station, ici + 7);
	removeUtf8(station);
	separatorUcg(station);
}

// cli.stopped or label
void statusUcg(const char *label)
{
	cleartitleUcg(TITLE1);
	cleartitleUcg(TITLE11);
	cleartitleUcg(TITLE2);
	cleartitleUcg(TITLE21);
	strcpy(title, label);
	lline[TITLE1] = title;
}
// cli.nameset
void namesetUcg(char *ici)
{
	strcpy(nameset, ici + 8);
	ici = strstr(nameset, " ");
	if (ici != NULL)
	{
		clearAllUcg();
		strncpy(nameNum, nameset, ici - nameset + 1);
		nameNum[ici - nameset + 1] = 0;
		setFuturNum(atoi(nameNum));
	}
	char nameseti[BUFLEN];
	strcpy(nameseti, nameset + strlen(nameNum));
	strcpy(nameset, nameseti);
	charset = Latin;
	removeUtf8(nameset);
	lline[STATIONNAME] = nameset;
	markDrawResetUcg(STATIONNAME);
}

// cli.playing
void playingUcg()
{
	if (strcmp(title, "STOPPED") == 0)
	{
		cleartitleUcg(TITLE1);
		cleartitleUcg(TITLE11);
		cleartitleUcg(TITLE2);
		cleartitleUcg(TITLE21);
		separatorUcg(title);
	}
}

void lcd_initUcg(uint8_t *lcd_type)
{

	gpio_num_t miso;
	gpio_num_t mosi;
	gpio_num_t sclk;
	uint8_t spi_no;

	gpio_num_t scl;
	gpio_num_t sda;
	gpio_num_t rsti2c;

	gpio_num_t cs;
	gpio_num_t a0;
	gpio_num_t rstlcd;
	//	gpio_num_t t_cs;
	//	gpio_num_t t_irq;
	dt = getDt();
	uint8_t rotat = getRotat();
	ESP_LOGI(TAG, "lcd init  type: %u, Rotat: %u", *lcd_type, rotat);
	if (*lcd_type == LCD_NONE)
		return;

	ucg_esp32_hal_t ucg_esp32_hal = UCG_ESP32_HAL_DEFAULT;
	if (*lcd_type & LCD_SPI) // Color SPI
	{
		gpio_get_spi_bus(&spi_no, &miso, &mosi, &sclk);
		gpio_get_spi_lcd(&cs, &a0, &rstlcd);
		ucg_esp32_hal.spi_no = spi_no;
		ucg_esp32_hal.clk = sclk;
		ucg_esp32_hal.mosi = mosi;
		ucg_esp32_hal.cs = cs;
		ucg_esp32_hal.dc = a0;
		if (rstlcd != GPIO_NONE)
			ucg_esp32_hal.reset = rstlcd;
	}
	else // Color I2c (never seen this one)
	{
		gpio_get_i2c(&scl, &sda, &rsti2c);
		ucg_esp32_hal.sda = sda;
		ucg_esp32_hal.scl = scl;
		ucg_esp32_hal.reset = rsti2c;
	}

	ucg_esp32_hal_init(ucg_esp32_hal);

	switch (*lcd_type)
	{
		// Color spi
	case LCD_SPI_SSD1351:
		ucg_Init(&ucg, ucg_dev_ssd1351_18x128x128_ilsoft, ucg_ext_ssd1351_18, ucg_com_hal);
		break;
	case LCD_SPI_ST7735:
		ucg_Init(&ucg, ucg_dev_st7735_18x128x160, ucg_ext_st7735_18, ucg_com_hal);
		break;
	case LCD_SPI_ST7735S:
		ucg_Init(&ucg, ucg_dev_st7735_18x128x128, ucg_ext_st7735S_18, ucg_com_hal);
		break;
	case LCD_SPI_ST7735L:
		ucg_Init(&ucg, ucg_dev_st7735_18x80x160, ucg_ext_st7735L_18, ucg_com_hal);
		break;
	case LCD_SPI_ST7735W:
		ucg_Init(&ucg, ucg_dev_st7735_18x128x160_W, ucg_ext_st7735W_18, ucg_com_hal);
		break;
	case LCD_SPI_ILI9341:
		ucg_Init(&ucg, ucg_dev_ili9341_18x240x320, ucg_ext_ili9341_18, ucg_com_hal);
		break;
	case LCD_SPI_ILI9163:
		ucg_Init(&ucg, ucg_dev_ili9163_18x128x128, ucg_ext_ili9163_18, ucg_com_hal);
		break;
	case LCD_SPI_PCF8833:
		ucg_Init(&ucg, ucg_dev_pcf8833_16x132x132, ucg_ext_pcf8833_16, ucg_com_hal);
		break;
	case LCD_SPI_SSD1331:
		ucg_Init(&ucg, ucg_dev_ssd1331_18x96x64_univision, ucg_ext_ssd1331_18, ucg_com_hal);
		break;
	case LCD_SPI_SEPS225:
		ucg_Init(&ucg, ucg_dev_seps225_16x128x128_univision, ucg_ext_seps225_16, ucg_com_hal);
		break;
	case LCD_SPI_ST7789V:
		ucg_Init(&ucg, ucg_dev_st7789_18x240x320, ucg_ext_st7789_18, ucg_com_hal);
		break;
	case LCD_SPI_ST7789S:
		ucg_Init(&ucg, ucg_dev_st7789_18x240x240, ucg_ext_st7789_24, ucg_com_hal);
		break;
	case LCD_SPI_ST7789T:
		ucg_Init(&ucg, ucg_dev_st7789_13x135x240, ucg_ext_st7789_24, ucg_com_hal);
		break;
	case LCD_SPI_ST7789V3:
		ucg_Init(&ucg, ucg_dev_st7789_18x170x320, ucg_ext_st7789_18, ucg_com_hal);
		break;

	default:
		ESP_LOGE(TAG, "lcd invalid type: %d, Fall back to LCD_NONE", *lcd_type);
		*lcd_type = LCD_NONE;
		return;
	}

	ESP_LOGI(TAG, "lcd init Color type: %u", *lcd_type);
	// define prefered font rendering method (no text will be visibile, if this is missing
	ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);
	ucg_ClearScreen(&ucg);

	if (rotat)
		ucg_SetRotate270(&ucg);
	else
		ucg_SetRotate90(&ucg);

	ucg_SetFontPosTop(&ucg);
	x = ucg_GetWidth(&ucg);

	setfont(text);
	yy = ucg_GetHeight(&ucg);
#ifndef CONFIG_NO_XPT2046
	// set size of screen on touch driver
	setTouchSize(x, yy);
#endif

	if (yy <= 80)
		y = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg) + 2; // interline
	else
		y = -ucg_GetFontDescent(&ucg) + ucg_GetFontAscent(&ucg) + 3; // interline

	HHeader = yy / 5;

	ESP_LOGI(TAG, "X: %u, yy: %u, y: %u", x, yy, y);
	z = 0;
}

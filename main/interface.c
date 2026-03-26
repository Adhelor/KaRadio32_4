/******************************************************************************
 *
 * Copyright 2018 karawin (http://www.karawin.fr)
 *
 *******************************************************************************/

// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#define TAG "Interface"
#include "interface.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "eeprom.h"
#include "ntp.h"
#include "webclient.h"
#include "webserver.h"
#ifndef CONFIG_NO_VS1053
#include "vs1053.h"
#endif
#include "gpio.h"
#include "ota.h"
#include "spiram_fifo.h"
#include "addon.h"
#if defined(CONFIG_DISPLAY_TYPE_ALL) || defined(CONFIG_DISPLAY_TYPE_MONO)
#include "addonu8g2.h"
#endif
#if CONFIG_BT_SPEAKER_MODE
#include "bt_speaker.h"
#endif
#include "app_main.h"
#include "custom.h"
#ifndef CONFIG_NO_XPT2046
#include "xpt2046.h"
#endif
// #include "rda5807Task.c"
#include "ClickEncoder.h"
#include "audio_player.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "fdk_aac_decoder.h"
#include "common_buffer.h"

#include "esp_system.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_netif.h"
#include "esp_mac.h"
#endif
#include "mdns.h"
#include "esp_wifi.h"

const char parslashquote[] = {"(\""};
const char parquoteslash[] = {"\")"};
const char msgsys[] = {"##SYS."};
const char msgcli[] = {"##CLI."};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
const char stritWIFISTATUS[] = {"#WIFI.STATUS#\r\nIP: %lu.%lu.%lu.%lu\r\nMask: %lu.%lu.%lu.%lu\r\nGateway: %lu.%lu.%lu.%lu\r\n##WIFI.STATUS#\r\n"};
#else
const char stritWIFISTATUS[] = {"#WIFI.STATUS#\r\nIP: %d.%d.%d.%d\r\nMask: %d.%d.%d.%d\r\nGateway: %d.%d.%d.%d\r\n##WIFI.STATUS#\r\n"};
#endif
const char stritWIFISTATION[] = {"#WIFI.STATION#\r\nSSID: %s\r\nPASSWORD: %s\r\n##WIFI.STATION#\r\n"};
const char stritPATCH[] = {"#WIFI.PATCH#: VS1053 Patch will be %s after power Off and On#\r\r\n"};
const char stritCMDERROR[] = {"##CMD_ERROR#\r\n"};
static const char stritHELP0[] = {"\
Commands:\r\n\
---------\r\n\
//////////////////\r\n\
  Debug commands   \r\n\
//////////////////\r\n\
dbg.ssl(\"x\"): Display or Tune the log level of the wolfssl component. 0: error to 3 full log.\r\n\
dbg.fifo: Display the audio buffer level.\r\n\r\n\
//////////////////\r\n\
 Wifi related commands\r\n\
//////////////////\r\n\
wifi.lis or wifi.scan: give the list of received SSID\r\n\
wifi.con: Display the AP1 and AP2 SSID\r\n\
wifi.recon: Reconnect wifi if disconnected by wifi.discon\r\n\
wifi.con(\"ssid\",\"password\"): Record the given AP ssid with password in AP1 for next reboot\r\n\
wifi.discon: disconnect the current ssid\r\n\
wifi.station: the current ssid and password\r\n\
wifi.status: give the current IP GW and mask\r\n\
wifi.rssi: print the rssi (power of the reception\r\n\
wifi.auto[(\"x\")]  show the autoconnection  state or set it to x. x=0: reboot on wifi disconnect or 1: try reconnection.\r\n\r\n\
"};
static const char stritHELP1[] = {"\
//////////////////\r\n\
  Station Client commands\r\n\
//////////////////\r\n\
cli.url(\"url\"): the name or ip of the station on instant play\r\n\
cli.path(\"path\"): the path of the station on instant play\r\n\
cli.port(\"xxxx\"): the port number of the station on instant play\r\n\
cli.instant: play the instant station\r\n\
cli.start: start to play the current station\r\n\
cli.play(\"x\"): play the x recorded station in the list\r\n\
cli.prev (or cli.previous): select the previous station in the list and play it\r\n\
cli.next: select the next station in the list and play it\
cli.stop: stop the playing station or instant\r\n\
cli.list: list all recorded stations\r\n\
cli.list(\"x\"): list only one of the recorded stations. Answer with #CLI.LISTINFO#: followed by infos\r\n\
cli.vol(\"x\"): set the volume to x with x from 0 to 23 (volume max)\r\n\
cli.vol: display the current volume. respond with ##CLI.VOL# xxx\r\n\
cli.vol-: Decrement the volume by 1 \r\n\
cli.vol+: Increment the volume by 1 \r\n\
"};
static const char stritHELP2[] = {"\
Every vol command from uart or web or browser respond with ##CLI.VOL#: xxx\r\n\
cli.mute: Toggle mute (volume temporarily set to 0 and restored on next call)\r\n\
cli.wake(\"x\"):  x in minutes. Start or stop the wake function. A value 0 stop the wake timer\r\n\
cli.sleep(\"x\"):  x in minutes. Start or stop the sleep function. A value 0 stop the sleep timer\r\n\
cli.wake: Display the current value in secondes\r\n\
cli.Sleep: Display the current value in secondes\r\n\
cli.info: Respond with nameset, all icy, meta, volume and stae playing or stopped. Used to refresh the lcd informations \r\n\r\n\
//////////////////\r\n\
  System commands\r\n\
//////////////////\r\n\
sys.uart(\"x\"): Change the baudrate of the uart on the next reset.\r\n\
 Valid x are: 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 76880, 115200, 230400\r\n\
sys.i2s: Display the current I2S speed\r\n\
"};

static const char stritHELP3[] = {"\
sys.i2s(\"x\"): Change and record the I2S clock speed of the vs1053 GPIO5 MCLK of the i2s interface to external dac.\r\n\
: 0=48kHz, 1=96kHz, 2=192kHz, other equal 0\r\n\
sys.erase: erase all recorded configuration and stations.\r\n\
sys.heap: show the ram heap size\r\n\
sys.boot: reboot.\r\n\
sys.sleep: Enter deep sleep mode.\r\n\
sys.patch and sys.patch(\"x\"): Display and Change the status of the vs1053 patch at power on.\r\n\
 0 = Patch will not be loaded, 1 or up = Patch will be loaded (default) at power On \r\n\
sys.led and sys.led(\"x\"): Display and Change the led indication:\r\n\
 1 = Led is in Play mode (lighted when a station is playing), 0 = Led is in Blink mode (default)\r\n\
sys.version: Display the Release and Revision numbers\r\n\
sys.tzo and sys.tzo(\"x:y\"): Display and Set the timezone offset of your country.\r\n\
"};

static const char stritHELP4[] = {"\
sys.date: Send a ntp request and Display the current locale time\r\n\
sys.dlog: Display the current log level\r\n\
sys.logx: Set log level to x with x=n for none, v for verbose, d for debug, i for info, w for warning, e for error\r\n\
sys.logt and sys.logt(x\"): Display and Change the log on telnet toggle. 0 = no system log on telnet\r\n\
sys.log: do nothing apart a trace on uart (debug use)\r\n\
sys.lcdout and sys.lcdout(\"x\"): Timer in seconds to switch off the lcd. 0= no timer\r\n\
sys.lcdstop and sys.lcdstop(\"x\"): Timer in seconds to switch off the lcd on stop mode. 0= no timer\r\n\
sys.lcdblv and sys.lcdblv(\"x\"): Value in percent of the backlight.\r\n\
sys.lcd and sys.lcd(\"x\"): Display and Change the lcd type to x on next reset\r\n\
"};

static const char stritHELP5[] = {"\
sys.ir and sys.ir(\"x\"): Display and Change the IR GPIO (default 21) to x\r\n\
sys.sleepio and sys.sleepio(\"x\"): Display and Change the Deep-Sleep GPIO (255 = none) to x\r\n\
sys.sleeplvl and sys.sleeplvl(\"0|1\"): Display and Change the Deep-Sleep trigger level (0 or 1)\r\n\
sys.ledgpio and sys.ledgpio(\"x\"): Display and Change the default Led GPIO (4) to x\r\n\
sys.ledpola and sys.ledpola(\"x\"): display or set the polarity of the system led\r\n\
sys.ledbr and sys.ledpbr(\"x\"): display or set the brightness of the system led from 1 to 100 \r\n\
sys.ddmm and sys.ddmm(\"x\"):  Display and Change  the date format. 0:MMDD, 1:DDMM\r\n\
sys.host and sys.host(\"your hostname\"): display and change the hostname for mDNS\r\n\
sys.rotat and sys.rotat(\"x\"): Change and display the lcd rotation option (reset needed). 0:no rotation, 1: rotation\r\n\
sys.henc0 or sys.henc1: Display the current step setting for the encoder. Normal= 4 steps/notch, Half: 2 steps/notch\r\n\
sys.hencx(\"y\") with y=0 Normal, y=1 Half\r\n\
sys.encpull(\"x\"): Display or set the internal ESP pull-up/pull-down resistors for encoders. 0=none, 1=pull-up, 2=pull-down\r\n\
sys.cali[brate]: start a touch screen calibration\r\n\
sys.conf: Display the label of the csv file\r\n\
"
#if CONFIG_BT_SPEAKER_MODE
								  "bt.forget: Remove all paired Bluetooth devices from ESP memory\r\n\
bt.list: Show all bonded Bluetooth device names and MAC addresses\r\n\
bt.name(\"x\"): Display/change the bluetooth name\r\n\
bt.pass(\"x\"): Display/change the bluetooth pin\r\n\
bt.toggle: toggle between radio and bt speaker mode (with restart)\r\n\
"
#endif
								  "///////////\r\n\
Other\r\n\
///////////\r\n\
help: this command\r\n\
<enter> will display\r\n\
#INFO:\"\"#\r\n\
\r\n\
A command error display:\r\n\
##CMD_ERROR#\r\n\r\
"};

uint16_t currentStation = 0;
static gpio_num_t led_gpio = GPIO_NONE;
static IRAM_ATTR uint32_t lcd_out = 0xFFFFFFFF;
static IRAM_ATTR uint32_t lcd_stop = 0xFFFFFFFF;

static esp_log_level_t s_log_default_level = ESP_LOG_DEBUG;
extern void wsVol(char *vol);
extern void playStation(char *id);
void clientVol(char *s);

#define MAX_WIFI_STATIONS 50
bool inside = false;
static uint8_t ddmm;
static uint8_t rotat;

// log print
int lkprintf(const char *format, va_list ap)
{
	if (format == NULL) // additinal check after kernel panic
	{
		ESP_LOGE("lkprintf", "NULL format string");
		return -1;
	}
#ifndef CONFIG_NO_TELNET
	extern bool logTel;
#endif
	// print to uart0
	int i = vprintf(format, ap);

	// send to all telnet clients
#ifndef CONFIG_NO_TELNET
	if (logTel)
		vTelnetWrite(i, format, ap);
#endif
	return i;
}

uint8_t getDdmm()
{
	return ddmm;
}
void setDdmm(uint8_t dm)
{
	if (dm == 0)
		ddmm = 0;
	else
		ddmm = 1;
}
uint8_t getRotat()
{
	return rotat;
}
void setRotat(uint8_t dm)
{
	if (dm == 0)
		rotat = 0;
	else
		rotat = 1;
}

void setVolumePlus()
{
	setRelVolume(1);
}
void setVolumeMinus()
{
	setRelVolume(-1);
}
void setVolumew(char *vol)
{
	setVolume(vol);
	wsVol(vol);
}

uint16_t getCurrentStation()
{
	return currentStation;
}
void setCurrentStation(uint16_t cst)
{
	currentStation = cst;
}

unsigned short adcdiv;

uint8_t startsWith(const char *pre, const char *str)
{
	size_t lenpre = strlen(pre),
		   lenstr = strlen(str);
	return lenstr < lenpre ? false : strncmp(pre, str, lenpre) == 0;
}

// get rssi
int8_t get_rssi(void)
{
	wifi_ap_record_t wifidata;
	esp_wifi_sta_get_ap_info(&wifidata);
	if (wifidata.primary != 0)
	{
		return (wifidata.rssi);
	}
	return -30;
}

void readRssi()
{
	kprintf("##RSSI: %d\r\n", get_rssi());
}

void printInfo(char *s)
{
	kprintf("#INFO:\"%s\"#\r\n", s);
}

const char htitle[] = {"\
=============================================================================\r\n \
             SSID                   |    RSSI    |           AUTH            \r\n\
=============================================================================\r\n\
"};
const char hscan1[] = {"#WIFI.SCAN#\r\n Number of access points found: %d\r\n"};

void wifiScan()
{
	// from https://github.com/VALERE91/ESP32_WifiScan
	uint16_t number;
	wifi_ap_record_t *records;
	wifi_scan_config_t config = {
		.ssid = NULL,
		.bssid = NULL,
		.channel = 0,
		.show_hidden = true};

	config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
	config.scan_time.passive = 500;
	esp_wifi_scan_start(&config, true);
	esp_wifi_scan_get_ap_num(&number);
	records = kmalloc(sizeof(wifi_ap_record_t) * number);
	if (records == NULL)
		return;
	esp_wifi_scan_get_ap_records(&number, records); // get the records
	kprintf(hscan1, number);
	if (number == 0)
	{
		free(records);
		return;
	}
	int i;
	kprintf(htitle);

	for (i = 0; i < number; i++)
	{
		char *authmode;
		switch (records[i].authmode)
		{
		case WIFI_AUTH_OPEN:
			authmode = (char *)"WIFI_AUTH_OPEN";
			break;
		case WIFI_AUTH_WEP:
			authmode = (char *)"WIFI_AUTH_WEP";
			break;
		case WIFI_AUTH_WPA_PSK:
			authmode = (char *)"WIFI_AUTH_WPA_PSK";
			break;
		case WIFI_AUTH_WPA2_PSK:
			authmode = (char *)"WIFI_AUTH_WPA2_PSK";
			break;
		case WIFI_AUTH_WPA_WPA2_PSK:
			authmode = (char *)"WIFI_AUTH_WPA_WPA2_PSK";
			break;
		default:
			authmode = (char *)"Unknown";
			break;
		}
		kprintf("%32.32s    |    % 4d    |    %22.22s\r\n", records[i].ssid, records[i].rssi, authmode);
	}
	kprintf("##WIFI.SCAN#: DONE\r\n");

	free(records);
}

void wifiConnect(char *cmd)
{
	int i;
	for (i = 0; i < 32; i++)
		g_device->ssid1[i] = 0;
	for (i = 0; i < 64; i++)
		g_device->pass1[i] = 0;
	char *t = strstr(cmd, parslashquote);
	if (t == 0)
	{
		kprintf(stritCMDERROR);
		return;
	}
	char *t_end = strstr(t, "\",\"");
	if (t_end == 0)
	{
		kprintf(stritCMDERROR);
		return;
	}

	strncpy(g_device->ssid1, (t + 2), (t_end - t - 2));

	t = t_end + 3;
	t_end = strstr(t, parquoteslash);
	if (t_end == 0)
	{
		kprintf(stritCMDERROR);
		return;
	}

	strncpy(g_device->pass1, t, (t_end - t));
	g_device->current_ap = 1;
	g_device->dhcpEn1 = 1;
	saveDeviceSettings(g_device);
	// test Save g_device to device1
	copyDeviceSettings();
	//
	kprintf("#WIFI.CON#\r\n");
	kprintf("##AP1: %s with dhcp on next reset#\r\n", g_device->ssid1);
	kprintf("##WIFI.CON#\r\n");
}

void wifiConnectMem()
{
	kprintf("#WIFI.CON#\r\n");
	kprintf("##AP1: %s#\r\n", g_device->ssid1);
	kprintf("##AP2: %s#\r\n", g_device->ssid2);
	kprintf("##WIFI.CON#\r\n");
}

static bool autoConWifi = true; // control for wifiReConnect & wifiDisconnect
static bool autoWifi = false;	// auto reconnect wifi if disconnected
bool getAutoWifi(void)
{
	return autoWifi;
}
void setAutoWifi()
{
	autoWifi = (g_device->options32 & T_WIFIAUTO) ? true : false;
}

void wifiAuto(char *cmd)
{
	char *t = strstr(cmd, parslashquote);
	if (t == 0)
	{
		kprintf("##Wifi Auto is %s#\r\n", autoWifi ? "On" : "Off");
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);

	if (value == 0)
		g_device->options32 &= NT_WIFIAUTO;
	else
		g_device->options32 |= T_WIFIAUTO;
	autoWifi = value;
	saveDeviceSettings(g_device);

	wifiAuto((char *)"");
}

void wifiReConnect()
{
	if (autoConWifi == false)
		esp_wifi_connect();
	autoConWifi = true;
}

void wifiDisconnect()
{
	esp_err_t err;
	autoConWifi = false;
	err = esp_wifi_disconnect();
	if (err == ESP_OK)
		kprintf("##WIFI.NOT_CONNECTED#\r\n");
	else
		kprintf("##WIFI.DISCONNECT_FAILED %d#\r\n", err);
}

void wifiStatus()
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	esp_netif_ip_info_t ipi;
	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (netif && esp_netif_get_ip_info(netif, &ipi) == ESP_OK)
	{
		kprintf(stritWIFISTATUS,
				(ipi.ip.addr & 0xff), ((ipi.ip.addr >> 8) & 0xff), ((ipi.ip.addr >> 16) & 0xff), ((ipi.ip.addr >> 24) & 0xff),
				(ipi.netmask.addr & 0xff), ((ipi.netmask.addr >> 8) & 0xff), ((ipi.netmask.addr >> 16) & 0xff), ((ipi.netmask.addr >> 24) & 0xff),
				(ipi.gw.addr & 0xff), ((ipi.gw.addr >> 8) & 0xff), ((ipi.gw.addr >> 16) & 0xff), ((ipi.gw.addr >> 24) & 0xff));
	}
	else
	{
		kprintf("Failed to get IP information.\r\n");
	}
#else

	tcpip_adapter_ip_info_t ipi;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipi);
	kprintf(stritWIFISTATUS,
			(ipi.ip.addr & 0xff), ((ipi.ip.addr >> 8) & 0xff), ((ipi.ip.addr >> 16) & 0xff), ((ipi.ip.addr >> 24) & 0xff),
			(ipi.netmask.addr & 0xff), ((ipi.netmask.addr >> 8) & 0xff), ((ipi.netmask.addr >> 16) & 0xff), ((ipi.netmask.addr >> 24) & 0xff),
			(ipi.gw.addr & 0xff), ((ipi.gw.addr >> 8) & 0xff), ((ipi.gw.addr >> 16) & 0xff), ((ipi.gw.addr >> 24) & 0xff));
#endif
}
void wifiGetStation()
{
	wifi_config_t conf;
	esp_wifi_get_config(ESP_IF_WIFI_STA, &conf);
	kprintf(stritWIFISTATION, conf.sta.ssid, conf.sta.password);
}

void clientParseUrl(char *s)
{
	char *t_end = NULL;
	char *t = strstr(s, parslashquote);
	if (t)
		t_end = strstr(t, parquoteslash);
	if ((!t) || (!t_end))
	{
		kprintf(stritCMDERROR);
		return;
	}
	t_end -= 2;

	char *url = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (url != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			url[tmp] = 0;
		strncpy(url, t + 2, (t_end - t));
		clientSetURL(url);
		char *title = kmalloc(strlen(url) + 13);
		sprintf(title, "{\"iurl\":\"%s\"}", url);
		websocketbroadcast(title, strlen(title));
		free(title);
		free(url);
	}
}

void clientParsePath(char *s)
{
	char *t_end = NULL;
	char *t = strstr(s, parslashquote);
	if (t)
		t_end = strstr(t, parquoteslash);
	if ((!t) || (!t_end))
	{
		kprintf(stritCMDERROR);
		return;
	}
	t_end -= 2;

	char *path = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (path != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			path[tmp] = 0;
		strncpy(path, t + 2, (t_end - t));
		// kprintf("cli.path: %s\r\n",path);
		clientSetPath(path);
		char *title = kmalloc(strlen(path) + 14);
		sprintf(title, "{\"ipath\":\"%s\"}", path);
		websocketbroadcast(title, strlen(title));
		free(title);
		free(path);
	}
}

void clientParsePort(char *s)
{
	char *t_end = NULL;
	char *t = strstr(s, parslashquote);
	if (t)
		t_end = strstr(t, parquoteslash);
	if ((!t) || (!t_end))
	{
		kprintf(stritCMDERROR);
		return;
	}
	t_end -= 2;

	char *port = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (port != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			port[tmp] = 0;
		strncpy(port, t + 2, (t_end - t));
		uint16_t porti = atoi(port);
		clientSetPort(porti);
		char *title = kmalloc(24);
		sprintf(title, "{\"iport\":\"%d\"}", porti);
		websocketbroadcast(title, strlen(title));
		free(title);
		free(port);
	}
}

void clientPlay(char *s)
{
	char *t_end = NULL;
	char *t = strstr(s, parslashquote);
	if (t)
		t_end = strstr(t, parquoteslash);
	if ((!t) || (!t_end))
	{
		kprintf(stritCMDERROR);
		return;
	}
	t_end -= 2;

	char *id = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (id != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			id[tmp] = 0;
		strncpy(id, t + 2, (t_end - t));
		playStation(id);
		free(id);
	}
}

const char strilLIST[] = {"##CLI.LIST#\r\n"};
const char strilINFO[] = {"#CLI.LISTINFO#: %3d: %s, %s:%d%s%%%d\r\n"};
const char strilNUM[] = {"#CLI.LISTNUM#: %3d: %s, %s:%d%s%%%d\r\n"};
const char strilDLIST[] = {"\r\n#CLI.LIST#\r\n"};

void clientList(char *s)
{
	struct shoutcast_info *si;
	uint16_t i = 0, j = 255;
	bool onlyOne = false;

	char *t = strstr(s, parslashquote);
	if (t != NULL) // a number specified
	{
		char *t_end = strstr(t, parquoteslash) - 2;
		if (t_end <= (char *)0)
		{
			kprintf(stritCMDERROR);
			return;
		}
		i = atoi(t + 2);
		if (i > 254)
			i = 0;
		j = i + 1;
		onlyOne = true;
	}
	{
		if (!onlyOne)
			kprintf(strilDLIST);
		for (; i < j; i++)
		{
			vTaskDelay(pdMS_TO_TICKS(10));
			si = getStation(i);

			if ((si == NULL) || (si->port == 0))
			{
				if (si != NULL)
				{
					free(si);
				}
				continue;
			}

			if (si != NULL)
			{
				if (si->port != 0)
				{
					if (onlyOne)
						kprintf(strilINFO, i, si->name, si->domain, si->port, si->file, si->ovol);
					else
						kprintf(strilNUM, i, si->name, si->domain, si->port, si->file, si->ovol);
				}
				free(si);
			}
		}
		if (!onlyOne)
			kprintf(strilLIST);
	}
}
// parse url
bool parseUrl(char *src, char *url, char *path, uint16_t *port)
{
	char *teu, *tbu, *tbus, *tbpa;
	char *tmp = src;
	// https?
	tmp = strstr(src, "https://");
	if (!tmp)
		tmp = strstr(src, "HTTPS://");
	if (tmp)
	{
		tbu = src;
		tbus = tbu + 8;
	}
	else // http://  remove http://
	{
		tmp = strstr(src, "://");
		if (tmp)
			tmp += 3;
		else
			tmp = src;
		tbu = tmp;
		tbus = tbu;
	}
	// printf("tbu: %s\r\n",tbu);
	teu = strchr(tbus, ':');
	tmp = tbus;
	if (teu)
	{
		tmp = teu + 1;
		*port = atoi(tmp);
	}
	tbpa = strchr(tmp, '/');
	if (tbpa)
	{
		if (!teu)
			teu = tbpa - 1;
		strcpy(path, tbpa);
	}
	if (teu)
	{
		strncpy(url, tbu, teu - tbu);
		url[teu - tbu] = 0;
	}
	else
		strcpy(url, src);
	return true;
}

// edit a station
//  format:  cli.edit("num:Name,url:port/path{%offsetVol}
//  example: cli.edit("229:Hotmix Funky,http://streaming.hotmix-radio.net:80/hotmixradio-funky-128.mp3%0")
void clientEdit(char *s)
{
	struct shoutcast_info *si;
	uint8_t id = 0xff;
	char *tmp;
	char *tmpend;
	char url[200];

	si = kmalloc(sizeof(struct shoutcast_info));
	if (si == NULL)
	{
		kprintf("##CLI.EDIT#: ERROR MEM#");
		return;
	}
	memset(si->domain, 0, sizeof(si->domain));
	memset(si->file, 0, sizeof(si->file));
	memset(si->name, 0, sizeof(si->name));
	memset(url, 0, 200);
	si->port = 80;
	si->ovol = 0;
	//	printf("##CLI.EDIT: %s",s);
	ESP_LOGI(TAG, "%s", s);
	tmp = s + 10;
	tmpend = strchr(tmp, ':');
	if ((tmp == NULL) || (tmpend == NULL))
		return;
	if (tmpend - tmp)
		id = atoi(tmp);
	tmp = ++tmpend; //:
	tmpend = strchr(tmp, ',');
	if ((tmp == NULL) || (tmpend == NULL))
		return;
	if (tmpend - tmp)
	{
		strncpy(si->name, tmp, tmpend - tmp);
	} //*tmpend = 0; }
	tmp = ++tmpend; //,
	tmpend = strchr(tmp, '%');
	if (tmpend == NULL)
		tmpend = strchr(tmp, '"');
	if (tmpend - tmp)
	{
		strncpy(url, tmp, tmpend - tmp);
	} //*tmpend = 0; }
	else
		url[0] = 0;
	tmp = ++tmpend; //%
	tmpend = strchr(tmp, '"');
	if ((tmpend != NULL) && (tmpend - tmp))
		si->ovol = atoi(tmp);

	// printf("==> id: %d, name: %s, url: %s\r\n",id,si->name,url);

	// Parsing
	if (url[0] != 0)
		parseUrl(url, si->domain, si->file, &(si->port));

	kprintf(" id: %d, name: %s, url: %s, port: %d, path: %s\r\n", id, si->name, si->domain, si->port, si->file);
	if (id < 0xff)
	{
		if (si->domain[0] == 0)
		{
			si->port = 0;
			si->file[0] = 0;
		}
		saveStation(si, id);
		kprintf("##CLI.EDIT#: OK (%d)\r\n", id);
	}
	else
		kprintf("##CLI.EDIT#: ERROR\r\n");
}
void clientInfo()
{
#if CONFIG_BT_SPEAKER_MODE
	extern uint8_t bt_sindex;
	bt_bonded_list_t *list = malloc(sizeof(bt_bonded_list_t));
	if (!list)
	{
		kprintf("##CLI.EDIT#: ERROR MEM\r\n");
		return;
	}
	gpio_get_bt_bonded_list(list);
#endif
	struct shoutcast_info *si;
	kprintf("##CLI.INFO#\r\n");
#if CONFIG_BT_SPEAKER_MODE
	if (!_isRadio)
	{
		ntp_print_time();
		int idx = BT_SINDEX_FIRST(bt_sindex);
		clientSetName(list->devices[idx].name, idx); // display  BT Name
		clientPrintHeaders();
		clientVol((char *)"");
		clientPrintState(); // print BT state
		free(list);
		return;
	}
	free(list); //
#endif
	si = getStation(currentStation);
	if (si != NULL)
	{
		ntp_print_time();
		clientSetName(si->name, currentStation);
		clientPrintHeaders();
		clientVol((char *)"");
		clientPrintState();
		free(si);
	}
}

char *webInfo()
{
	struct shoutcast_info *si;
	si = getStation(currentStation);
	char *resp = kmalloc(1024);
	if (si != NULL)
	{
		if (resp != NULL)
		{
			sprintf(resp, "vol: %d\r\nnum: %d\r\nstn: %s\r\ntit: %s\r\nsts: %d\r\n", getVolume(), currentStation, si->name, getMeta(), getState());
		}
		free(si);
	}
	return resp;
}
char *webList(int id)
{
	struct shoutcast_info *si;
	si = getStation(id);
	char *resp = kmalloc(1024);
	if (si != NULL)
	{
		if (resp != NULL)
		{
			sprintf(resp, "%s\r\n", si->name);
		}
		free(si);
	}
	return resp;
}

void sysI2S(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##I2S speed of the vs1053: %d, 0=48kHz, 1=96kHz, 2=192kHz#\r\n", g_device->i2sspeed);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t speed = atoi(t + 2);
#ifndef CONFIG_NO_VS1053
	VS1053_I2SRate(speed);
#endif
	g_device->i2sspeed = speed;
	saveDeviceSettings(g_device);
	sysI2S((char *)"");
}

void sysUart(char *s)
{
	bool empty = false;
	char *t;
	char *t_end;
	t = NULL;
	if (s != NULL)
	{
		t = strstr(s, parslashquote);
		if (t == NULL)
		{
			empty = true;
		}
		else
		{
			t_end = strstr(t, parquoteslash);
			if (t_end == NULL)
			{
				empty = true;
			}
		}
	}
	if ((!empty) && (t != NULL))
	{
		uint32_t speed = atoi(t + 2);
		speed = checkUart(speed);
		g_device->uartspeed = speed;
		saveDeviceSettings(g_device);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		kprintf("Speed: %lu\r\n", speed);
#else
		kprintf("Speed: %d\r\n", speed);
#endif
	}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
	kprintf("\r\n%sUART= %lu# on next reset\r\n", msgsys, g_device->uartspeed);
#else
	kprintf("\r\n%sUART= %d# on next reset\r\n", msgsys, g_device->uartspeed);
#endif
}

void clientVol(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == 0)
	{
		// no argument, return the current volume
		kprintf("%sVOL#: %d\r\n", msgcli, getVolume());
		return;
	}
	char *t_end = strstr(t, parquoteslash) - 2;
	if (t_end <= (char *)0)
	{

		kprintf(stritCMDERROR);
		return;
	}
	char *vol = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (vol != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			vol[tmp] = 0;
		strncpy(vol, t + 2, (t_end - t));
		if ((atoi(vol) >= 0) && (atoi(vol) <= 254))
		{
			setVolumew(vol);
			//			if (RDA5807M_detection()) RDA5807M_setVolume(atoi(vol)/16);
		}
		free(vol);
	}
}

void clientWake(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == 0)
	{
		// no argument, no action
		uint64_t temps = getWake();
		if (temps == 0ll)
			kprintf("No wake in progress\r\n");
		else
			kprintf("#Wake in %lld m  %lld s##\r\n", temps / (60ll), temps % 60ll);
		return;
	}
	char *t_end = strstr(t, parquoteslash) - 2;
	if (t_end <= (char *)0)
	{

		kprintf(stritCMDERROR);
		return;
	}
	char *label = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (label != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			label[tmp] = 0;
		strncpy(label, t + 2, (t_end - t));
		if (atoi(label) == 0)
			stopWake();
		else
			startWake(atoi(label));
		free(label);
	}
	clientWake((char *)"");
}
void clientSleep(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == 0)
	{
		// no argument, no action
		uint64_t temps = getSleep();
		if (temps == 0ll)
			kprintf("No sleep in progress\r\n");
		else
			kprintf("#Sleep in %lld m  %lld s##\r\n", temps / (60ll), temps % 60ll);
		return;
	}
	char *t_end = strstr(t, parquoteslash) - 2;
	if (t_end <= (char *)0)
	{
		kprintf(stritCMDERROR);
		return;
	}
	char *label = (char *)kmalloc((t_end - t + 1) * sizeof(char));
	if (label != NULL)
	{
		uint8_t tmp;
		for (tmp = 0; tmp < (t_end - t + 1); tmp++)
			label[tmp] = 0;
		strncpy(label, t + 2, (t_end - t));
		if (atoi(label) == 0)
			stopSleep();
		else
			startSleep(atoi(label));
		free(label);
	}
	clientSleep((char *)"");
}

// option for loading or not the pacth of the vs1053
void syspatch(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		if ((g_device->options & T_PATCH) != 0)
			kprintf("##VS1053 Patch is not loaded#\r\n");
		else
			kprintf("##VS1053 Patch is loaded#\r\n");
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value == 0)
		g_device->options |= T_PATCH;
	else
		g_device->options &= NT_PATCH; // 0 = load patch

	saveDeviceSettings(g_device);
	kprintf(stritPATCH, (g_device->options & T_PATCH) != 0 ? "unloaded" : "Loaded");
}

// the gpio to use for the led indicator
void sysledgpio(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##Led GPIO is %d#\r\n", g_device->led_gpio);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	uint8_t value = atoi(t + 2);
	if ((t_end == NULL) || (value >= GPIO_NUM_MAX))
	{
		kprintf(stritCMDERROR);
		return;
	}
	setLedGpio(value);
	gpio_output_conf(value);
	saveDeviceSettings(g_device);
	gpio_set_ledgpio(value); // write in nvs if any
	sysledgpio((char *)"");
	//	led_gpio = GPIO_NONE; // for getLedGpio
}

void sysir(char *s)
{
	extern xQueueHandle event_ir;
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		gpio_num_t ir;
		gpio_get_ir_signal(&ir);
		kprintf("##IR GPIO is %d#\r\n", ir);
		if (event_ir != NULL)
		{
			kprintf("##IR TASK is running#\r\n");
			esp_log_level_t log_level = getLogLevel();

			// Show extended debug only at DEBUG or VERBOSE level
			if (log_level == ESP_LOG_DEBUG || log_level == ESP_LOG_VERBOSE)
			{
				kprintf("##IR Loop called: %s#\r\n", ir_is_loop_called() ? "YES" : "NO");
				kprintf("##IR Custom Key Mode: %s#\r\n", ir_get_is_custom_key() ? "YES" : "NO");
				uint32_t count = ir_get_event_count();
				kprintf("##IR Events received: %d#\r\n", count);

				// Display last IR events
				if (count > 0)
				{
					ir_event_log_t events[3];
					uint8_t event_count;
					ir_get_last_events(events, &event_count);
					kprintf("##Last IR events:#\r\n");
					for (uint8_t i = 0; i < event_count; i++)
					{
						if (events[i].code != 0) // Only display non-zero events
						{
							#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
							kprintf("  %d: Code=0x%lX ADDR=0x%04X CMD=0x%04X REP=%d#\r\n",
									i + 1, (unsigned long)events[i].code, events[i].addr, events[i].cmd, events[i].repeat);
							#else
							kprintf("  %d: Code=0x%X ADDR=0x%04X CMD=0x%04X REP=%d#\r\n",
									i + 1, events[i].code, events[i].addr, events[i].cmd, events[i].repeat);
							#endif
						}
					}
				}
			}
		}
		else
		{
			kprintf("##IR TASK is NOT running#\r\n");
		}
	 	return;
	}
	char *t_end = strstr(t, parquoteslash);
	uint8_t value = atoi(t + 2);
	if ((t_end == NULL) || ((value >= GPIO_NUM_MAX) && (value != 255)))
	{
		kprintf(stritCMDERROR);
		return;
	}
	nvs_handle hardware_handle;
	if (open_partition("hardware", "gpio_space", NVS_READWRITE, &hardware_handle) == ESP_OK)
	{
		nvs_set_u8(hardware_handle, "P_IR_SIGNAL", value);
		nvs_commit(hardware_handle);
		close_partition(hardware_handle, "hardware");
		kprintf("##IR GPIO set to %d (restart required)#\r\n", value);
	}
	else
	{
		kprintf("##IR GPIO NOT set to %d #\r\n", value);
	}
}

void syssleepio(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		gpio_num_t pin;
		bool level;
		gpio_get_pinSleep(&pin, &level);
		kprintf("##P_SLEEP GPIO is %d (device wakes on %d)#\r\n", pin, level ? 0 : 1);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	uint8_t value = atoi(t + 2);
	if ((t_end == NULL) || ((value >= GPIO_NUM_MAX) && (value != 255)))
	{
		kprintf(stritCMDERROR);
		return;
	}

	gpio_num_t cur_pin;
	bool cur_level;
	gpio_get_pinSleep(&cur_pin, &cur_level);
	if ((uint8_t)cur_pin == value)
	{
		kprintf("##P_SLEEP GPIO is %d (no change)#\r\n", value);
		return;
	}

	nvs_handle hardware_handle;
	if (open_partition("hardware", "gpio_space", NVS_READWRITE, &hardware_handle) == ESP_OK)
	{
		nvs_set_u8(hardware_handle, "P_SLEEP", value);
		nvs_commit(hardware_handle);
		close_partition(hardware_handle, "hardware");
		// Reload runtime config so change takes effect without restart
		if (!deepSleepConf())
			kprintf("##P_SLEEP GPIO set to %d (applied)#\r\n", value);
		else
			kprintf("##P_SLEEP GPIO set to %d (applied)#\r\n", value);
	}
	else
	{
		kprintf("##P_SLEEP GPIO set to %d (write failed)#\r\n", value);
	}
}

void syssleeplvl(char *s)
{
	char *t = strstr(s, parslashquote);
	gpio_num_t pin;
	bool level;
	gpio_get_pinSleep(&pin, &level);
	if (t == NULL)
	{
		kprintf("##P_LEVEL_SLEEP is %d (device wakes on %d)#\r\n", level ? 1 : 0, level ? 0 : 1);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	int value = atoi(t + 2);
	if ((t_end == NULL) || (value < 0) || (value > 1))
	{
		kprintf(stritCMDERROR);
		return;
	}
	// If already same, don't rewrite
	if ((level ? 1 : 0) == value)
	{
		kprintf("##P_LEVEL_SLEEP is %d (no change)#\r\n", value);
		return;
	}

	nvs_handle hardware_handle;
	if (open_partition("hardware", "gpio_space", NVS_READWRITE, &hardware_handle) == ESP_OK)
	{
		nvs_set_u8(hardware_handle, "P_LEVEL_SLEEP", (uint8_t)value);
		nvs_commit(hardware_handle);
		close_partition(hardware_handle, "hardware");
		// Reload runtime config so change takes effect immediately
		if (!deepSleepConf())
			kprintf("##P_LEVEL_SLEEP set to %d (applied)#\r\n", value);
		else
			kprintf("##P_LEVEL_SLEEP set to %d (applied)#\r\n", value);
	}
	else
	{
		kprintf("##P_LEVEL_SLEEP set to %d (write failed)#\r\n", value);
	}
}

void setLedGpio(uint8_t val)
{
	led_gpio = val;
	g_device->led_gpio = val;
}

IRAM_ATTR uint8_t getLedGpio()
{
	return led_gpio;
}

// display or change the lcd type
void syslcd(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##LCD is %d#\r\n", g_device->lcd_type);
		kprintf("##LCD Width %d, Height %d#\r\n", GetWidth(), GetHeight());
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	g_device->lcd_type = value;
	saveDeviceSettings(g_device);
	option_set_lcd_info(value, rotat);
	kprintf("##LCD is %d on next reset#\r\n", value);
}

// display or change the DDMM display mode
void sysddmm(char *s)
{
	char *t = strstr(s, parslashquote);

	if (t == NULL)
	{
		if (ddmm)
			kprintf("##Time is DDMM#\r\n");
		else
			kprintf("##Time is MMDD#\r\n");

		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value == 0)
		g_device->options32 &= NT_DDMM;
	else
		g_device->options32 |= T_DDMM;
	ddmm = (value) ? 1 : 0;
	saveDeviceSettings(g_device);
	option_set_ddmm(ddmm);
	sysddmm((char *)"");
}

// get or set the encoder half resolution. Must be set depending of the hardware
void syshenc(int nenc, char *s)
{
	char *t = strstr(s, parslashquote);
	Encoder_t *encoder;
	bool encvalue;

	// Get the encoder instance (nenc = 0 for encoder 0, nenc = 1 for encoder 1)
	encoder = (Encoder_t *)getEncoder(nenc);
	if (encoder == NULL)
	{
		kprintf("Encoder %d not defined#\r\n", nenc);
		return;
	}

	// Retrieve the current step resolution from device options
	uint8_t options32 = g_device->options32;
	if (nenc == 0)
		encvalue = options32 & T_ENC0; // Check if encoder 0 is in half-step mode
	else
		encvalue = options32 & T_ENC1; // Check if encoder 1 is in half-step mode

	// If no argument is provided, display the current step resolution
	if (t == NULL)
	{
		kprintf("##Step for encoder%d is ", nenc);
		if (encvalue)
			kprintf("half#\r\n");
		else
			kprintf("normal#\r\n");
		return;
	}

	// Parse the argument to set the step resolution
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);

	// Update the step resolution in device options
	if (value == 0)
	{
		if (nenc == 0)
			g_device->options32 &= NT_ENC0; // Set encoder 0 to normal step mode
		else
			g_device->options32 &= NT_ENC1; // Set encoder 1 to normal step mode
	}
	else
	{
		if (nenc == 0)
			g_device->options32 |= T_ENC0; // Set encoder 0 to half-step mode
		else
			g_device->options32 |= T_ENC1; // Set encoder 1 to half-step mode
	}

	// Apply the step resolution to the encoder
	setHalfStep(encoder, value);

	// Save the updated settings to NVS
	saveDeviceSettings(g_device);

	// Display the updated step resolution
	syshenc(nenc, (char *)"");
}
void sysencpull(char *s)
{
	char *t = strstr(s, parslashquote);
	uint8_t encpull = (g_device->options32 & ENCPULL_BITS) >> 6;

	if (t == NULL)
	{
		// Display current setting
		if (encpull == 0)
			kprintf("##Encoder pull: none#\r\n");
		else if (encpull == 3)
			kprintf("##Encoder pull: pull-up#\r\n");
		else if (encpull == 2)
			kprintf("##Encoder pull: pull-down#\r\n");
		else
			kprintf("##Encoder pull: unknown (%d)#\r\n", encpull);
		return;
	}

	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);

	// Clear bits 6 and 7
	g_device->options32 &= ~ENCPULL_BITS;

	if (value == 1)
	{ // pull-up
		g_device->options32 |= ENCPULL_PULLUP;
	}
	else if (value == 2)
	{ // pull-down
		g_device->options32 |= ENCPULL_PULLDOWN;
	} // else 0: none (already cleared)

	saveDeviceSettings(g_device);
	sysencpull((char *)"");
}

// display or change the rotation lcd mode
void sysrotat(char *s)
{
	char *t = strstr(s, parslashquote);

	if (t == NULL)
	{
		kprintf("##Lcd rotation is ");
		if (rotat)
			kprintf("on#\r\n");
		else
			kprintf("off#\r\n");
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value == 0)
		g_device->options32 &= NT_ROTAT;
	else
		g_device->options32 |= T_ROTAT;
	rotat = value;
	option_set_lcd_info(g_device->lcd_type, rotat);
	saveDeviceSettings(g_device);
	sysrotat((char *)"");
}

// Timer in seconds to switch off the lcd
void syslcdout(char *s)
{
	char *t = strstr(s, parslashquote);
	//	lcd_out = g_device->lcd_out;
	if (t == NULL)
	{
		kprintf("##LCD out is ");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		kprintf("%lu#\r\n", lcd_out);
#else
		kprintf("%d#\r\n", lcd_out);
#endif
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint16_t value = atoi(t + 2);
	if ((value < 5) && value)
		value = 5; // min value
	lcd_out = value;
	//	saveDeviceSettings(g_device);
	option_set_lcd_out(lcd_out);
	syslcdout((char *)"");
	wakeLcd();
}
// Timer in seconds to switch off the lcd on stop state
void syslcdstop(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##LCD stop is ");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		kprintf("%lu#\r\n", lcd_stop);
#else
		kprintf("%d#\r\n", lcd_stop);
#endif
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint16_t value = atoi(t + 2);
	if ((value < 5) && value)
		value = 5;
	lcd_stop = value;
	option_set_lcd_stop(lcd_stop);
	syslcdstop((char *)"");
	wakeLcd();
}
// Backlight value
void syslcdblv(char *s)
{
	int lcd_blv = getBlv();

	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##LCD blv is %d#\r\n", lcd_blv);
		// kprintf("%d#\r\n", lcd_blv);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	int value = atoi(t + 2);
	if (value > 100)
		value = 100;
	if (value < 2)
		value = 2;
	lcd_blv = value;
	option_set_lcd_blv(lcd_blv);
	backlight_percentage_set(lcd_blv);
	setBlv(lcd_blv); // in addon
	syslcdblv((char *)"");
	wakeLcd();
}

uint32_t getLcdOut()
{
	option_get_lcd_out(&lcd_out, &lcd_stop);
	return lcd_out;
}
uint32_t getLcdStop()
{
	return lcd_stop;
}

// mode of the led indicator. Blink or play/stop
void sysled(char *s)
{
	extern bool ledStatus, ledPolarity;
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##Led is in %s mode#\r\n", ((g_device->options & T_LED) == 0) ? "Blink" : "Play");
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value != 0) // play mode
	{
		g_device->options |= T_LED; // set
		ledStatus = false;
		if (getState())
		{
			if (getLedGpio() != GPIO_NONE)
				gpio_set_level(getLedGpio(), ledPolarity ? 0 : 1);
		}
	}
	else // blink mode
	{
		g_device->options &= NT_LED; // clear
		ledStatus = true;
	} // options:0 = ledStatus true = Blink mode

	saveDeviceSettings(g_device);
	sysled((char *)"");
}

// mode of the led indicator. polarity 0 or 1
void sysledpol(char *s)
{
	char *t = strstr(s, parslashquote);
	extern bool ledPolarity;
	if (t == NULL)
	{
		kprintf("##Led polarity is %d#\r\n", (g_device->options & T_LEDPOL) ? 1 : 0);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value != 0)
	{
		g_device->options |= T_LEDPOL; // set
		ledPolarity = true;
		if (getState())
		{
			if (getLedGpio() != GPIO_NONE)
				gpio_set_level(getLedGpio(), ledPolarity ? 0 : 1);
		}
	}
	else
	{
		g_device->options &= NT_LEDPOL;
		ledPolarity = false;
	} // options:0 = ledPolarity

	saveDeviceSettings(g_device);
	sysledpol((char *)"");
}
// brightness of the led indicator. 0 to 100
void sysledbr(char *s)
{
	char *t = strstr(s, parslashquote);
	extern uint8_t led_brightness;
	if (t == NULL)
	{
		kprintf("##Led brightness is %d#\r\n", g_device->led_brightness);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value < 1)
		value = 1;
	if (value > 100)
		value = 100;
	g_device->led_brightness = value;
	led_brightness = value;

	saveDeviceSettings(g_device);
	sysledbr((char *)"");
}

// display or change the tzo for ntp
void tzoffset(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##SYS.TZO#: %+d:%d\r\n", g_device->tzoffseth, g_device->tzoffsetm);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	int tzoffseth = 0;
	int tzoffsetm = 0;
	sscanf(t + 2, "%d:%d", &tzoffseth, &tzoffsetm);
	g_device->tzoffseth = tzoffseth; // int to byte
	g_device->tzoffsetm = tzoffsetm;
	saveDeviceSettings(g_device);
	tzoffset((char *)"");
	addonDt(); // for addon, force the dt fetch
}

// print the heapsize
void heapSize()
{
	kprintf("%sHEAP: %d, Internal: %d #\r\n", msgsys, xPortGetFreeHeapSize(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// set hostname in mDNS
void setHostname(char *s)
{
	ESP_ERROR_CHECK(mdns_service_remove("_http", "_tcp"));
	ESP_ERROR_CHECK(mdns_service_remove("_telnet", "_tcp"));
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_ERROR_CHECK(mdns_hostname_set(s));
	ESP_ERROR_CHECK(mdns_instance_name_set(s));
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
	ESP_ERROR_CHECK(mdns_service_add(NULL, "_telnet", "_tcp", 23, NULL, 0));
}

// display or change the hostname and services
void hostname(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##SYS.HOST#: %s.local\r\n  IP:%s #\r\n", g_device->hostname, getIp());
		return;
	}

	t += 2;
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}

	if (t_end - t == 0)
		strcpy(g_device->hostname, "karadio32");
	else
	{
		if (t_end - t >= HOSTLEN)
			t_end = t + HOSTLEN;
		strncpy(g_device->hostname, t, (t_end - t) * sizeof(char));
		g_device->hostname[(t_end - t) * sizeof(char)] = 0;
	}
	saveDeviceSettings(g_device);
	setHostname(g_device->hostname);
	hostname((char *)"");
}

void displayLogLevel()
{
	switch (s_log_default_level)
	{
	case ESP_LOG_NONE:
		kprintf("Log level is now ESP_LOG_NONE\r\n");
		break;
	case ESP_LOG_ERROR:
		kprintf("Log level is now ESP_LOG_ERROR\r\n");
		break;
	case ESP_LOG_WARN:
		kprintf("Log level is now ESP_LOG_WARN\r\n");
		break;
	case ESP_LOG_INFO:
		kprintf("Log level is now ESP_LOG_INFO\r\n");
		break;
	case ESP_LOG_DEBUG:
		kprintf("Log level is now ESP_LOG_DEBUG\r\n");
		break;
	case ESP_LOG_VERBOSE:
		kprintf("Log level is now ESP_LOG_VERBOSE\r\n");
		break;
	default:
		kprintf("Log level is now Unknonwn\r\n");
	}
	// kprintf("g_device->trace_level is %d\r\n", g_device->trace_level); //#Debug
}

esp_log_level_t getLogLevel()
{
	return s_log_default_level;
}

void setLogLevel(esp_log_level_t level)
{
	esp_log_level_set("*", level);
	s_log_default_level = level;
	g_device->trace_level = level;
	saveDeviceSettings(g_device);
	displayLogLevel();
}

void setLogTelnet(char *s)
{
	extern bool logTel;
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##Log Telnet is %s#\r\n", ((g_device->options & T_LOGTEL) == 0) ? "Off" : "On");
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value != 0) // log on telnet
	{
		g_device->options |= T_LOGTEL; // set
		logTel = true;
	}
	else // no log on telnet
	{
		g_device->options &= NT_LOGTEL; // clear
		logTel = false;
	} // options:0 = ledStatus true = Blink mode

	setLogTelnet((char *)"");
	saveDeviceSettings(g_device);
}

/*
void fmSeekUp()
{seekUp();seekingComplete(); kprintf("##FM.FREQ#: %3.2f MHz\r\n",getFrequency());}
void fmSeekDown()
{seekDown();seekingComplete(); kprintf("##FM.FREQ#: %3.2f MHz\r\n",getFrequency());}
void fmVol(char* tmp)
{clientVol(tmp);}
void fmMute()
{RDA5807M_unmute(RDA5807M_FALSE); }
void fmUnmute()
{RDA5807M_unmute(RDA5807M_TRUE);}
*/

void sys_conf()
{
	char *label;
	kprintf("##CONFIG#\r\n");
	gpio_get_label(&label);
	kprintf("#LABEL: ");
	if (label != NULL)
	{
		kprintf("%s\r\n", label);
		free(label);
	}
	else
		kprintf("no label\r\n");
	gpio_get_comment(&label);

	kprintf("#COMMENT: ");
	if (label != NULL)
	{
		kprintf("%s\r\n", label);
		free(label);
	}
	else
		kprintf("no comment\r\n");
}

void dbgSSL(char *s)
{
	extern bool logTel;
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##dbg.ssl is %d#\r\n", (g_device->options & T_WOLFSSL) >> S_WOLFSSL);
		return;
	}
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	uint8_t value = atoi(t + 2);
	if (value > 3)
		value = 3;
	g_device->options &= NT_WOLFSSL; // clear
	g_device->options |= (value << S_WOLFSSL) & T_WOLFSSL;

	dbgSSL((char *)"");
	saveDeviceSettings(g_device);
}
#if CONFIG_BT_SPEAKER_MODE
void btname(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##BT Name is %s#\r\n", g_device->BTname);
		return;
	}

	t += 2;
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}

	if (t_end - t == 0)
		strcpy(g_device->BTname, CONFIG_BT_NAME);
	else
	{
		if (t_end - t >= BTHOSTLEN)
			t_end = t + BTHOSTLEN;
		strncpy(g_device->BTname, t, (t_end - t) * sizeof(char));
		g_device->BTname[(t_end - t) * sizeof(char)] = 0;
	}
	saveDeviceSettings(g_device);
	kprintf("##BT Name is %s on next reset#\r\n", g_device->BTname);
}
void btpass(char *s)
{
	char *t = strstr(s, parslashquote);
	if (t == NULL)
	{
		kprintf("##BT Pass is %s#\r\n", g_device->BTpass);
		return;
	}
	t += 2;
	char *t_end = strstr(t, parquoteslash);
	if (t_end == NULL)
	{
		kprintf(stritCMDERROR);
		return;
	}
	if (t_end - t == 0)
		strcpy(g_device->BTname, "1234");
	else
	{
		if (t_end - t >= BTPASSLEN)
			t_end = t + BTPASSLEN;
		strncpy(g_device->BTpass, t, (t_end - t) * sizeof(char));
		g_device->BTpass[(t_end - t) * sizeof(char)] = 0;
	}
	saveDeviceSettings(g_device);
	kprintf("##BT Pass is %s on next reset#\r\n", g_device->BTpass);
}
#endif
void checkCommand(int size, char *s)
{
	char *tmp = (char *)kmalloc((size + 1) * sizeof(char));
	int i;
	for (i = 0; i < size; i++)
		tmp[i] = s[i];
	tmp[size] = 0;
	//	kprintf("size: %d, cmd=%s\r\n",size,tmp);
	/*	if(startsWith ("fm.", tmp))
		{
			if(strcmp(tmp+3, "up") == 0) 	fmSeekUp();
			else if(strcmp(tmp+3, "down") == 0) 	fmSeekDown();
			else if(strcmp(tmp+3, "stop") == 0) 	fmMute();
			else if(strcmp(tmp+3, "start") == 0) 	fmUnmute();
			else if(startsWith (  "vol",tmp+3)) 	clientVol(tmp);
			else printInfo(tmp);
		} else
	*/
	if (startsWith("dbg.", tmp))
	{
		if (strcmp(tmp + 4, "fifo") == 0)
		{
			kprintf("##dbg.fifo#\r\n");
			kprintf("Buffer fill %u%%, %d bytes, OverRun: %ld, UnderRun: %ld\r\n",
					(spiRamFifoFill() * 100) / spiRamFifoLen(), spiRamFifoFill(), spiRamGetOverrunCt(), spiRamGetUnderrunCt());
			// --- AAC decoder buffers ---
			buffer_t *aac_in = get_aac_in_buf();
			buffer_t *aac_pcm = get_aac_pcm_buf();
			if (aac_in)
			{
				kprintf("AAC in_buf: %d/%d bytes unread (%.1f%%)\r\n",
						buf_data_unread(aac_in), aac_in->len,
						100.0 * buf_data_unread(aac_in) / aac_in->len);
			}
			if (aac_pcm)
			{
				kprintf("AAC PCM buf: %d/%d bytes unread (%.1f%%)\r\n",
						buf_data_unread(aac_pcm), aac_pcm->len,
						100.0 * buf_data_unread(aac_pcm) / aac_pcm->len);
			}
		}
		else if (strcmp(tmp + 4, "clear") == 0)
			spiRamFifoReset();
		else if (startsWith("ssl", tmp + 4))
			dbgSSL(tmp);
		else
			printInfo(tmp);
	}
	else if (startsWith("wifi.", tmp))
	{
		if (strcmp(tmp + 5, "list") == 0)
			wifiScan();
		else if (strcmp(tmp + 5, "scan") == 0)
			wifiScan();
		else if (strcmp(tmp + 5, "con") == 0)
			wifiConnectMem();
		else if (strcmp(tmp + 5, "recon") == 0)
			wifiReConnect();
		else if (startsWith("con", tmp + 5))
			wifiConnect(tmp);
		else if (strcmp(tmp + 5, "rssi") == 0)
			readRssi();
		else if (strcmp(tmp + 5, "discon") == 0)
			wifiDisconnect();
		else if (strcmp(tmp + 5, "status") == 0)
			wifiStatus();
		else if (strcmp(tmp + 5, "station") == 0)
			wifiGetStation();
		else if (startsWith("auto", tmp + 5))
			wifiAuto(tmp);
		else
			printInfo(tmp);
	}
	else if (startsWith("cli.", tmp))
	{
		if (startsWith("url", tmp + 4))
			clientParseUrl(tmp);
		else if (startsWith("path", tmp + 4))
			clientParsePath(tmp);
		else if (startsWith("port", tmp + 4))
			clientParsePort(tmp);
		else if (strcmp(tmp + 4, "instant") == 0)
		{
			clientDisconnect("cli instantplay");
			clientConnectOnce();
		}
		else if (strcmp(tmp + 4, "start") == 0)
			clientPlay((char *)"(\"255\")"); // outside value to play the current station
		else if (strcmp(tmp + 4, "stop") == 0)
			clientDisconnect("cli stop");
		else if (startsWith("list", tmp + 4))
			clientList(tmp);
		else if (strcmp(tmp + 4, "next") == 0)
			wsStationNext();
		else if (strncmp(tmp + 4, "previous", 4) == 0)
			wsStationPrev();
		else if (startsWith("play", tmp + 4))
			clientPlay(tmp);
		else if (strcmp(tmp + 4, "vol+") == 0)
			setVolumePlus();
		else if (strcmp(tmp + 4, "vol-") == 0)
			setVolumeMinus();
		else if (strcmp(tmp + 4, "mute") == 0)
			mute();
		else if (strcmp(tmp + 4, "info") == 0)
			clientInfo();
		else if (startsWith("vol", tmp + 4))
			clientVol(tmp);
		else if (startsWith("edit", tmp + 4))
			clientEdit(tmp);
		else if (startsWith("wake", tmp + 4))
			clientWake(tmp);
		else if (startsWith("sleep", tmp + 4))
			clientSleep(tmp);
		else
			printInfo(tmp);
	}
	else if (startsWith("sys.", tmp))
	{
		if (startsWith("i2s", tmp + 4))
			sysI2S(tmp);
		//		else if(strcmp(tmp+4, "adc") == 0) 		readAdc();
		else if (startsWith("uart", tmp + 4))
			sysUart(tmp);
		else if (strcmp(tmp + 4, "erase") == 0)
			eeEraseAll();
		else if (strcmp(tmp + 4, "heap") == 0)
			heapSize();
		else if (strcmp(tmp + 4, "boot") == 0)
		{
			audio_player_stop(); // Stop the audio player before rebooting
			vTaskDelay(pdMS_TO_TICKS(50));
			extern bool i2s_driver_installed; // You should set this flag in your I2S init/uninit code
			if (i2s_driver_installed)
			{
				i2s_driver_uninstall(I2S_NUM_0); // Use the configured I2S port
				i2s_driver_installed = false;
			}
			gpio_num_t lrck;
			gpio_num_t bclk;
			gpio_num_t i2sdata;
			gpio_get_i2s(&lrck, &bclk, &i2sdata);
			gpio_set_level(i2sdata, 0);	   // Set data pin low
			vTaskDelay(pdMS_TO_TICKS(50)); // Wait for output to settle
			esp_restart();
		}
		else if (strcmp(tmp + 4, "sleep") == 0)
		{
			kprintf("##System going to deep sleep - press encoder button or remote to wake#\r\n");
			audio_player_stop(); // Stop the audio player before sleeping
			vTaskDelay(pdMS_TO_TICKS(50));
			deepSleepEnableWakeup(); // Configure wakeup sources
			deepSleepStart();		 // Enter deep sleep
		}
		else if (strcmp(tmp + 4, "conf") == 0)
			sys_conf();
		/* OTA via CLI removed; use web interface for OTA updates */
		else if (startsWith("patch", tmp + 4))
			syspatch(tmp);
		else if (startsWith("ir", tmp + 4))
			sysir(tmp);
		else if (startsWith("sleepio", tmp + 4))
			syssleepio(tmp);
		else if (startsWith("sleeplvl", tmp + 4))
			syssleeplvl(tmp);
		else if (startsWith("ledg", tmp + 4))
			sysledgpio(tmp); // ledgpio
		else if (startsWith("ledpol", tmp + 4))
			sysledpol(tmp);
		else if (startsWith("ledbr", tmp + 4))
			sysledbr(tmp);
		else if (startsWith("led", tmp + 4))
			sysled(tmp);
		else if (strcmp(tmp + 4, "date") == 0)
			ntp_print_time();
		else if (strncmp(tmp + 4, "vers", 4) == 0)
			kprintf("Release: %s, Revision: %s, KaRadio32\r\n", RELEASE, REVISION);
		else if (startsWith("tzo", tmp + 4))
			tzoffset(tmp);
		else if (strcmp(tmp + 4, "logn") == 0)
			setLogLevel(ESP_LOG_NONE);
		else if (strcmp(tmp + 4, "loge") == 0)
			setLogLevel(ESP_LOG_ERROR);
		else if (strcmp(tmp + 4, "logw") == 0)
			setLogLevel(ESP_LOG_WARN);
		else if (strcmp(tmp + 4, "logi") == 0)
			setLogLevel(ESP_LOG_INFO);
		else if (strcmp(tmp + 4, "logd") == 0)
			setLogLevel(ESP_LOG_DEBUG);
		else if (strcmp(tmp + 4, "logv") == 0)
			setLogLevel(ESP_LOG_VERBOSE);
		else if (startsWith("logt", tmp + 4))
			setLogTelnet(tmp);
		else if (strcmp(tmp + 4, "dlog") == 0)
			displayLogLevel();
		else if (strncmp(tmp + 4, "cali", 4) == 0)
		{
#ifndef CONFIG_NO_XPT2046
			xpt_calibrate();
#endif
		}
		else if (startsWith("log", tmp + 4))
			; // do nothing
		else if (startsWith("lcdo", tmp + 4))
			syslcdout(tmp); // lcdout timer to switch off the lcd
		else if (startsWith("lcds", tmp + 4))
			syslcdstop(tmp); // lcdout timer to switch off the lcd on stop state
		else if (startsWith("lcdb", tmp + 4))
			syslcdblv(tmp); // lcd back light value
		else if (startsWith("lcd", tmp + 4))
			syslcd(tmp);
		else if (startsWith("ddmm", tmp + 4))
			sysddmm(tmp);
		else if (startsWith("host", tmp + 4))
			hostname(tmp);
		else if (startsWith("rotat", tmp + 4))
			sysrotat(tmp);
		else if (startsWith("henc0", tmp + 4))
			syshenc(0, tmp);
		else if (startsWith("henc1", tmp + 4))
			syshenc(1, tmp);
		else if (startsWith("encpull", tmp + 4))
			sysencpull(tmp);
		else
			printInfo(tmp);
	}
#if defined(CONFIG_BT_SPEAKER_MODE) && CONFIG_BT_SPEAKER_MODE
	else if (startsWith("bt", tmp))
	{
		if (startsWith("forget", tmp + 3))
		{
			bt_remove_all_bonded_devices();
			kprintf("##BT.FORGET#: All paired Bluetooth devices removed.\r\n");
		}
		else if (startsWith("name", tmp + 3))
		{
			btname(tmp);
			kprintf("##BT.NAME#: %s\r\n", g_device->BTname);
		}
		else if (startsWith("pass", tmp + 3))
		{
			btpass(tmp);
			kprintf("##BT.PASS#: %s\r\n", g_device->BTpass);
		}
		else if (startsWith("list", tmp + 3))
		{
			bt_list_bonded_devices();
		}
		else if (startsWith("toggle", tmp + 3))
		{
			radioToggle(true);
		}
		else
		{
			kprintf("##BT.CMD_ERROR#: Unknown BT command\r\n");
		}
	}
#endif
	else
	{
		if (strcmp(tmp, "help") == 0)
		{
			kprintf(stritHELP0);
			vTaskDelay(pdMS_TO_TICKS(10));
			kprintf(stritHELP1);
			vTaskDelay(pdMS_TO_TICKS(10));
			kprintf(stritHELP2);
			vTaskDelay(pdMS_TO_TICKS(10));
			kprintf(stritHELP3);
			vTaskDelay(pdMS_TO_TICKS(10));
			kprintf(stritHELP4);
			vTaskDelay(pdMS_TO_TICKS(10));
			kprintf(stritHELP5);
		}
		else
			printInfo(tmp);
	}
	free(tmp);
}

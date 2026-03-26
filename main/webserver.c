/*
 * Copyright 2016 karawin (http://www.karawin.fr)
 */

// #define LOG_LOCAL_LEVEL ESP_LOG_WARN

#include <string.h>
#include "interface.h"
#include "webserver.h"
#include "serv-fs.h"
#include "servers.h"
#include "driver/uart.h"
#include "audio_renderer.h"
#include "app_main.h"
#include "ota.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "webclient.h"
#include "vs1053.h"
#include "eeprom.h"
#include "interface.h"
#include "KaRadio_version.h"
#include "addon.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_mac.h"
#endif
#if CONFIG_BT_SPEAKER_MODE
#include "bt_speaker.h"
#endif
#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/api.h"
#include "lwip/sockets.h"

#define TAG "webserver"
static char apMode[] = {"*Hidden*"};

xSemaphoreHandle semfile = NULL;

const char strsROK[] = {"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s"};
const char tryagain[] = {"try again"};

const char lowmemory[] = {"HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nlow memory\n"};
const char strsMALLOC[] = {"WebServer inmalloc fails for %d\n"};
const char strsMALLOC1[] = {"WebServer %s kmalloc fails\n"};
const char strsSOCKET[] = {"WebServer Socket fails %s errno: %d\n"};
const char strsID[] = {"getstation, no id or Wrong id %d\n"};
const char strsR13[] = {"HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:13\r\n\r\n{\"%s\":\"%c\"}"};
const char strsICY[] = {"HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n{\"curst\":\"%s\",\"descr\":\"%s\",\"name\":\"%s\",\"bitr\":\"%s\",\"url1\":\"%s\",\"not1\":\"%s\",\"not2\":\"%s\",\"genre\":\"%s\",\"meta\":\"%s\",\"vol\":\"%s\",\"treb\":\"%s\",\"bass\":\"%s\",\"tfreq\":\"%s\",\"bfreq\":\"%s\",\"spac\":\"%s\",\"auto\":\"%c\"}"};
const char strsWIFI[] = {"{\"ssid\":\"%s\",\"pasw\":\"%s\",\"ssid2\":\"%s\",\"pasw2\":\"%s\",\
\"ip\":\"%s\",\"msk\":\"%s\",\"gw\":\"%s\",\"ip2\":\"%s\",\"msk2\":\"%s\",\"gw2\":\"%s\",\"ua\":\"%s\",\"dhcp\":\"%s\",\"dhcp2\":\"%s\",\"mac\":\"%s\"\
,\"host\":\"%s\",\"tzo\":\"%s\",\"BTname\":\"%s\",\"BTpass\":\"%s\",\"BTenabled\":\"%s\"}"};
const char strsGSTAT[] = {"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n{\"Name\":\"%s\",\"URL\":\"%s\",\"File\":\"%s\",\"Port\":\"%d\",\"ovol\":\"%d\"}"};

int8_t clientOvol = 0;

static void *inmalloc(size_t n)
{
	void *ret;
	//	ESP_LOGV(TAG, "server kmalloc of %d %d,  Heap size: %d",n,((n / 32) + 1) * 32,xPortGetFreeHeapSize( ));
	ret = kmalloc(n);
	ESP_LOGV(TAG, "server kmalloc of %x : %d bytes Heap size: %d", (int)ret, n, xPortGetFreeHeapSize());
	//	if (n <4) printf("Server: incmalloc size:%d\n",n);
	return ret;
}
static void infree(void *p)
{
	if (p != NULL)
	{
		free(p);
		ESP_LOGV(TAG, "server free of %x,  Heap size: %d", (int)p, xPortGetFreeHeapSize());
	}
}

static struct servFile *findFile(char *name)
{
	struct servFile *f = (struct servFile *)&indexFile;
	while (1)
	{
		if (strcmp(f->name, name) == 0)
			return f;
		else
			f = f->next;
		if (f == NULL)
			return NULL;
	}
}

static void respOk(int conn, const char *message)
{
	const char rempty[] = {""};
	if (message == NULL)
		message = rempty;
	char fresp[strlen(strsROK) + strlen(message) + 15]; // = inmalloc(strlen(strsROK)+strlen(message)+15);
	sprintf(fresp, strsROK, "text/plain", strlen(message), message);
	ESP_LOGV(TAG, "respOk %s", fresp);
	write(conn, fresp, strlen(fresp));
}

static void respKo(int conn)
{
	write(conn, lowmemory, strlen(lowmemory));
}

static void serveFile(char *name, int conn)
{
#define PART 1460
	int length = 0;
	int progress, part, gpart;
	char buf[270] = "HTTP/1.1 404 File not found\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 158\r\n\r\n<!DOCTYPE html><html><head><title>404 Not Found</title></head><body><h1>Not Found</h1><p>The requested URL was not found on this server.</p></body></html>\r\n";
	char *content;
	if (strcmp(name, "/style.css") == 0)
	{
		if (g_device->options & T_THEME)
			strcpy(name, "/style1.css");
		//			printf("name: %s, theme:%d\n",name,g_device->options&T_THEME);
	}
	struct servFile *f = findFile(name);
	ESP_LOGV(TAG, "find %s at %x", name, (int)f);
	// ESP_LOGV(TAG,"Heap size: %d",xPortGetFreeHeapSize( ));
	gpart = PART;
	if (f != NULL)
	{
		length = f->size;
		content = (char *)f->content;
		progress = 0;
	}

	if (length > 0)
	{
		if (xSemaphoreTake(semfile, portMAX_DELAY))
		{

			// ESP_LOGV(TAG,"serveFile socket:%d,  %s. Length: %d  sliced in %d",conn,name,length,gpart);
			sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", (f != NULL ? f->type : "text/plain"), length);
			ESP_LOGV(TAG, "serveFile send %d bytes\n%s", strlen(buf), buf);
			vTaskDelay(pdMS_TO_TICKS(10)); // why i need it? Don't know.
			if (write(conn, buf, strlen(buf)) == -1)
			{
				respKo(conn);
				ESP_LOGE(TAG, "semfile fails 0 errno:%d", errno);
				xSemaphoreGive(semfile);
				return;
			}
			progress = length;
			part = gpart;
			if (progress <= part)
				part = progress;
			while (progress > 0)
			{
				if (write(conn, content, part) == -1)
				{
					respKo(conn);
					ESP_LOGE(TAG, "semfile fails 1 errno:%d", errno);
					xSemaphoreGive(semfile);
					return;
				}

				//				ESP_LOGV(TAG,"serveFile socket:%d,  read at %x len: %d",conn,(int)content,(int)part);
				content += part;
				progress -= part;
				if (progress <= part)
					part = progress;
				vTaskDelay(pdMS_TO_TICKS(10));
			}
			xSemaphoreGive(semfile);
		}
		else
		{
			respKo(conn);
			ESP_LOGE(TAG, "semfile fails 2 errno:%d", errno);
			xSemaphoreGive(semfile);
		}
		return;
	}

	ESP_LOGV(TAG, "File not found : %s", name);
	if (write(conn, buf, strlen(buf)) == -1)
	{
		respKo(conn);
		ESP_LOGE(TAG, "semfile fails 0 errno:%d", errno);
		xSemaphoreGive(semfile);
	}

	//	ESP_LOGV(TAG,"serveFile socket:%d, end",conn);
}

static bool getSParameter(char *result, uint32_t len, const char *sep, const char *param, char *data, uint16_t data_length)
{
	if ((data == NULL) || (param == NULL))
		return false;
	char *p = strstr(data, param);
	if (p != NULL)
	{
		p += strlen(param);
		char *p_end = strstr(p, sep);
		if (p_end == NULL)
			p_end = data_length + data;
		if (p_end != NULL)
		{
			if (p_end == p)
				return false;
			int i;
			if (len > (p_end - p))
				len = p_end - p;
			for (i = 0; i < len; i++)
				result[i] = 0;
			strncpy(result, p, len);
			result[len] = 0;
			ESP_LOGV(TAG, "getSParam: in: \"%s\"   \"%s\"", data, result);
			return true;
		}
		else
			return false;
	}
	else
		return false;
}

static char *getParameter(const char *sep, const char *param, char *data, uint16_t data_length)
{
	if ((data == NULL) || (param == NULL))
		return NULL;
	char *p = strstr(data, param);
	if (p != NULL)
	{
		p += strlen(param);
		char *p_end = strstr(p, sep);
		if (p_end == NULL)
			p_end = data_length + data;
		if (p_end != NULL)
		{
			if (p_end == p)
				return NULL;
			char *t = inmalloc(p_end - p + 1);
			if (t == NULL)
			{
				printf("getParameterF fails\n");
				return NULL;
			}
			ESP_LOGV(TAG, "getParameter kmalloc of %d  for %s", p_end - p + 1, param);
			int i;
			for (i = 0; i < (p_end - p + 1); i++)
				t[i] = 0;
			strncpy(t, p, p_end - p);
			ESP_LOGV(TAG, "getParam: in: \"%s\"   \"%s\"", data, t);
			return t;
		}
		else
			return NULL;
	}
	else
		return NULL;
}

static char *getParameterFromResponse(const char *param, char *data, uint16_t data_length)
{
	return getParameter("&", param, data, data_length);
}
static bool getSParameterFromResponse(char *result, uint32_t size, const char *param, char *data, uint16_t data_length)
{
	return getSParameter(result, size, "&", param, data, data_length);
}
static char *getParameterFromComment(const char *param, char *data, uint16_t data_length)
{
	return getParameter("\"", param, data, data_length);
}

// volume offset
static void clientSetOvol(int8_t ovol)
{
	clientOvol = ovol;
	kprintf("##CLI.OVOLSET#: %d\n", ovol);
	vTaskDelay(pdMS_TO_TICKS(10));
}

// set the volume with vol,  add offset
void setVolumei(int16_t vol)
{
	setIvol(vol); // store internal vol
	int16_t uvolWithOffset = vol + clientOvol;

	if (uvolWithOffset < 0)
		uvolWithOffset = 0;
	if (uvolWithOffset > 60)
		uvolWithOffset = 60;

#ifndef CONFIG_NO_VS1053
	if (get_audio_output_mode() == VS1053)
	{
		VS1053_SetVolume(uvolWithOffset);
		return;
	}
#endif

	renderer_volume(vol); // renderer is aware of offsets internally
	kprintf("##CLI.VOL#: %d\n", getIvol());
}
void setVolume(char *volStr)
{
	if (!volStr)
		return;

	int16_t uvol = atoi(volStr);
	setVolumei(uvol);
}
// set the current volume with its offset not needed but kept
static void setOffsetVolume(void)
{
	int16_t uvol = getIvol();
	setVolumei(uvol);
}
uint16_t getVolume()
{
	return (getIvol());
}

// Incremental volume change
void setRelVolume(int8_t delta)
{
	int16_t newVol = getIvol() + delta;

	if (newVol < 0)
		newVol = 0;
	if (newVol > 23)
		newVol = 23;

	setVolumei(newVol); // backend will handle offset

	// Update webclient / UI
	char volStr[5];
	sprintf(volStr, "%d", newVol);
	wsVol(volStr);
}

// send the rssi
static void rssi(int socket)
{
	char answer[20];
	sprintf(answer, "{\"wsrssi\":\"%d\"}", get_rssi());
	websocketwrite(socket, answer, strlen(answer));
}

// flip flop the theme indicator
static void theme()
{
	if ((g_device->options & T_THEME) != 0)
		g_device->options &= NT_THEME;
	else
		g_device->options |= T_THEME;
	saveDeviceSettings(g_device);
	ESP_LOGV(TAG, "theme:%d", g_device->options & T_THEME);
}

// treat the received message of the websocket
void websockethandle(int socket, wsopcode_t opcode, uint8_t *payload, size_t length)
{
	// wsvol
	// ESP_LOGV(TAG,"websocketHandle: %s",payload);
	if (strstr((char *)payload, "wsvol=") != NULL)
	{
		char answer[17];
		if (strstr((char *)payload, "&") != NULL)
			*strstr((char *)payload, "&") = 0;
		else
			return;
		//		setVolume(payload+6);
		sprintf(answer, "{\"wsvol\":\"%s\"}", payload + 6);
		websocketlimitedbroadcast(socket, answer, strlen(answer));
	}
	else if (strstr((char *)payload, "startSleep=") != NULL)
	{
		if (strstr((char *)payload, "&") != NULL)
			*strstr((char *)payload, "&") = 0;
		else
			return;
		startSleep(atoi((char *)payload + 11));
	}
	else if (strstr((char *)payload, "stopSleep") != NULL)
	{
		stopSleep();
	}
	else if (strstr((char *)payload, "startWake=") != NULL)
	{
		if (strstr((char *)payload, "&") != NULL)
			*strstr((char *)payload, "&") = 0;
		else
			return;
		startWake(atoi((char *)payload + 10));
	}
	else if (strstr((char *)payload, "stopWake") != NULL)
	{
		stopWake();
	}
	// monitor
	else if (strstr((char *)payload, "monitor") != NULL)
	{
		wsMonitor();
	}
	else if (strstr((char *)payload, "theme") != NULL)
	{
		theme();
	}
	else if (strstr((char *)payload, "wsrssi") != NULL)
	{
		rssi(socket);
	}
#if CONFIG_BT_SPEAKER_MODE
	else if (strstr((char *)payload, "toggle") != NULL)
	{
		radioToggle(true);
	}
	else if (strstr((char *)payload, "forget") != NULL)
	{
		bt_remove_all_bonded_devices();
	}
#endif
}

void playStationInt(int sid)
{
	struct shoutcast_info *si;
	char answer[24];

	si = getStation(sid);

	// if(si->domain && si->file) {
	if (si->domain[0] != '\0' && si->file[0] != '\0')
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		clientSilentDisconnect();
		ESP_LOGV(TAG, "playstationInt: %d, new station: %s", sid, si->name);
		clientSetName(si->name, sid);
		clientSetURL(si->domain);
		clientSetPath(si->file);
		clientSetPort(si->port);
		clientSetOvol(si->ovol);

		// printf("Name: %s, url: %s, path: %s\n",	si->name,	si->domain, si->file);

		clientConnect();
		setOffsetVolume();
		for (int i = 0; i < 100; i++)
		{
			if (clientIsConnected())
				break;
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
	infree(si);
	sprintf(answer, "{\"wsstation\":\"%d\"}", sid);
	websocketbroadcast(answer, strlen(answer));
	ESP_LOGI(TAG, "playstationInt: %d, g_device: %d", sid, g_device->currentstation);
	if (g_device->currentstation != sid)
	{
		g_device->currentstation = sid;
		setCurrentStation(sid);
		saveDeviceSettings(g_device);
	}
}

void playStation(char *id)
{
	int uid = atoi(id);
	ESP_LOGV(TAG, "playstation: %d", uid);
	if (uid < 255)
		setCurrentStation(atoi(id));
	playStationInt(getCurrentStation());
}

// https://circuits4you.com/2019/03/21/esp8266-url-encode-decode-example/
unsigned char h2int(char c)
{
	if (c >= '0' && c <= '9')
	{
		return ((unsigned char)c - '0');
	}
	if (c >= 'a' && c <= 'f')
	{
		return ((unsigned char)c - 'a' + 10);
	}
	if (c >= 'A' && c <= 'F')
	{
		return ((unsigned char)c - 'A' + 10);
	}
	return (0);
}

// decode URI like Javascript
static void pathParse(char *str)
{
	char c;
	char code0;
	char code1;
	int j = 0;
	if (str == NULL)
		return;
	for (int i = 0; i < strlen(str); i++)
	{
		if (str[i] == '+')
		{
			c = ' ';
		}
		else if (str[i] == '%')
		{
			i++;
			code0 = str[i];
			i++;
			code1 = str[i];
			c = (h2int(code0) << 4) | h2int(code1);
		}
		else
		{
			c = str[i];
		}
		str[j] = c;
		j++;
	}
	str[j] = 0;
}

static void handlePOST(char *name, char *data, int data_size, int conn)
{
	ESP_LOGD(TAG, "HandlePost %s\n", name);
	//	int i;
	bool tst;
	bool changed = false;
	if (strcmp(name, "/instant_play") == 0)
	{
		if (data_size > 0)
		{
			char url[256];
			tst = getSParameterFromResponse(url, 100, "url=", data, data_size);
			char path[512];
			tst &= getSParameterFromResponse(path, 200, "path=", data, data_size);
			pathParse(path);
			char port[10];
			tst &= getSParameterFromResponse(port, 10, "port=", data, data_size);
			if (tst)
			{
				clientDisconnect("Post instPlay");
				for (int i = 0; i < 100; i++)
				{
					if (!clientIsConnected())
						break;
					vTaskDelay(pdMS_TO_TICKS(10));
				}
				clientSetURL(url);
				clientSetPath(path);
				clientSetPort(atoi(port));
				clientSetOvol(0);
				clientConnectOnce();
				setOffsetVolume();
				for (int i = 0; i < 100; i++)
				{
					if (clientIsConnected())
						break;
					vTaskDelay(pdMS_TO_TICKS(10));
				}
			}
		}
	}
	else if (strcmp(name, "/soundvol") == 0)
	{
		if (data_size > 0)
		{
			/*
						char * vol = data+4;
						data[data_size-1] = 0;
						ESP_LOGD(TAG,"/soundvol vol: %s num:%d",vol, atoi(vol));
						setVolume(vol);
						respOk(conn,NULL);
						return;
						*/
			char param[4];
			*param = 0;
			int vol;
			if (getSParameterFromResponse(param, 4, "vol=", data, data_size))
			{
				if (*param == 0)
				{
					return;
				}
				vol = atoi(param);
				if (vol < 0 || vol > 254)
				{
					return;
				}
				ESP_LOGD(TAG, "/sounvol vol: %s num:%d", param, vol);
				setVolume(param); // setVolume waits for a string
				wsVol(param);
				respOk(conn, NULL);
				return;
			}
		}
	}
#ifndef CONFIG_NO_VS1053
	else if (strcmp(name, "/sound") == 0)
	{
		if (data_size > 0)
		{
			char bass[6];
			char treble[6];
			char bassfreq[6];
			char treblefreq[6];
			char spacial[6];
			changed = false;
			if (getSParameterFromResponse(bass, 6, "bass=", data, data_size))
			{
				if (g_device->bass != atoi(bass))
				{
					if (get_audio_output_mode() == VS1053)
					{
						VS1053_SetBass(atoi(bass));
						changed = true;
						g_device->bass = atoi(bass);
					}
				}
			}
			if (getSParameterFromResponse(treble, 6, "treble=", data, data_size))
			{
				if (g_device->treble != atoi(treble))
				{
					if (get_audio_output_mode() == VS1053)
					{
						VS1053_SetTreble(atoi(treble));
						changed = true;
						g_device->treble = atoi(treble);
					}
				}
			}
			if (getSParameterFromResponse(bassfreq, 6, "bassfreq=", data, data_size))
			{
				if (g_device->freqbass != atoi(bassfreq))
				{
					if (get_audio_output_mode() == VS1053)
					{
						VS1053_SetBassFreq(atoi(bassfreq));
						changed = true;
						g_device->freqbass = atoi(bassfreq);
					}
				}
			}
			if (getSParameterFromResponse(treblefreq, 6, "treblefreq=", data, data_size))
			{
				if (g_device->freqtreble != atoi(treblefreq))
				{
					if (get_audio_output_mode() == VS1053)
					{
						VS1053_SetTrebleFreq(atoi(treblefreq));
						changed = true;
						g_device->freqtreble = atoi(treblefreq);
					}
				}
			}
			if (getSParameterFromResponse(spacial, 6, "spacial=", data, data_size))
			{
				if (g_device->spacial != atoi(spacial))
				{
					if (get_audio_output_mode() == VS1053)
					{
						VS1053_SetSpatial(atoi(spacial));
						changed = true;
						g_device->spacial = atoi(spacial);
					}
				}
			}
			if (changed)
				saveDeviceSettings(g_device);
		}
	}
#endif
	else if (strcmp(name, "/getStation") == 0)
	{
		if (data_size > 0)
		{
			char id[6];

			if (getSParameterFromResponse(id, 6, "idgp=", data, data_size))
			{
				if ((atoi(id) >= 0) && (atoi(id) < 255))
				{
					char ibuf[12];
					char *buf;
					for (int i = 0; i < sizeof(ibuf); i++)
						ibuf[i] = 0;
					struct shoutcast_info *si;
					si = getStation(atoi(id));
					if (strlen(si->domain) > sizeof(si->domain))
						si->domain[sizeof(si->domain) - 1] = 0; // truncate if any (rom crash)
					if (strlen(si->file) > sizeof(si->file))
						si->file[sizeof(si->file) - 1] = 0; // truncate if any (rom crash)
					if (strlen(si->name) > sizeof(si->name))
						si->name[sizeof(si->name) - 1] = 0; // truncate if any (rom crash)
					sprintf(ibuf, "%d%d", si->ovol, si->port);
					int json_length = strlen(si->domain) + strlen(si->file) + strlen(si->name) + strlen(ibuf) + 50;
					buf = inmalloc(json_length + 75);
					if (buf == NULL)
					{
						ESP_LOGE(TAG, " %s kmalloc fails", "getStation");
						respKo(conn);
						// return;
					}
					else
					{
						for (int i = 0; i < sizeof(buf); i++)
							buf[i] = 0;
						sprintf(buf, strsGSTAT,
								json_length, si->name, si->domain, si->file, si->port, si->ovol);
						ESP_LOGW(TAG, "getStation Buf len:%d : %s", strlen(buf), buf);
						write(conn, buf, strlen(buf));
						infree(buf);
					}
					infree(si);
					return;
				}
				else
					printf(strsID, atoi(id));
				//				infree (id);
			}
		}
	}
	else if (strcmp(name, "/setStation") == 0)
	{
		if (data_size > 0)
		{
			// printf("data:%s\n",data);
			char nb[6];
			bool res;
			uint16_t unb, uid = 0;
			bool pState = getState(); // remember if we are playing
			res = getSParameterFromResponse(nb, 6, "nb=", data, data_size);
			if (res)
			{
				ESP_LOGV(TAG, "Setstation: nb init:%s", nb);
				unb = atoi(nb);
			}
			else
			{
				unb = 1;
				ESP_LOGE(TAG, " %s nb null > set to 1 by default", "setStation");
			}

			ESP_LOGV(TAG, "unb init:%d", unb);
			struct shoutcast_info *si = inmalloc(sizeof(struct shoutcast_info) * unb);
			struct shoutcast_info *nsi;

			if (si == NULL)
			{
				ESP_LOGE(TAG, " %s kmalloc fails", "setStation");
				respKo(conn);
				return;
			}
			char *bsi = (char *)si;
			for (int j = 0; j < sizeof(struct shoutcast_info) * unb; j++)
				bsi[j] = 0; // clean

			char *url;
			char *file;
			char *name;
			char id[6];
			char port[6];
			char ovol[6];
			for (int i = 0; i < unb; i++)
			{
				nsi = si + i;
				if (getSParameterFromResponse(id, 6, "id=", data, data_size))
				{
					ESP_LOGV(TAG, "nb:%d, id:%s", i, id);
					if ((atoi(id) >= 0) && (atoi(id) < 255))
					{
						if (i == 0)
							uid = atoi(id);
						url = getParameterFromResponse("url=", data, data_size);
						file = getParameterFromResponse("file=", data, data_size);
						pathParse(file);
						name = getParameterFromResponse("name=", data, data_size);
						pathParse(name);
						if (url && file && name && getSParameterFromResponse(port, 6, "port=", data, data_size))
						{
							if (strlen(url) > sizeof(nsi->domain))
								url[sizeof(nsi->domain) - 1] = 0; // truncate if any
							strcpy(nsi->domain, url);
							if (strlen(file) > sizeof(nsi->file))
								url[sizeof(nsi->file) - 1] = 0; // truncate if any
							strcpy(nsi->file, file);
							if (strlen(name) > sizeof(nsi->name))
								url[sizeof(nsi->name) - 1] = 0; // truncate if any
							strcpy(nsi->name, name);
							nsi->ovol = (getSParameterFromResponse(ovol, 6, "ovol=", data, data_size)) ? atoi(ovol) : 0;
							nsi->port = atoi(port);
							ESP_LOGD(TAG, "Setstation nb:%d,name:%s,id:%s,url:%s,file:%s", i, name, id, url, file);
						}
						infree(name);
						infree(file);
						infree(url);
					}
				}

				data = strstr(data, "&&") + 2;
				ESP_LOGV(TAG, "si:%x, nsi:%x, addr:%x", (int)si, (int)nsi, (int)data);
			}
			ESP_LOGV(TAG, "save station: %d, unb:%d, addr:%x", uid, unb, (int)si);
			saveMultiStation(si, uid, unb);
			ESP_LOGV(TAG, "save station return: %d, unb:%d, addr:%x", uid, unb, (int)si);
			infree(si);
			if (pState != getState())
				if (pState)
				{
					clientConnect();
					vTaskDelay(100);
				} // we was playing so start again the play
		}
	}
	else if (strcmp(name, "/play") == 0)
	{
		if (data_size > 4)
		{
			char *id = data + 3;
			data[data_size - 1] = 0;
			playStation(id);
		}
	}
	else if (strcmp(name, "/auto") == 0)
	{
		if (data_size > 4)
		{
			char *id = data + 3;
			data[data_size - 1] = 0;
			if ((strcmp(id, "true")) && (g_device->autostart == 1))
			{
				g_device->autostart = 0;
				saveDeviceSettings(g_device);
			}
			else if ((strcmp(id, "false")) && (g_device->autostart == 0))
			{
				g_device->autostart = 1;
				saveDeviceSettings(g_device);
			}
		}
		if (getLogLevel() >= ESP_LOG_DEBUG)
			kprintf("auto post autostart: %d, currentstation: %d\n", g_device->autostart, g_device->currentstation);
	}
	else if (strcmp(name, "/rauto") == 0)
	{
		char buf[strlen(strsR13) + 16]; // = inmalloc( strlen(strsRAUTO)+16);
		sprintf(buf, strsR13, "rauto", (g_device->autostart) ? '1' : '0');
		// kprintf("autostart: %d, currentstation:%d\n", g_device->autostart, g_device->currentstation);
		write(conn, buf, strlen(buf));
		return;
	}
	else if (strcmp(name, "/theme") == 0)
	{
		char buf[strlen(strsR13) + 16]; // = inmalloc( strlen(strsRAUTO)+16);
		sprintf(buf, strsR13, "theme", (g_device->options & T_THEME) ? '1' : '0');
		write(conn, buf, strlen(buf));
		return;
	}
	else if (strcmp(name, "/stop") == 0)
	{
		if (clientIsConnected())
		{
			clientDisconnect("Post Stop");
			for (int i = 0; i < 100; i++)
			{
				if (!clientIsConnected())
					break;
				vTaskDelay(pdMS_TO_TICKS(10));
			}
		}
	}
	else if (strcmp(name, "/upload") == 0)
	{
		ESP_LOGI(TAG, "OTA upload requested via /upload");
		wsUpgrade("OTA: stream firmware via POST", 0, 100);
		respKo(conn); // nie akceptujemy starego bufora
		return;
	}
	else if (strcmp(name, "/icy") == 0)
	{
		ESP_LOGV(TAG, "icy vol");
		char currentSt[8];
		sprintf(currentSt, "%d", getCurrentStation());
		char vol[7];
		sprintf(vol, "%d", (getVolume()));

#if defined(CONFIG_USE_VS1053)
		char treble[5];
		char bass[5];
		char tfreq[5];
		char bfreq[5];
		char spac[5];
		if (get_audio_output_mode() == VS1053)
		{
			sprintf(treble, "%d", VS1053_GetTreble());
			sprintf(bass, "%d", VS1053_GetBass());
			sprintf(tfreq, "%d", VS1053_GetTrebleFreq());
			sprintf(bfreq, "%d", VS1053_GetBassFreq());
			sprintf(spac, "%d", VS1053_GetSpatial());
		}
		else
		{
			strcpy(treble, "0");
			strcpy(bass, "0");
			strcpy(tfreq, "0");
			strcpy(bfreq, "0");
			strcpy(spac, "0");
		}
#else
		char treble[5] = "0";
		char bass[5] = "0";
		char tfreq[5] = "0";
		char bfreq[5] = "0";
		char spac[5] = "0";
#endif
		struct icyHeader *header = clientGetHeader();
		ESP_LOGV(TAG, "icy start header %x", (int)header);
		char *not2;
		not2 = header->members.single.notice2;
		if (not2 == NULL)
			not2 = header->members.single.audioinfo;
		// if ((header->members.single.notice2 != NULL)&&(strlen(header->members.single.notice2)==0)) not2=header->members.single.audioinfo;
		int json_length;
		json_length = 166 + //
					  ((header->members.single.description == NULL) ? 0 : strlen(header->members.single.description)) +
					  ((header->members.single.name == NULL) ? 0 : strlen(header->members.single.name)) +
					  ((header->members.single.bitrate == NULL) ? 0 : strlen(header->members.single.bitrate)) +
					  ((header->members.single.url == NULL) ? 0 : strlen(header->members.single.url)) +
					  ((header->members.single.notice1 == NULL) ? 0 : strlen(header->members.single.notice1)) +
					  ((not2 == NULL) ? 0 : strlen(not2)) +
					  ((header->members.single.genre == NULL) ? 0 : strlen(header->members.single.genre)) +
					  ((header->members.single.metadata == NULL) ? 0 : strlen(header->members.single.metadata)) + strlen(currentSt) + strlen(vol) + strlen(treble) + strlen(bass) + strlen(tfreq) + strlen(bfreq) + strlen(spac);
		ESP_LOGD(TAG, "icy start header %x  len:%d vollen:%d vol:%s", (int)header, json_length, strlen(vol), vol);

		char *buf = inmalloc(json_length + 75);
		if (buf == NULL)
		{
			ESP_LOGE(TAG, " %s kmalloc fails", "post icy");
			infree(buf);
			respKo(conn);
			return;
		}
		else
		{
			uint8_t vauto = 0;
			vauto = (g_device->autostart) ? '1' : '0';
			sprintf(buf, strsICY,
					json_length,
					currentSt,
					(header->members.single.description == NULL) ? "" : header->members.single.description,
					(header->members.single.name == NULL) ? "" : header->members.single.name,
					(header->members.single.bitrate == NULL) ? "" : header->members.single.bitrate,
					(header->members.single.url == NULL) ? "" : header->members.single.url,
					(header->members.single.notice1 == NULL) ? "" : header->members.single.notice1,
					(not2 == NULL) ? "" : not2,
					(header->members.single.genre == NULL) ? "" : header->members.single.genre,
					(header->members.single.metadata == NULL) ? "" : header->members.single.metadata,
					vol, treble, bass, tfreq, bfreq, spac,
					vauto);
			ESP_LOGV(TAG, "test: len fmt:%d %d\n%s\n", strlen(strsICY), strlen(strsICY), buf);
			write(conn, buf, strlen(buf));
			infree(buf);
			wsMonitor();
			return;
		}
	}
	else if (strcmp(name, "/hardware") == 0)
	{
		bool val = false;
		uint8_t cout;
		changed = false;
		if (data_size > 0)
		{
			char valid[6];
			if (getSParameterFromResponse(valid, 6, "valid=", data, data_size))
			{
				if (strcmp(valid, "1") == 0)
					val = true;
				char coutput[6];
				getSParameterFromResponse(coutput, 6, "coutput=", data, data_size);
				cout = atoi(coutput);
				if (val)
				{
					g_device->audio_output_mode = cout;
					changed = true;
					saveDeviceSettings(g_device);
				}
				char json_body[64];
				int json_length = snprintf(json_body, sizeof(json_body),
										   "{\"coutput\":\"%d\",\"BTenabled\":\"%s\"}",
										   g_device->audio_output_mode,
#if CONFIG_BT_SPEAKER_MODE
										   "1"
#else
										   "0"
#endif
				);

				char buf[110];
				sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n{\"coutput\":\"%d\",\"BTenabled\":\"%s\"}",
						json_length,
						g_device->audio_output_mode,
#if CONFIG_BT_SPEAKER_MODE
						"1"
#else
						"0"
#endif
				);
				ESP_LOGI(TAG, "hardware Buf len:%d\n%s", strlen(buf), buf);
				write(conn, buf, strlen(buf));
				if (val)
				{
					// set current_ap to the first filled ssid
					ESP_LOGI(TAG, "audio_output_mode: %d", g_device->audio_output_mode);
					//				copyDeviceSettings();
					vTaskDelay(pdMS_TO_TICKS(20));
					esp_restart();
				}
				return;
			}
			else if (getSParameterFromResponse(valid, 5, "valid=", data, data_size))
			{
				if (strcmp(valid, "3") == 0)
				{
					char *aBTname = getParameterFromResponse("BTname=", data, data_size);
					pathParse(aBTname);
					char *aBTpass = getParameterFromResponse("BTpass=", data, data_size);
					pathParse(aBTpass);
					if (aBTname)
						strncpy(g_device->BTname, aBTname, sizeof(g_device->BTname) - 1);
					if (aBTpass)
						strncpy(g_device->BTpass, aBTpass, sizeof(g_device->BTpass) - 1);
					saveDeviceSettings(g_device);
					ESP_LOGI(TAG, "Stored: BTname: %s, BTpass: %s", g_device->BTname, g_device->BTpass);
					// Respond with updated BT config (no restart)
					char json_body[64];
					int json_length = snprintf(json_body, sizeof(json_body),
											   "{\"BTenabled\":\"%s\",\"BTname\":\"%s\",\"BTpass\":\"%s\"}",
#if CONFIG_BT_SPEAKER_MODE
											   "1",
#else
											   "0",
#endif
											   g_device->BTname, g_device->BTpass);
					char header[128];
					int header_length = snprintf(header, sizeof(header),
												 "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n",
												 json_length);
					write(conn, header, header_length);
					write(conn, json_body, json_length);
					return;
				}
			}
		}
	}
	else if (strcmp(name, "/wifi") == 0)
	{
		bool val = false;
		bool val2 = false;
		char tmpip[16], tmpmsk[16], tmpgw[16];
		char tmpip2[16], tmpmsk2[16], tmpgw2[16], tmptzo[10];
		changed = false;
		if (data_size > 0)
		{
			char valid[5];
			if (getSParameterFromResponse(valid, 5, "valid=", data, data_size))
			{
				if (strcmp(valid, "1") == 0)
					val = true;
				if (strcmp(valid, "2") == 0)
					val2 = true;
			}
			char *aua = getParameterFromResponse("ua=", data, data_size);
			pathParse(aua);
			char *host = getParameterFromResponse("host=", data, data_size);
			pathParse(host);
			char *tzo = getParameterFromResponse("tzo=", data, data_size);
			pathParse(tzo);

			//			ESP_LOGV(TAG,"wifi received  valid:%s,val:%d, ssid:%s, pasw:%s, aip:%s, amsk:%s, agw:%s, adhcp:%s, aua:%s",valid,val,ssid,pasw,aip,amsk,agw,adhcp,aua);
			if (val)
			{
				char adhcp[5], adhcp2[5];
				char *ssid = getParameterFromResponse("ssid=", data, data_size);
				pathParse(ssid);
				char *pasw = getParameterFromResponse("pasw=", data, data_size);
				pathParse(pasw);
				char *ssid2 = getParameterFromResponse("ssid2=", data, data_size);
				pathParse(ssid2);
				char *pasw2 = getParameterFromResponse("pasw2=", data, data_size);
				pathParse(pasw2);
				char *aBTname = getParameterFromResponse("BTname=", data, data_size);
				pathParse(aBTname);
				char *aBTpass = getParameterFromResponse("BTpass=", data, data_size);
				pathParse(aBTpass);
				char *aip = getParameterFromResponse("ip=", data, data_size);
				char *amsk = getParameterFromResponse("msk=", data, data_size);
				char *agw = getParameterFromResponse("gw=", data, data_size);
				char *aip2 = getParameterFromResponse("ip2=", data, data_size);
				char *amsk2 = getParameterFromResponse("msk2=", data, data_size);
				char *agw2 = getParameterFromResponse("gw2=", data, data_size);
				changed = true;
				ip_addr_t valu;
				if (aip != NULL)
				{
					ipaddr_aton(aip, &valu);
					memcpy(g_device->ipAddr1, &valu, sizeof(uint32_t));
					ipaddr_aton(amsk, &valu);
					memcpy(g_device->mask1, &valu, sizeof(uint32_t));
					ipaddr_aton(agw, &valu);
					memcpy(g_device->gate1, &valu, sizeof(uint32_t));
				}
				if (aip2 != NULL)
				{
					ipaddr_aton(aip2, &valu);
					memcpy(g_device->ipAddr2, &valu, sizeof(uint32_t));
					ipaddr_aton(amsk2, &valu);
					memcpy(g_device->mask2, &valu, sizeof(uint32_t));
					ipaddr_aton(agw2, &valu);
					memcpy(g_device->gate2, &valu, sizeof(uint32_t));
				}
				if (getSParameterFromResponse(adhcp, 4, "dhcp=", data, data_size))
					if (strlen(adhcp) != 0)
					{
						if (strcmp(adhcp, "true") == 0)
							g_device->dhcpEn1 = 1;
						else
							g_device->dhcpEn1 = 0;
					}
				if (getSParameterFromResponse(adhcp2, 4, "dhcp2=", data, data_size))
					if (strlen(adhcp2) != 0)
					{
						if (strcmp(adhcp2, "true") == 0)
							g_device->dhcpEn2 = 1;
						else
							g_device->dhcpEn2 = 0;
					}

				strcpy(g_device->ssid1, (ssid == NULL) ? "" : ssid);
				strcpy(g_device->ssid2, (ssid2 == NULL) ? "" : ssid2);
				strcpy(g_device->BTname, (aBTname == NULL) ? "" : aBTname);
				strcpy(g_device->BTpass, (aBTpass == NULL) ? "" : aBTpass);

				if (pasw != NULL)
				{
					if (strcmp(pasw, apMode) != 0)
						strcpy(g_device->pass1, pasw);
				}
				if (pasw2 != NULL)
				{
					if (strcmp(pasw2, apMode) != 0)
						strcpy(g_device->pass2, pasw2);
				}

				infree(ssid);
				infree(pasw);
				infree(ssid2);
				infree(pasw2);
				infree(aip);
				infree(amsk);
				infree(agw);
				infree(aip2);
				infree(amsk2);
				infree(agw2);
				infree(aBTname);
				infree(aBTpass);
			}

			if (/*(g_device->ua!= NULL)&&*/ (strlen(g_device->ua) == 0))
			{
				if (aua == NULL)
				{
					aua = inmalloc(12);
					strcpy(aua, "Karadio32/2.4");
				}
			}
			if (aua != NULL)
			{
				if ((strcmp(g_device->ua, aua) != 0) && (strcmp(aua, "undefined") != 0))
				{
					strcpy(g_device->ua, aua);
					changed = true;
				}
				infree(aua);
			}

			if (host != NULL)
			{
				if (strlen(host) > 0)
				{
					if ((strcmp(g_device->hostname, host) != 0) && (strcmp(host, "undefined") != 0))
					{
						strncpy(g_device->hostname, host, HOSTLEN - 1);
						setHostname(g_device->hostname);
						changed = true;
					}
				}
				infree(host);
			}

			if (tzo == NULL)
			{
				tzo = inmalloc(10);
				sprintf(tmptzo, "%d", g_device->tzoffseth);
				strcpy(tzo, tmptzo);
			}
			else if (strlen(tzo) == 0)
			{
				free(tzo);
				tzo = inmalloc(10);
				strcpy(tzo, "0");
			}

			if (strlen(tzo) > 0)
			{
				if ((strcmp(tzo, "undefined") != 0) && (val2))
				{
					int offtzo = 0;
					int offtzoh = 0;
					sscanf(tzo, "%d:%d", &offtzoh, &offtzo);
					g_device->tzoffseth = offtzoh;
					g_device->tzoffsetm = offtzo;
					addonDt();
					changed = true;
				}
			}
			infree(tzo);

			if (changed)
			{
				saveDeviceSettings(g_device);
			}
			uint8_t macaddr[10]; // = inmalloc(10*sizeof(uint8_t));
			char macstr[20];	 // = inmalloc(20*sizeof(char));
			char adhcp[4], adhcp2[4];
			esp_wifi_get_mac(WIFI_IF_STA, macaddr);

			int json_length;
			json_length = 95 + 39 + 19 + 24 +
						  strlen(g_device->ssid1) +
						  strlen(g_device->ssid2) +
						  strlen(g_device->ua) +
						  strlen(g_device->hostname) +
						  strlen(g_device->BTname) +
						  strlen(g_device->BTpass) +
						  sprintf(tmptzo, "%+d:%d", g_device->tzoffseth, g_device->tzoffsetm) +
						  sprintf(tmpip, "%d.%d.%d.%d", g_device->ipAddr1[0], g_device->ipAddr1[1], g_device->ipAddr1[2], g_device->ipAddr1[3]) +
						  sprintf(tmpmsk, "%d.%d.%d.%d", g_device->mask1[0], g_device->mask1[1], g_device->mask1[2], g_device->mask1[3]) +
						  sprintf(tmpgw, "%d.%d.%d.%d", g_device->gate1[0], g_device->gate1[1], g_device->gate1[2], g_device->gate1[3]) +
						  sprintf(adhcp, "%d", g_device->dhcpEn1) +
						  sprintf(tmpip2, "%d.%d.%d.%d", g_device->ipAddr2[0], g_device->ipAddr2[1], g_device->ipAddr2[2], g_device->ipAddr2[3]) +
						  sprintf(tmpmsk2, "%d.%d.%d.%d", g_device->mask2[0], g_device->mask2[1], g_device->mask2[2], g_device->mask2[3]) +
						  sprintf(tmpgw2, "%d.%d.%d.%d", g_device->gate2[0], g_device->gate2[1], g_device->gate2[2], g_device->gate2[3]) +
						  sprintf(adhcp2, "%d", g_device->dhcpEn2) +
						  sprintf(macstr, MACSTR, MAC2STR(macaddr)) + 16 + 1; // Add for ,"BTenabled":"1";

			char *buf = inmalloc(json_length + 95 + 39 + 10);
			if (buf == NULL)
			{
				ESP_LOGE(TAG, " %s kmalloc fails", "post wifi");
				respKo(conn);
				// return;
			}
			else
			{
				char json_body[1024];
				int json_length = snprintf(json_body, sizeof(json_body), strsWIFI,
										   g_device->ssid1, "", g_device->ssid2, "", tmpip, tmpmsk, tmpgw, tmpip2, tmpmsk2, tmpgw2,
										   g_device->ua, adhcp, adhcp2, macstr, g_device->hostname, tmptzo, g_device->BTname, g_device->BTpass,
#if CONFIG_BT_SPEAKER_MODE
										   "1"
#else
										   "0"
#endif
				);

				// Now send the HTTP header with the correct Content-Length
				char header[128];
				int header_length = snprintf(header, sizeof(header),
											 "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nContent-Length:%d\r\n\r\n",
											 json_length); // Use strlen(json_body) for the actual length

				write(conn, header, header_length);
				write(conn, json_body, json_length);
				infree(buf);
			}

			if (val)
			{
				// set current_ap to the first filled ssid
				ESP_LOGI(TAG, "currentAP: %d", g_device->current_ap);
				if (g_device->current_ap == APMODE)
				{
					if (strlen(g_device->ssid1) != 0)
						g_device->current_ap = STA1;
					else if (strlen(g_device->ssid2) != 0)
						g_device->current_ap = STA2;
					saveDeviceSettings(g_device);
				}
				ESP_LOGI(TAG, "currentAP: %d", g_device->current_ap);
				copyDeviceSettings(); // save the current one
				vTaskDelay(pdMS_TO_TICKS(50));
				esp_restart();
			}
			return;
		}
	}
	else if (strcmp(name, "/clear") == 0)
	{
		eeEraseStations(); // clear all stations
	}
#if CONFIG_BT_SPEAKER_MODE

	else if (strcmp(name, "bt.forget") == 0)
	{
		bt_remove_all_bonded_devices();
		kprintf("##BT.FORGET#: All paired Bluetooth devices removed.\n");
	}
#endif
	respOk(conn, NULL);
}

static bool httpServerHandleConnection(int conn, char *buf, uint16_t buflen)
{
	char *c;
	char *d;
	ESP_LOGD(TAG, "Heap size: %d", xPortGetFreeHeapSize());
	// printf("httpServerHandleConnection  %20c \n",&buf);
	if ((c = strstr(buf, "GET ")) != NULL)
	{
		ESP_LOGI(TAG, "GET socket:%d len: %d, str:\n%s", conn, buflen, buf);
		if (((d = strstr(buf, "Connection:")) != NULL) && ((d = strstr(d, " Upgrade")) != NULL))
		{ // a websocket request
			websocketAccept(conn, buf, buflen);
			ESP_LOGD(TAG, "websocketAccept socket: %d", conn);
			return false;
		}
		else
		{
			c += 4;
			char *c_end = strstr(c, "HTTP");
			if (c_end == NULL)
				return true;
			*(c_end - 1) = 0;
			c_end = strstr(c, "?");
			//
			// web command api,
			///////////////////
			if (c_end != NULL) // commands api
			{
				char *param;
				// printf("GET commands  socket:%d command:%s\n",conn,c);
				//  uart command
				param = strstr(c, "uart");
				if (param != NULL)
				{
					uart_set_baudrate(0, 115200);
				} // UART_SetBaudrate(0, 115200);}
				// volume command
				param = getParameterFromResponse("volume=", c, strlen(c));
				if ((param != NULL) && (atoi(param) >= 0) && (atoi(param) <= 254))
				{
					setVolume(param);
					wsVol(param);
				}
				infree(param);
				// volume+ command
				param = strstr(c, "volume+");
				if (param != NULL)
				{
					setRelVolume(5);
				}
				// volume- command
				param = strstr(c, "volume-");
				if (param != NULL)
				{
					setRelVolume(-5);
				}
				// play command
				param = getParameterFromResponse("play=", c, strlen(c));
				if (param != NULL)
				{
					playStation(param);
					infree(param);
				}
				// start command
				param = strstr(c, "start");
				if (param != NULL)
				{
					playStationInt(getCurrentStation());
				}
				// stop command
				param = strstr(c, "stop");
				if (param != NULL)
				{
					clientDisconnect("Web stop");
				}
				// next command
				param = strstr(c, "next");
				if (param != NULL)
				{
					wsStationNext();
				}
				// prev command
				param = strstr(c, "prev");
				if (param != NULL)
				{
					wsStationPrev();
				}
				// instantplay command
				param = getParameterFromComment("instant=", c, strlen(c));
				if (param != NULL)
				{
					clientDisconnect("Web Instant");
					pathParse(param);
					clientParsePlaylist(param);
					infree(param);
					clientSetName("Instant Play", 255);
					clientConnectOnce();
					vTaskDelay(pdMS_TO_TICKS(10));
				}
				// version command
				param = strstr(c, "version");
				if (param != NULL)
				{
					char vr[30]; // = kmalloc(30);
					sprintf(vr, "Release: %s, Revision: %s\n", RELEASE, REVISION);
					printf("Version:%s\n", vr);
					respOk(conn, vr);
					return true;
				}
				// infos command
				param = strstr(c, "infos");
				if (param != NULL)
				{
					char *vr = webInfo();
					respOk(conn, vr);
					infree(vr);
					return true;
				}
				// list command	 ?list=1 to list the name of the station 1
				param = getParameterFromResponse("list=", c, strlen(c));
				if ((param != NULL) && (atoi(param) >= 0) && (atoi(param) <= 254))
				{
					char *vr = webList(atoi(param));
					respOk(conn, vr);
					infree(vr);
					return true;
				}
				respOk(conn, NULL); // response OK to the origin
			}
			else
			// file GET
			{
				if (strlen(c) > 32)
				{
					respKo(conn);
					return true;
				}
				ESP_LOGI(TAG, "GET file  socket:%d file:%s", conn, c);
				serveFile(c, conn);
				ESP_LOGI(TAG, "GET end socket:%d file:%s", conn, c);
			}
		}
	}
	else if ((c = strstr(buf, "POST ")) != NULL)
	{
		// a post request
		ESP_LOGI(TAG, "POST socket: %d  buflen: %d", conn, buflen);
		char fname[32];
		uint8_t i;
		for (i = 0; i < 32; i++)
			fname[i] = 0;
		c += 5;
		char *c_end = strstr(c, " ");
		if (c_end == NULL)
			return true;
		uint8_t len = c_end - c;
		if (len > 32)
			return true;
		strncpy(fname, c, len);
		ESP_LOGI(TAG, "POST Name: %s", fname);
		// DATA
		char *d_start = strstr(buf, "\r\n\r\n");

		//		ESP_LOGV(TAG,"dstart:%s",d_start);
		if (d_start != NULL)
		{
			d_start += 4;
			uint16_t len = buflen - (d_start - buf);
			handlePOST(fname, d_start, len, conn);
		}
	}
	return true;
}

#define RECLEN 768
#define DRECLEN (RECLEN * 2)
// Server child task to handle a request from a browser.
void serverclientTask(void *pvParams)
{
	struct timeval timeout = {.tv_sec = 6, .tv_usec = 0};
	int client_sock = (int)pvParams;
	int recbytes = 0;
	char *buf = inmalloc(DRECLEN);
	if (!buf)
	{
		ESP_LOGE(TAG, "serverclientTask: buf malloc failed");
		vTaskDelete(NULL);
		return;
	}
	memset(buf, 0, DRECLEN);

	if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
	{
		ESP_LOGE(TAG, "setsockopt failed: errno %d", errno);
	}

	bool result = true;
	bool is_ota = false;
	int content_length = 0;

	// --- read request header ---
	recbytes = read(client_sock, buf, DRECLEN - 1);
	if (recbytes <= 0)
	{
		ESP_LOGE(TAG, "initial read failed: %d", recbytes);
		goto close;
	}

	buf[recbytes] = 0;

	// --- generate PIN request (do not return the PIN over HTTP) ---
	// --- has_display probe (returns "1" if device has a display, "0" otherwise)
	if (strstr(buf, "GET /has_display") || strstr(buf, "POST /has_display"))
	{
		extern uint8_t lcd_type;
		if (lcd_type == LCD_NONE)
			respOk(client_sock, "0");
		else
			respOk(client_sock, "1");
		goto close;
	}

	// --- generate PIN request (do not return the PIN over HTTP) ---
	if (strstr(buf, "GET /generate_pin") || strstr(buf, "POST /generate_pin"))
	{
		// If no display is available at runtime, bypass PIN generation
		extern uint8_t lcd_type;
		if (lcd_type == LCD_NONE)
		{
			respOk(client_sock, "No display: PIN disabled");
			goto close;
		}
		// create a one-time PIN and display it on the device (60s)
		ota_generate_pin(60); // display for 60s
		respOk(client_sock, "PIN displayed on device");
		goto close;
	}

	// --- check for POST ---
	if (strstr(buf, "POST /upload"))
	{
		is_ota = true;
		char *cl_ptr = strstr(buf, "Content-Length: ");
		if (cl_ptr)
		{
			content_length = atoi(cl_ptr + 16);
		}
		else
		{
			ESP_LOGE(TAG, "Content-Length not found");
			respKo(client_sock);
			goto close;
		}
	}
	else
	{
		char *cl_ptr = strstr(buf, "Content-Length: ");
		if (cl_ptr)
		{
			content_length = atoi(cl_ptr + 16);
		}
	}

	// --- limit normal POST size ---
	if (!is_ota && content_length > MAX_NORMAL_POST)
	{
		respKo(client_sock);
		goto close;
	}

	// --- OTA handling ---
	if (is_ota)
	{
		ESP_LOGI(TAG, "Starting OTA upload (unknown size)");

		// set expected size for progress reporting if Content-Length known
		if (content_length > 0)
			ota_set_expected_size((size_t)content_length);

		// extract X-OTA-PIN header and validate (one-time)
		char pinval[32] = {0};
		char *pin_hdr = strstr(buf, "X-OTA-PIN:");
		if (pin_hdr == NULL)
			pin_hdr = strstr(buf, "X-OTA-PIN ");
		if (pin_hdr != NULL)
		{
			// move to value
			pin_hdr += strlen("X-OTA-PIN:");
			while ((*pin_hdr == ' ' || *pin_hdr == '\t') && *pin_hdr)
				pin_hdr++;
			int pi = 0;
			while (*pin_hdr && *pin_hdr != '\r' && *pin_hdr != '\n' && pi < (int)sizeof(pinval) - 1)
			{
				pinval[pi++] = *pin_hdr++;
			}
			pinval[pi] = '\0';
		}
		// require a valid PIN
		if (pinval[0] == '\0' || !ota_check_pin(pinval))
		{
			const char forbidden[] = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nForbidden\n";
			write(client_sock, forbidden, strlen(forbidden));
			ESP_LOGW(TAG, "OTA denied: missing/invalid PIN");
			goto close;
		}

		ota_update_from_socket_start();

		// --- handle any initial data already read ---
		char *data_start = strstr(buf, "\r\n\r\n");
		if (data_start)
		{
			data_start += 4; // skip HTTP header
			int initial_data_len = recbytes - (data_start - buf);
			if (initial_data_len > 0)
			{
				if (ota_update_from_socket_write((uint8_t *)data_start, initial_data_len) != ESP_OK)
				{
					ESP_LOGE(TAG, "OTA initial write failed");
					respKo(client_sock);
					ota_update_from_socket_finish();
					goto close;
				}
			}
		}

		uint8_t tmp[1024];
		while (1)
		{
			int r = read(client_sock, tmp, sizeof(tmp)); // read up to 1024 bytes
			if (r < 0)
			{
				ESP_LOGE(TAG, "Read error: errno %d", errno);
				break;
			}
			if (r == 0)
			{
				ESP_LOGI(TAG, "OTA upload finished (client closed connection)");
				break;
			}

			if (ota_update_from_socket_write(tmp, r) != ESP_OK)
			{
				ESP_LOGE(TAG, "OTA write failed");
				respKo(client_sock);
				ota_update_from_socket_finish();
				goto close;
			}

			vTaskDelay(pdMS_TO_TICKS(1)); // allow other tasks -> avoid WDT
		}

		if (ota_update_from_socket_finish() == ESP_OK)
		{
			respOk(client_sock, "OTA update succeeded. Device will restart.");
		}
		else
		{
			respKo(client_sock);
		}
		goto close;
	}

	// --- normal HTTP handling ---
	result = httpServerHandleConnection(client_sock, buf, recbytes);

close:
	infree(buf);
	if (result)
	{
		close(client_sock);
	}
	xSemaphoreGive(semclient);
	ESP_LOGV(TAG, "Released client_sock: %d", client_sock);
	vTaskDelete(NULL);
}
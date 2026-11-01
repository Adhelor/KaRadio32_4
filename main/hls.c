/*
 * HLS (m3u8) client: manifest parsing, blocking HTTP(S) downloader and segment loop.
 * Containers are handled through hls_demux_ops_t (see hls_fmp4.c).
 */
#include "hls.h"
#include "webclient.h"
#include "audio_player.h"
#include "spiram_fifo.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "hls"
#define HLS_URL_MAX 640
#define HLS_MANIFEST_MAX (160 * 1024) // live DVR windows reach ~50 KB; lands in PSRAM
#define HLS_HDR_MAX 2048
#define HLS_RBUF 1460
#define HLS_IDLE_MS 12000

typedef int (*body_cb_t)(void *ctx, const uint8_t *d, size_t n);

typedef struct
{
	int fd;
	WOLFSSL *ssl;
} conn_t;

// ---------------------------------------------------------------- URL helpers

static bool parse_url(const char *url, bool *https, char *host, size_t hsz, uint16_t *port, char *path, size_t psz)
{
	const char *p;
	if (!strncmp(url, "https://", 8))
	{
		*https = true;
		*port = 443;
		p = url + 8;
	}
	else if (!strncmp(url, "http://", 7))
	{
		*https = false;
		*port = 80;
		p = url + 7;
	}
	else
		return false;
	size_t alen = strcspn(p, "/?#");
	const char *colon = NULL;
	for (size_t i = 0; i < alen; i++)
		if (p[i] == ':')
			colon = p + i;
	size_t hlen = colon ? (size_t)(colon - p) : alen;
	if (hlen == 0 || hlen >= hsz)
		return false;
	memcpy(host, p, hlen);
	host[hlen] = 0;
	if (colon)
		*port = (uint16_t)atoi(colon + 1);
	const char *q = p + alen;
	if (*q == '/')
		snprintf(path, psz, "%s", q);
	else
		snprintf(path, psz, "/%s", q);
	char *h = strchr(path, '#');
	if (h)
		*h = 0;
	return true;
}

static bool url_resolve(const char *base, const char *ref, char *out, size_t osz)
{
	if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8))
		return snprintf(out, osz, "%s", ref) < (int)osz;
	const char *auth = strstr(base, "://");
	if (!auth)
		return false;
	auth += 3;
	if (ref[0] == '/' && ref[1] == '/')
		return snprintf(out, osz, "%.*s%s", (int)(auth - 2 - base), base, ref) < (int)osz;
	const char *origin_end = auth + strcspn(auth, "/?#");
	if (ref[0] == '/')
		return snprintf(out, osz, "%.*s%s", (int)(origin_end - base), base, ref) < (int)osz;
	const char *path_end = auth + strcspn(auth, "?#");
	const char *dir_end = origin_end;
	for (const char *s = path_end; s > origin_end; s--)
		if (s[-1] == '/')
		{
			dir_end = s;
			break;
		}
	if (dir_end == origin_end)
		return snprintf(out, osz, "%.*s/%s", (int)(origin_end - base), base, ref) < (int)osz;
	return snprintf(out, osz, "%.*s%s", (int)(dir_end - base), base, ref) < (int)osz;
}

static const char *hdr_value(const char *hdr, const char *name)
{
	size_t nl = strlen(name);
	const char *l = strstr(hdr, "\r\n");
	while (l)
	{
		l += 2;
		if (!strncasecmp(l, name, nl) && l[nl] == ':')
		{
			l += nl + 1;
			while (*l == ' ')
				l++;
			return l;
		}
		l = strstr(l, "\r\n");
	}
	return NULL;
}

// ---------------------------------------------------------------- connection

static void sleep_ms(const hls_cfg_t *cfg, uint32_t ms)
{
	while (ms && !cfg->should_stop())
	{
		uint32_t s = ms > 100 ? 100 : ms;
		vTaskDelay(pdMS_TO_TICKS(s));
		ms -= s;
	}
}

static void conn_close(conn_t *c)
{
	if (c->ssl)
	{
		wolfSSL_free(c->ssl);
		c->ssl = NULL;
	}
	if (c->fd >= 0)
	{
		shutdown(c->fd, SHUT_RDWR);
		close(c->fd);
		c->fd = -1;
	}
}

// One kept-alive connection (hlsTask is the only user); avoids a TLS handshake per request
static conn_t kc;
static bool kvalid, khttps;
static uint16_t kport;
static char khost[96];

static void keep_drop(void)
{
	if (kvalid)
	{
		conn_close(&kc);
		kvalid = false;
	}
}

static bool conn_open(const hls_cfg_t *cfg, conn_t *c, bool https, const char *host, uint16_t port)
{
	c->fd = -1;
	c->ssl = NULL;
	struct hostent *he = gethostbyname(host);
	if (!he || !he->h_addr_list[0])
	{
		ESP_LOGE(TAG, "DNS failed: %s", host);
		return false;
	}
	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port);
	memcpy(&dest.sin_addr, he->h_addr_list[0], sizeof(dest.sin_addr));
	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd < 0)
		return false;
	struct timeval rt = {.tv_sec = 1, .tv_usec = 0}, st = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
	if (connect(c->fd, (struct sockaddr *)&dest, sizeof(dest)) < 0)
	{
		ESP_LOGE(TAG, "connect %s errno=%d", host, errno);
		conn_close(c);
		return false;
	}
	if (https)
	{
		if (!cfg->wctx || !(c->ssl = wolfSSL_new(cfg->wctx)))
		{
			conn_close(c);
			return false;
		}
		wolfSSL_set_using_nonblock(c->ssl, 1); // same mode as clientTask: timeouts -> WANT_READ
		wolfSSL_set_fd(c->ssl, c->fd);
#ifdef HAVE_SNI
		wolfSSL_UseSNI(c->ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));
#endif
		TickType_t tries_start = xTaskGetTickCount();
		int ret;
		while ((ret = wolfSSL_connect(c->ssl)) != SSL_SUCCESS)
		{
			int err = wolfSSL_get_error(c->ssl, ret);
			if ((err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) || (xTaskGetTickCount() - tries_start) > pdMS_TO_TICKS(10000) || cfg->should_stop())
			{
				ESP_LOGE(TAG, "TLS handshake failed: %d", err);
				conn_close(c);
				return false;
			}
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
	return true;
}

static bool conn_write(conn_t *c, const char *d, size_t n)
{
	if (c->ssl)
		return wolfSSL_write(c->ssl, d, n) == (int)n;
	while (n)
	{
		int r = send(c->fd, d, n, 0);
		if (r <= 0)
			return false;
		d += r;
		n -= r;
	}
	return true;
}

// >0 bytes, 0 closed, -1 error, -2 timeout
static int conn_read(conn_t *c, uint8_t *b, size_t n)
{
	if (c->ssl)
	{
		int r = wolfSSL_read(c->ssl, b, n);
		vTaskDelay(1); // let lower priority tasks (idle, display) run during bulk transfers
		if (r > 0)
			return r;
		int err = wolfSSL_get_error(c->ssl, r);
		if (err == SSL_ERROR_WANT_READ)
		{
			vTaskDelay(pdMS_TO_TICKS(10));
			return -2;
		}
		return (r == 0 || err == SSL_ERROR_ZERO_RETURN) ? 0 : -1;
	}
	int r = recv(c->fd, b, n, 0);
	vTaskDelay(1);
	if (r >= 0)
		return r;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		return -2;
	}
	return -1;
}

// ---------------------------------------------------------------- HTTP GET

typedef struct
{
	bool chunked;
	int cs; // 0 size, 1 ext, 2 data, 3 crlf, 4 done
	uint32_t csize;
	int64_t remain; // identity body with content-length, -1 unknown
	body_cb_t cb;
	void *ctx;
} body_t;

// 1 complete, 0 more needed, -1 aborted
static int body_feed(body_t *b, const uint8_t *d, size_t n)
{
	if (!b->chunked)
	{
		if (b->remain >= 0 && (int64_t)n > b->remain)
			n = (size_t)b->remain;
		if (n && b->cb(b->ctx, d, n) < 0)
			return -1;
		if (b->remain >= 0)
		{
			b->remain -= n;
			return b->remain == 0;
		}
		return 0;
	}
	while (n)
	{
		switch (b->cs)
		{
		case 0:
		case 1:
		{
			uint8_t ch = *d++;
			n--;
			if (ch == '\n')
			{
				if (b->csize == 0)
				{
					b->cs = 5; // consume the final CRLF so the connection can be reused
					break;
				}
				b->cs = 2;
			}
			else if (b->cs == 0)
			{
				int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
													   : (ch >= 'A' && ch <= 'F')	  ? ch - 'A' + 10
																					  : -1;
				if (v >= 0)
					b->csize = (b->csize << 4) | v;
				else if (ch == ';')
					b->cs = 1;
			}
			break;
		}
		case 2:
		{
			size_t t = n < b->csize ? n : b->csize;
			if (b->cb(b->ctx, d, t) < 0)
				return -1;
			d += t;
			n -= t;
			b->csize -= t;
			if (b->csize == 0)
				b->cs = 3;
			break;
		}
		case 3:
			if (*d++ == '\n')
			{
				b->cs = 0;
				b->csize = 0;
			}
			n--;
			break;
		case 5:
			if (*d++ == '\n')
			{
				b->cs = 4;
				return 1;
			}
			n--;
			break;
		default:
			return 1;
		}
	}
	return 0;
}

// 0 ok, -1 failure, -2 stopped
static int http_get(const hls_cfg_t *cfg, const char *url_in, body_cb_t cb, void *ctx)
{
	int result = -1;
	char *url = malloc(HLS_URL_MAX);
	char *path = malloc(HLS_URL_MAX);
	char *hdr = malloc(HLS_HDR_MAX);
	uint8_t *rb = malloc(HLS_RBUF);
	char host[96];
	if (!url || !path || !hdr || !rb)
		goto out;
	snprintf(url, HLS_URL_MAX, "%s", url_in);

	for (int redir = 0; redir < 4; redir++)
	{
		bool https;
		uint16_t port;
		if (!parse_url(url, &https, host, sizeof(host), &port, path, HLS_URL_MAX))
		{
			ESP_LOGE(TAG, "Bad URL: %s", url);
			goto out;
		}
		conn_t c;
		bool reused = false;
		bool allow_reuse = true;
	again:
		if (allow_reuse && kvalid && khttps == https && kport == port && !strcmp(khost, host))
		{
			c = kc;
			kvalid = false;
			reused = true;
		}
		else
		{
			keep_drop();
			reused = false;
			if (!conn_open(cfg, &c, https, host, port))
				goto out;
		}

		char *req = (char *)rb; // rb is free until the response arrives
		int rl = snprintf(req, HLS_RBUF, "GET %s HTTP/1.1\r\nHost: %s", path, host);
		if (port != (https ? 443 : 80))
			rl += snprintf(req + rl, HLS_RBUF - rl, ":%u", port);
		rl += snprintf(req + rl, HLS_RBUF - rl, "\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n", cfg->user_agent ? cfg->user_agent : "Karadio32");
		if (rl >= HLS_RBUF || !conn_write(&c, req, rl))
		{
			conn_close(&c);
			if (reused)
			{
				allow_reuse = false;
				goto again;
			}
			goto out;
		}

		// headers
		int have = 0;
		TickType_t idle = xTaskGetTickCount();
		char *end = NULL;
		while (!end)
		{
			if (cfg->should_stop())
			{
				conn_close(&c);
				result = -2;
				goto out;
			}
			if (have >= HLS_HDR_MAX - 1)
				break;
			int r = conn_read(&c, (uint8_t *)hdr + have, HLS_HDR_MAX - 1 - have);
			if (r == -2)
			{
				if (xTaskGetTickCount() - idle > pdMS_TO_TICKS(HLS_IDLE_MS))
					break;
				continue;
			}
			if (r <= 0)
			{
				if (r != -2 && have == 0 && reused)
				{
					// the server dropped the idle connection: retry on a fresh one
					conn_close(&c);
					allow_reuse = false;
					goto again;
				}
				break;
			}
			idle = xTaskGetTickCount();
			have += r;
			hdr[have] = 0;
			end = strstr(hdr, "\r\n\r\n");
		}
		if (!end)
		{
			conn_close(&c);
			goto out;
		}
		end += 4;
		int status = 0;
		if (!strncmp(hdr, "HTTP/", 5))
		{
			const char *sp = strchr(hdr, ' ');
			if (sp)
				status = atoi(sp + 1);
		}
		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
		{
			const char *loc = hdr_value(hdr, "location");
			char *tmp = malloc(HLS_URL_MAX);
			bool ok = false;
			if (loc && tmp)
			{
				size_t l = strcspn(loc, "\r\n");
				char *ref = malloc(l + 1);
				if (ref)
				{
					memcpy(ref, loc, l);
					ref[l] = 0;
					ok = url_resolve(url, ref, tmp, HLS_URL_MAX);
					free(ref);
				}
			}
			if (ok)
				snprintf(url, HLS_URL_MAX, "%s", tmp);
			free(tmp);
			conn_close(&c);
			if (!ok)
				goto out;
			continue;
		}
		if (status < 200 || status >= 300)
		{
			ESP_LOGE(TAG, "HTTP %d for %s", status, url);
			conn_close(&c);
			goto out;
		}

		body_t b = {.cb = cb, .ctx = ctx, .remain = -1};
		const char *te = hdr_value(hdr, "transfer-encoding");
		if (te && !strncasecmp(te, "chunked", 7))
			b.chunked = true;
		else
		{
			const char *cl = hdr_value(hdr, "content-length");
			if (cl)
				b.remain = atoll(cl);
		}
		int done = 0;
		size_t left = have - (end - hdr);
		if (left)
			done = body_feed(&b, (uint8_t *)end, left);
		idle = xTaskGetTickCount();
		while (done == 0)
		{
			if (cfg->should_stop())
			{
				result = -2;
				break;
			}
			int r = conn_read(&c, rb, HLS_RBUF);
			if (r == -2)
			{
				if (xTaskGetTickCount() - idle > pdMS_TO_TICKS(HLS_IDLE_MS))
					break;
				continue;
			}
			if (r <= 0)
			{
				// close-delimited body: end of stream is the end of data
				if (!b.chunked && b.remain < 0)
					done = 1;
				break;
			}
			idle = xTaskGetTickCount();
			done = body_feed(&b, rb, r);
		}
		bool reusable = false;
		if (done == 1 && result != -2)
		{
			const char *cn = hdr_value(hdr, "connection");
			reusable = !strncmp(hdr, "HTTP/1.1", 8) && (b.chunked || hdr_value(hdr, "content-length")) &&
					   !(cn && !strncasecmp(cn, "close", 5)) && b.cs != 5;
		}
		if (reusable)
		{
			kc = c;
			kvalid = true;
			khttps = https;
			kport = port;
			snprintf(khost, sizeof(khost), "%s", host);
		}
		else
			conn_close(&c);
		if (done == 1)
			result = 0;
		else if (result != -2)
			result = -1;
		goto out;
	}
	ESP_LOGE(TAG, "Too many redirects");
out:
	free(url);
	free(path);
	free(hdr);
	free(rb);
	return result;
}

// ---------------------------------------------------------------- manifest

bool hls_is_manifest(const char *t)
{
	return t && strstr(t, "#EXTM3U") &&
		   (strstr(t, "#EXT-X-TARGETDURATION") || strstr(t, "#EXT-X-STREAM-INF") ||
			strstr(t, "#EXT-X-MEDIA-SEQUENCE") || strstr(t, "#EXT-X-VERSION"));
}

typedef struct
{
	bool master, endlist, unsupported;
	uint32_t target;
	int64_t media_seq;
	int nseg;
	char variant[HLS_URL_MAX];
	bool variant_audio;
} plinfo_t;

typedef struct
{
	const hls_cfg_t *cfg;
	char *mbuf;
	size_t mlen;
	char media_url[HLS_URL_MAX];
	char map_url[HLS_URL_MAX];
	char last_map[HLS_URL_MAX];
	char seg_url[HLS_URL_MAX];
	int64_t last_seq;
	bool have_last, playing_notified;
	void *demux;
	const hls_demux_ops_t *ops;
} hls_ctx_t;

static char *next_line(char **p)
{
	char *s = *p;
	if (!s || !*s)
		return NULL;
	char *e = strchr(s, '\n');
	if (e)
	{
		*e = 0;
		*p = e + 1;
	}
	else
		*p = s + strlen(s);
	size_t l = strlen(s);
	while (l && (s[l - 1] == '\r' || s[l - 1] == ' '))
		s[--l] = 0;
	return s;
}

static bool attr_uri(const char *line, char *out, size_t osz)
{
	const char *u = strstr(line, "URI=\"");
	if (!u)
		return false;
	u += 5;
	size_t l = strcspn(u, "\"");
	if (l >= osz)
		return false;
	memcpy(out, u, l);
	out[l] = 0;
	return true;
}

static void analyze(char *text, plinfo_t *pi)
{
	memset(pi, 0, sizeof(*pi));
	char *p = text, *line;
	bool pend_variant = false, pend_audio = false, have_variant = false;
	while ((line = next_line(&p)))
	{
		if (line[0] == '#')
		{
			if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22))
				pi->target = strtoul(line + 22, NULL, 10);
			else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22))
				pi->media_seq = strtoll(line + 22, NULL, 10);
			else if (!strcmp(line, "#EXT-X-ENDLIST"))
				pi->endlist = true;
			else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18))
			{
				pi->master = true;
				pend_variant = true;
				pend_audio = strstr(line, "RESOLUTION=") == NULL;
			}
			else if (!strncmp(line, "#EXT-X-KEY:", 11))
			{
				if (!strstr(line, "METHOD=NONE"))
					pi->unsupported = true; // encrypted segments
			}
			else if (!strncmp(line, "#EXT-X-BYTERANGE:", 17))
				pi->unsupported = true;
			else if (!strncmp(line, "#EXTINF", 7))
				pi->nseg++;
		}
		else if (line[0] && pend_variant)
		{
			pend_variant = false;
			if (!have_variant || (pend_audio && !pi->variant_audio))
			{
				snprintf(pi->variant, sizeof(pi->variant), "%s", line);
				pi->variant_audio = pend_audio;
				have_variant = true;
			}
		}
	}
}

// ---------------------------------------------------------------- segment loop

static int manifest_cb(void *c, const uint8_t *d, size_t n)
{
	hls_ctx_t *x = c;
	size_t room = HLS_MANIFEST_MAX - 1 - x->mlen;
	if (n > room)
	{
		ESP_LOGE(TAG, "manifest too large");
		return -1;
	}
	memcpy(x->mbuf + x->mlen, d, n);
	x->mlen += n;
	return 0;
}

typedef struct
{
	hls_ctx_t *x;
	bool init;
} feed_ctx_t;

static int feed_cb(void *c, const uint8_t *d, size_t n)
{
	feed_ctx_t *f = c;
	if (f->x->cfg->should_stop())
		return -1;
	return f->init ? f->x->ops->feed_init(f->x->demux, d, n) : f->x->ops->feed(f->x->demux, d, n);
}

static const hls_cfg_t *sink_cfg;

static int audio_sink(const uint8_t *d, size_t n)
{
	// spiRamFifoWrite blocks forever on a full FIFO once the decoder is stopped; wait here so a stop request is seen
	while (spiRamFifoFree() < n)
	{
		if (sink_cfg && sink_cfg->should_stop())
			return -1;
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	return audio_stream_consumer((const char *)d, n);
}

static bool has_ext(const char *url, const char *const *exts)
{
	size_t l = strcspn(url, "?#");
	for (; *exts; exts++)
	{
		size_t el = strlen(*exts);
		if (l >= el && !strncasecmp(url + l - el, *exts, el))
			return true;
	}
	return false;
}

static bool ensure_demux(hls_ctx_t *x, const char *seg_rel)
{
	if (x->demux)
		return true;
	static const char *const fmp4_ext[] = {".m4s", ".mp4", ".m4a", ".cmfa", NULL};
	static const char *const ts_ext[] = {".ts", NULL};
	const hls_demux_ops_t *ops = NULL;
	if (x->map_url[0] || has_ext(seg_rel, fmp4_ext))
		ops = &hls_fmp4_ops;
	else if (has_ext(seg_rel, ts_ext))
		ops = &hls_ts_ops;
	if (!ops)
	{
		ESP_LOGE(TAG, "Unsupported segment container: %s", seg_rel);
		return false;
	}
	x->demux = ops->create(audio_sink);
	if (!x->demux)
		return false;
	x->ops = ops;
	ESP_LOGI(TAG, "container: %s", ops->name);
	x->cfg->on_start(ops->content_type);
	return true;
}

// 0 ok (n = fetched count), 1 stopped, -1 error
static int process_segments(hls_ctx_t *x, char *text, const plinfo_t *pi, int *fetched)
{
	const hls_cfg_t *cfg = x->cfg;
	*fetched = 0;
	if (x->have_last && x->last_seq >= pi->media_seq + pi->nseg)
		x->have_last = false; // playlist restarted
	int first_idx = 0;
	if (!x->have_last && !pi->endlist && pi->nseg > 2)
		first_idx = pi->nseg - 2; // join close to the live edge, keep a little buffer
	int idx = 0;
	bool pending = false, disc = false;
	char *p = text, *line;
	while ((line = next_line(&p)))
	{
		if (line[0] == '#')
		{
			if (!strncmp(line, "#EXT-X-MAP:", 11))
			{
				char rel[HLS_URL_MAX];
				if (attr_uri(line, rel, sizeof(rel)))
					url_resolve(x->media_url, rel, x->map_url, sizeof(x->map_url));
			}
			else if (!strncmp(line, "#EXT-X-DISCONTINUITY", 20) && line[20] != '-')
				disc = true;
			else if (!strncmp(line, "#EXTINF", 7))
				pending = true;
			continue;
		}
		if (!line[0] || !pending)
			continue;
		pending = false;
		bool seg_disc = disc;
		disc = false;
		int64_t seq = pi->media_seq + idx;
		bool want = x->have_last ? (seq > x->last_seq) : (idx >= first_idx);
		idx++;
		if (!want)
			continue;
		if (cfg->should_stop())
			return 1;
		if (!ensure_demux(x, line))
			return -1;
		if (seg_disc && x->have_last)
		{
			// a new timeline: the init segment may differ even when its URL is unchanged
			ESP_LOGI(TAG, "discontinuity at segment %d", (int)seq);
			x->last_map[0] = 0;
		}
		feed_ctx_t f = {.x = x};
		int rc;
		if (x->map_url[0] && strcmp(x->map_url, x->last_map))
		{
			f.init = true;
			rc = http_get(cfg, x->map_url, feed_cb, &f);
			if (rc == -2 || cfg->should_stop())
				return 1;
			if (rc < 0)
				return -1;
			snprintf(x->last_map, sizeof(x->last_map), "%s", x->map_url);
			f.init = false;
		}
		if (!url_resolve(x->media_url, line, x->seg_url, sizeof(x->seg_url)))
			return -1;
		TickType_t t0 = xTaskGetTickCount();
		rc = http_get(cfg, x->seg_url, feed_cb, &f);
		x->ops->segment_end(x->demux);
		ESP_LOGD(TAG, "segment %d fetched in %d ms", (int)seq, (int)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
		if (rc == -2 || cfg->should_stop())
			return 1;
		if (rc < 0)
		{
			ESP_LOGW(TAG, "segment %lld failed", (long long)seq);
			return -1;
		}
		x->last_seq = seq;
		x->have_last = true;
		(*fetched)++;
		if (!x->playing_notified)
		{
			x->playing_notified = true;
			cfg->on_playing();
		}
	}
	return 0;
}

hls_result_t hls_play(const hls_cfg_t *cfg, const char *url)
{
	hls_result_t res = HLS_ERROR;
	hls_ctx_t *x = calloc(1, sizeof(hls_ctx_t));
	if (!x)
		return HLS_ERROR;
	x->cfg = cfg;
	sink_cfg = cfg;
	x->mbuf = malloc(HLS_MANIFEST_MAX);
	if (!x->mbuf)
	{
		free(x);
		return HLS_ERROR;
	}
	snprintf(x->media_url, sizeof(x->media_url), "%s", url);
	int errors = 0;
	plinfo_t *pi = malloc(sizeof(plinfo_t));
	if (!pi)
		goto done;

	while (1)
	{
		if (cfg->should_stop())
		{
			res = HLS_STOPPED;
			break;
		}
		x->mlen = 0;
		int rc = http_get(cfg, x->media_url, manifest_cb, x);
		if (rc == -2)
		{
			res = HLS_STOPPED;
			break;
		}
		x->mbuf[x->mlen] = 0;
		if (rc < 0 || !hls_is_manifest(x->mbuf))
		{
			ESP_LOGW(TAG, "manifest error (%d)", errors + 1);
			if (++errors >= 6)
				break;
			sleep_ms(cfg, 2000);
			continue;
		}
		char *copy = malloc(x->mlen + 1); // analyze() splits lines in place
		if (!copy)
			break;
		memcpy(copy, x->mbuf, x->mlen + 1);
		analyze(copy, pi);
		free(copy);
		if (pi->master)
		{
			char *next = malloc(HLS_URL_MAX);
			bool ok = next && url_resolve(x->media_url, pi->variant, next, HLS_URL_MAX);
			if (ok)
				snprintf(x->media_url, sizeof(x->media_url), "%s", next);
			free(next);
			if (!ok || ++errors > 4)
				break;
			ESP_LOGI(TAG, "master playlist -> %s", x->media_url);
			continue;
		}
		if (pi->unsupported)
		{
			ESP_LOGE(TAG, "Encrypted or byte-range HLS is not supported");
			break;
		}
		int fetched = 0;
		rc = process_segments(x, x->mbuf, pi, &fetched);
		if (rc == 1)
		{
			res = HLS_STOPPED;
			break;
		}
		if (rc < 0)
		{
			if (++errors >= 4)
				break;
			continue;
		}
		errors = 0;
		if (pi->endlist && fetched == 0)
		{
			res = HLS_ENDED;
			break;
		}
		if (fetched == 0)
		{
			uint32_t ms = pi->target ? pi->target * 500 : 3000;
			sleep_ms(cfg, ms < 1000 ? 1000 : ms);
		}
	}
done:
	keep_drop();
	if (x->demux)
		x->ops->destroy(x->demux);
	free(pi);
	free(x->mbuf);
	free(x);
	return res;
}

/*
 * HLS MPEG-TS demuxer: extracts the ADTS AAC elementary stream (stream_type 0x0F).
 */
#include "hls.h"
#include "webclient.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

#define TAG "hls_ts"
#define TS_PKT 188
#define TS_OUT 3008 // 16 packets worth of payload, stays below the 4 KB internal-malloc limit

typedef struct
{
	hls_sink_t sink;
	uint8_t pkt[TS_PKT];
	int fill;
	int pmt_pid;
	int audio_pid;
	int pes_skip; // PES header bytes still to discard
	int last_cc;  // last audio continuity counter, -1 at segment start
	int cc_err;	  // lost TS packets seen in the current segment
	bool unsupported_logged;
	int meta_pid;
	int meta_len;
	uint8_t meta[384]; // one ID3 PES (title/artist frames)
	int olen;
	uint8_t out[TS_OUT];
} ts_t;

// Batches the payload so the FIFO is not locked for every 184-byte packet
static int ts_out(ts_t *c, const uint8_t *d, int n)
{
	while (n > 0)
	{
		int k = TS_OUT - c->olen;
		if (k > n)
			k = n;
		memcpy(c->out + c->olen, d, k);
		c->olen += k;
		d += k;
		n -= k;
		if (c->olen == TS_OUT)
		{
			c->olen = 0;
			if (c->sink(c->out, TS_OUT) < 0)
				return -1;
		}
	}
	return 0;
}

static void parse_pat(ts_t *c, const uint8_t *s, int n)
{
	if (n < 12 || s[0] != 0x00)
		return;
	int len = ((s[1] & 0x0F) << 8) | s[2];
	int end = 3 + len - 4; // without CRC
	if (end > n)
		end = n;
	for (int i = 8; i + 4 <= end; i += 4)
	{
		int prog = (s[i] << 8) | s[i + 1];
		if (prog != 0)
		{
			c->pmt_pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
			return;
		}
	}
}

// Extracts TPE1 (artist) and TIT2 (title) from an ID3v2.3/2.4 tag and reports "Artist - Title"
static void meta_flush(ts_t *c)
{
	int n = c->meta_len;
	c->meta_len = 0;
	if (!hls_meta_sink || n < 19)
		return;
	const uint8_t *p = c->meta;
	// PES header: 00 00 01 bd len(2) flags(2) hdrlen
	if (p[0] || p[1] || p[2] != 1)
		return;
	int o = 9 + p[8];
	if (o + 10 > n || memcmp(p + o, "ID3", 3))
		return;
	int ver = p[o + 3];
	int tsz = (p[o + 6] << 21) | (p[o + 7] << 14) | (p[o + 8] << 7) | p[o + 9];
	int end = o + 10 + tsz;
	if (end > n)
		end = n;
	char artist[96] = "", title[128] = "";
	for (int i = o + 10; i + 11 <= end;)
	{
		const uint8_t *f = p + i;
		int fs = ver >= 4 ? ((f[4] << 21) | (f[5] << 14) | (f[6] << 7) | f[7]) : ((f[4] << 24) | (f[5] << 16) | (f[6] << 8) | f[7]);
		if (!f[0] || fs < 2 || i + 10 + fs > end)
			break;
		char *dst = NULL;
		size_t dsz = 0;
		if (!memcmp(f, "TIT2", 4))
			dst = title, dsz = sizeof(title);
		else if (!memcmp(f, "TPE1", 4))
			dst = artist, dsz = sizeof(artist);
		if (dst && (f[10] == 0 || f[10] == 3)) // ISO-8859-1 or UTF-8
		{
			size_t l = fs - 1;
			if (l >= dsz)
				l = dsz - 1;
			memcpy(dst, f + 11, l);
			dst[l] = 0;
		}
		i += 10 + fs;
	}
	if (!title[0])
		return;
	char out[256];
	if (artist[0])
		snprintf(out, sizeof(out), "%s - %s", artist, title);
	else
		snprintf(out, sizeof(out), "%s", title);
	hls_meta_sink(out);
}

static void parse_pmt(ts_t *c, const uint8_t *s, int n)
{
	if (n < 16 || s[0] != 0x02)
		return;
	int len = ((s[1] & 0x0F) << 8) | s[2];
	int end = 3 + len - 4;
	if (end > n)
		end = n;
	int i = 12 + (((s[10] & 0x0F) << 8) | s[11]);
	while (i + 5 <= end)
	{
		int type = s[i];
		int pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
		int es = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
		if (type == 0x0F && c->audio_pid < 0)
		{
			c->audio_pid = pid;
			ESP_LOGI(TAG, "audio pid %d (ADTS AAC)", pid);
		}
		else if (type == 0x15 && c->meta_pid < 0)
			c->meta_pid = pid;
		else if ((type == 0x03 || type == 0x04 || type == 0x11) && !c->unsupported_logged)
		{
			c->unsupported_logged = true;
			ESP_LOGW(TAG, "audio stream type 0x%02x not supported", type);
		}
		i += 5 + es;
	}
}

static int ts_packet(ts_t *c, const uint8_t *p)
{
	if (p[0] != 0x47 || (p[1] & 0x80))
		return 0;
	bool pusi = p[1] & 0x40;
	int pid = ((p[1] & 0x1F) << 8) | p[2];
	int afc = (p[3] >> 4) & 3;
	int off = 4;
	if (afc & 2)
		off += 1 + p[4];
	if (!(afc & 1) || off >= TS_PKT)
		return 0;
	const uint8_t *pl = p + off;
	int n = TS_PKT - off;

	if (pid == 0 || pid == c->pmt_pid)
	{
		if (!pusi)
			return 0; // sections are assumed to fit one packet
		int ptr = pl[0];
		if (1 + ptr >= n)
			return 0;
		if (pid == 0)
			parse_pat(c, pl + 1 + ptr, n - 1 - ptr);
		else
			parse_pmt(c, pl + 1 + ptr, n - 1 - ptr);
		return 0;
	}
	if (c->meta_pid >= 0 && pid == c->meta_pid)
	{
		if (pusi)
			meta_flush(c);
		if (n > 0 && c->meta_len + n <= (int)sizeof(c->meta))
		{
			memcpy(c->meta + c->meta_len, pl, n);
			c->meta_len += n;
		}
		return 0;
	}
	if (pid != c->audio_pid)
		return 0;

	int cc = p[3] & 0x0F;
	if (c->last_cc >= 0 && cc != c->last_cc && cc != ((c->last_cc + 1) & 0x0F))
		c->cc_err++;
	c->last_cc = cc;

	if (pusi)
	{
		if (n < 9 || pl[0] || pl[1] || pl[2] != 1)
		{
			c->pes_skip = 0;
			return 0;
		}
		c->pes_skip = 9 + pl[8];
	}
	if (c->pes_skip)
	{
		int s = c->pes_skip < n ? c->pes_skip : n;
		pl += s;
		n -= s;
		c->pes_skip -= s;
	}
	if (n > 0)
		return ts_out(c, pl, n);
	return 0;
}

static int ts_feed(void *vc, const uint8_t *d, size_t n)
{
	ts_t *c = vc;
	while (n)
	{
		if (c->fill == 0)
		{
			// resync on the 0x47 marker
			if (*d != 0x47)
			{
				d++;
				n--;
				continue;
			}
			if (n >= TS_PKT)
			{
				if (ts_packet(c, d) < 0)
					return -1;
				d += TS_PKT;
				n -= TS_PKT;
				continue;
			}
		}
		size_t k = TS_PKT - c->fill;
		if (k > n)
			k = n;
		memcpy(c->pkt + c->fill, d, k);
		c->fill += k;
		d += k;
		n -= k;
		if (c->fill == TS_PKT)
		{
			c->fill = 0;
			if (ts_packet(c, c->pkt) < 0)
				return -1;
		}
	}
	return 0;
}

static void *ts_create(hls_sink_t sink)
{
	ts_t *c = calloc(1, sizeof(ts_t));
	if (!c)
		return NULL;
	c->sink = sink;
	c->pmt_pid = -1;
	c->audio_pid = -1;
	c->meta_pid = -1;
	c->last_cc = -1;
	return c;
}

static int ts_feed_init(void *vc, const uint8_t *d, size_t n) { return 0; }

static void ts_segment_end(void *vc)
{
	ts_t *c = vc;
	if (c->olen)
		c->sink(c->out, c->olen);
	c->olen = 0;
	meta_flush(c);
	if (c->cc_err)
		ESP_LOGW(TAG, "%d continuity errors in segment (lost TS packets)", c->cc_err);
	c->cc_err = 0;
	c->last_cc = -1;
	c->fill = 0;
	c->pes_skip = 0;
}

static void ts_destroy(void *vc) { free(vc); }

const hls_demux_ops_t hls_ts_ops = {
	.name = "mpeg-ts",
	.content_type = KAUDIO_AAC,
	.create = ts_create,
	.feed_init = ts_feed_init,
	.feed = ts_feed,
	.segment_end = ts_segment_end,
	.destroy = ts_destroy,
};

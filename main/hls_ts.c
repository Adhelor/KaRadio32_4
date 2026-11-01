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
	bool unsupported_logged;
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
	if (pid != c->audio_pid)
		return 0;

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
	return c;
}

static int ts_feed_init(void *vc, const uint8_t *d, size_t n) { return 0; }

static void ts_segment_end(void *vc)
{
	ts_t *c = vc;
	if (c->olen)
		c->sink(c->out, c->olen);
	c->olen = 0;
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

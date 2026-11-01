/*
 * Streaming fragmented MP4 (CMAF) -> ADTS AAC demuxer for HLS.
 * Boxes moov/moof are buffered (small), mdat is streamed sample by sample.
 */
#include "hls.h"
#include "webclient.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

#define TAG "hls_fmp4"
#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define MAX_META_BOX 12288
#define MAX_FRAME 8184
#define MAX_SAMPLES 4096

enum
{
	ST_HDR,
	ST_COLLECT,
	ST_SKIP,
	ST_MDAT
};

typedef struct
{
	hls_sink_t sink;
	// audio configuration from moov
	bool have_cfg;
	uint8_t profile, freq, chan;
	uint32_t track_id;
	uint32_t trex_size;
	// box parser
	int st;
	uint8_t hdr[16];
	uint8_t hdr_have;
	uint32_t type;
	uint64_t remain;
	uint64_t seg_pos;
	uint64_t box_start;
	uint8_t *box;
	uint32_t box_have;
	// samples of the current fragment
	uint16_t *sizes;
	uint32_t nsamp, idx, fill;
	uint64_t data_start;
	bool data_start_set;
	uint8_t *frame;
} fmp4_t;

static inline uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

// Iterates the boxes of a buffer. Returns false at the end or on a malformed box.
static bool next_box(const uint8_t *buf, size_t len, size_t *off, uint32_t *type, const uint8_t **pl, size_t *pn)
{
	size_t o = *off;
	if (o + 8 > len)
		return false;
	uint64_t size = rd32(buf + o);
	size_t hlen = 8;
	*type = rd32(buf + o + 4);
	if (size == 1)
	{
		if (o + 16 > len)
			return false;
		size = rd64(buf + o + 8);
		hlen = 16;
	}
	else if (size == 0)
		size = len - o;
	if (size < hlen || size > len - o)
		return false;
	*pl = buf + o + hlen;
	*pn = (size_t)size - hlen;
	*off = o + (size_t)size;
	return true;
}

static bool find_box(const uint8_t *buf, size_t len, uint32_t want, const uint8_t **pl, size_t *pn)
{
	size_t off = 0;
	uint32_t t;
	while (next_box(buf, len, &off, &t, pl, pn))
		if (t == want)
			return true;
	return false;
}

static uint32_t getbits(const uint8_t *p, size_t n, size_t *bp, int k)
{
	uint32_t v = 0;
	while (k--)
	{
		size_t byte = *bp >> 3;
		uint32_t bit = (byte < n) ? (p[byte] >> (7 - (*bp & 7))) & 1 : 0;
		v = (v << 1) | bit;
		(*bp)++;
	}
	return v;
}

static int parse_asc(fmp4_t *c, const uint8_t *p, size_t n, unsigned fallback_chan)
{
	size_t bp = 0;
	uint32_t aot = getbits(p, n, &bp, 5);
	if (aot == 31)
		aot = 32 + getbits(p, n, &bp, 6);
	uint32_t fi = getbits(p, n, &bp, 4);
	if (fi == 15)
		return -1; // explicit sample rate has no ADTS index
	uint32_t ch = getbits(p, n, &bp, 4);
	if (aot == 5 || aot == 29)
	{ // explicit SBR/PS: ADTS carries the core object type and rate
		if (getbits(p, n, &bp, 4) == 15)
			getbits(p, n, &bp, 24);
		aot = getbits(p, n, &bp, 5);
		if (aot == 31)
			aot = 32 + getbits(p, n, &bp, 6);
	}
	if (ch == 0)
		ch = fallback_chan;
	if (aot < 1 || aot > 4 || fi > 12 || ch < 1 || ch > 7)
	{
		ESP_LOGE(TAG, "Unsupported AAC config aot=%u fi=%u ch=%u", (unsigned)aot, (unsigned)fi, (unsigned)ch);
		return -1;
	}
	c->profile = aot - 1;
	c->freq = fi;
	c->chan = ch;
	c->have_cfg = true;
	ESP_LOGI(TAG, "AAC aot=%u freqidx=%u ch=%u", (unsigned)aot, (unsigned)fi, (unsigned)ch);
	return 0;
}

static bool desc_len(const uint8_t *p, size_t n, size_t *i, size_t *len)
{
	size_t v = 0;
	for (int k = 0; k < 4; k++)
	{
		if (*i >= n)
			return false;
		uint8_t b = p[(*i)++];
		v = (v << 7) | (b & 0x7f);
		if (!(b & 0x80))
		{
			*len = v;
			return true;
		}
	}
	return false;
}

static int parse_esds(fmp4_t *c, const uint8_t *p, size_t n, unsigned fallback_chan)
{
	size_t i = 4, l; // version/flags
	if (n < 8 || p[i] != 3)
		return -1;
	i++;
	if (!desc_len(p, n, &i, &l) || i + 3 > n)
		return -1;
	uint8_t fl = p[i + 2];
	i += 3;
	if (fl & 0x80)
		i += 2;
	if (fl & 0x40)
	{
		if (i >= n)
			return -1;
		i += 1 + p[i];
	}
	if (fl & 0x20)
		i += 2;
	if (i >= n || p[i] != 4)
		return -1;
	i++;
	if (!desc_len(p, n, &i, &l))
		return -1;
	i += 13;
	if (i >= n || p[i] != 5)
		return -1;
	i++;
	if (!desc_len(p, n, &i, &l) || i + l > n)
		return -1;
	return parse_asc(c, p + i, l, fallback_chan);
}

static int parse_trak(fmp4_t *c, const uint8_t *trak, size_t n)
{
	const uint8_t *pl, *tmp;
	size_t pn, tn;
	if (!find_box(trak, n, FOURCC('t', 'k', 'h', 'd'), &tmp, &tn) || tn < 20)
		return -1;
	uint32_t tid = (tmp[0] == 1) ? rd32(tmp + 20) : rd32(tmp + 12);
	pl = trak;
	pn = n;
	static const uint32_t path[] = {FOURCC('m', 'd', 'i', 'a'), FOURCC('m', 'i', 'n', 'f'), FOURCC('s', 't', 'b', 'l'), FOURCC('s', 't', 's', 'd')};
	for (int d = 0; d < 4; d++)
		if (!find_box(pl, pn, path[d], &pl, &pn))
			return -1;
	if (pn < 8)
		return -1;
	// first sample entry must be mp4a
	size_t off = 0;
	uint32_t t;
	const uint8_t *e;
	size_t en;
	const uint8_t *entries = pl + 8;
	size_t entries_n = pn - 8;
	if (!next_box(entries, entries_n, &off, &t, &e, &en) || t != FOURCC('m', 'p', '4', 'a') || en < 28)
		return -1;
	unsigned chan = (e[16] << 8) | e[17];
	if (!find_box(e + 28, en - 28, FOURCC('e', 's', 'd', 's'), &tmp, &tn))
		return -1;
	if (parse_esds(c, tmp, tn, chan) < 0)
		return -1;
	c->track_id = tid;
	return 0;
}

static int parse_moov(fmp4_t *c, const uint8_t *moov, size_t n)
{
	size_t off = 0;
	uint32_t t;
	const uint8_t *pl;
	size_t pn;
	c->have_cfg = false;
	c->trex_size = 0;
	while (next_box(moov, n, &off, &t, &pl, &pn))
	{
		if (t == FOURCC('t', 'r', 'a', 'k') && !c->have_cfg)
			parse_trak(c, pl, pn);
		else if (t == FOURCC('m', 'v', 'e', 'x'))
		{
			const uint8_t *trex;
			size_t tn, o2 = 0;
			uint32_t t2;
			while (next_box(pl, pn, &o2, &t2, &trex, &tn))
				if (t2 == FOURCC('t', 'r', 'e', 'x') && tn >= 24 && (!c->track_id || rd32(trex + 4) == c->track_id))
					c->trex_size = rd32(trex + 16);
		}
	}
	if (!c->have_cfg)
	{
		ESP_LOGE(TAG, "No AAC track in moov");
		return -1;
	}
	return 0;
}

static void parse_traf(fmp4_t *c, const uint8_t *traf, size_t n, uint64_t moof_start)
{
	size_t off = 0;
	uint32_t t;
	const uint8_t *p;
	size_t pn;
	uint32_t dsize = c->trex_size;
	bool mine = true, has_base = false;
	uint64_t base = moof_start;
	while (next_box(traf, n, &off, &t, &p, &pn))
	{
		if (t == FOURCC('t', 'f', 'h', 'd') && pn >= 8)
		{
			uint32_t fl = rd32(p) & 0xFFFFFF;
			mine = (!c->track_id || rd32(p + 4) == c->track_id);
			size_t o = 8;
			if ((fl & 1) && o + 8 <= pn)
			{
				base = rd64(p + o);
				has_base = true;
				o += 8;
			}
			if (fl & 2)
				o += 4;
			if (fl & 8)
				o += 4;
			if ((fl & 0x10) && o + 4 <= pn)
			{
				dsize = rd32(p + o);
				o += 4;
			}
		}
		else if (t == FOURCC('t', 'r', 'u', 'n') && mine && pn >= 8)
		{
			uint32_t fl = rd32(p) & 0xFFFFFF;
			uint32_t cnt = rd32(p + 4);
			size_t o = 8;
			if (fl & 1)
			{
				if (o + 4 > pn)
					continue;
				if (!c->data_start_set)
				{
					c->data_start = base + (int64_t)(int32_t)rd32(p + o);
					c->data_start_set = true;
				}
				o += 4;
			}
			if (fl & 4)
				o += 4;
			(void)has_base;
			for (uint32_t i = 0; i < cnt && c->nsamp < MAX_SAMPLES; i++)
			{
				uint32_t sz = dsize;
				if (fl & 0x100)
					o += 4;
				if (fl & 0x200)
				{
					if (o + 4 > pn)
						break;
					sz = rd32(p + o);
					o += 4;
				}
				if (fl & 0x400)
					o += 4;
				if (fl & 0x800)
					o += 4;
				if (o > pn)
					break;
				c->sizes[c->nsamp++] = sz > 0xFFFF ? 0xFFFF : sz;
			}
		}
	}
}

static void parse_moof(fmp4_t *c, const uint8_t *moof, size_t n, uint64_t moof_start)
{
	size_t off = 0;
	uint32_t t;
	const uint8_t *pl;
	size_t pn;
	c->nsamp = c->idx = c->fill = 0;
	c->data_start_set = false;
	while (next_box(moof, n, &off, &t, &pl, &pn))
		if (t == FOURCC('t', 'r', 'a', 'f'))
			parse_traf(c, pl, pn, moof_start);
}

static void write_adts(const fmp4_t *c, uint8_t *h, uint32_t payload)
{
	uint32_t len = payload + 7;
	h[0] = 0xFF;
	h[1] = 0xF1;
	h[2] = (c->profile << 6) | (c->freq << 2) | ((c->chan >> 2) & 1);
	h[3] = ((c->chan & 3) << 6) | ((len >> 11) & 3);
	h[4] = (len >> 3) & 0xFF;
	h[5] = ((len & 7) << 5) | 0x1F;
	h[6] = 0xFC;
}

static int mdat_chunk(fmp4_t *c, const uint8_t *d, size_t n)
{
	uint64_t pos = c->seg_pos, end = pos + n;
	while (pos < end)
	{
		if (c->idx >= c->nsamp)
			break;
		if (pos < c->data_start)
		{
			uint64_t s = c->data_start - pos;
			if (s > end - pos)
				s = end - pos;
			pos += s;
			d += s;
			continue;
		}
		uint32_t sz = c->sizes[c->idx];
		uint32_t t = sz - c->fill;
		if (t > end - pos)
			t = end - pos;
		if (sz <= MAX_FRAME)
			memcpy(c->frame + 7 + c->fill, d, t);
		c->fill += t;
		d += t;
		pos += t;
		if (c->fill == sz)
		{
			if (sz > 0 && sz <= MAX_FRAME)
			{
				write_adts(c, c->frame, sz);
				if (c->sink(c->frame, sz + 7) < 0)
					return -1;
			}
			c->idx++;
			c->fill = 0;
		}
	}
	return 0;
}

static void fmp4_reset(fmp4_t *c)
{
	c->st = ST_HDR;
	c->hdr_have = 0;
	c->seg_pos = 0;
	c->nsamp = c->idx = c->fill = 0;
	c->data_start_set = false;
}

static int fmp4_feed(void *vc, const uint8_t *d, size_t n)
{
	fmp4_t *c = vc;
	while (n)
	{
		switch (c->st)
		{
		case ST_HDR:
		{
			while (n && (c->hdr_have < 8 || (c->hdr_have < 16 && rd32(c->hdr) == 1)))
			{
				c->hdr[c->hdr_have++] = *d++;
				n--;
				c->seg_pos++;
			}
			if (c->hdr_have < 8 || (c->hdr_have < 16 && rd32(c->hdr) == 1))
				break;
			uint64_t size = rd32(c->hdr);
			uint32_t hlen = 8;
			c->type = rd32(c->hdr + 4);
			if (size == 1)
			{
				size = rd64(c->hdr + 8);
				hlen = 16;
			}
			c->box_start = c->seg_pos - hlen;
			c->hdr_have = 0;
			if (size == 0)
				c->remain = UINT64_MAX;
			else if (size < hlen)
				return -1;
			else
				c->remain = size - hlen;
			if ((c->type == FOURCC('m', 'o', 'o', 'v') || c->type == FOURCC('m', 'o', 'o', 'f')) && c->remain <= MAX_META_BOX)
			{
				c->st = ST_COLLECT;
				c->box_have = 0;
			}
			else if (c->type == FOURCC('m', 'd', 'a', 't'))
			{
				if (!c->data_start_set)
				{
					c->data_start = c->seg_pos;
					c->data_start_set = true;
				}
				c->st = ST_MDAT;
			}
			else
				c->st = ST_SKIP;
			if (c->remain == 0)
				c->st = ST_HDR; // empty box
			break;
		}
		case ST_COLLECT:
		{
			size_t t = (c->remain < n) ? (size_t)c->remain : n;
			memcpy(c->box + c->box_have, d, t);
			c->box_have += t;
			c->remain -= t;
			c->seg_pos += t;
			d += t;
			n -= t;
			if (c->remain == 0)
			{
				c->st = ST_HDR;
				if (c->type == FOURCC('m', 'o', 'o', 'v'))
				{
					if (parse_moov(c, c->box, c->box_have) < 0)
						return -1;
				}
				else
					parse_moof(c, c->box, c->box_have, c->box_start);
			}
			break;
		}
		case ST_SKIP:
		{
			size_t t = (c->remain < n) ? (size_t)c->remain : n;
			if (c->remain != UINT64_MAX)
				c->remain -= t;
			c->seg_pos += t;
			d += t;
			n -= t;
			if (c->remain == 0)
				c->st = ST_HDR;
			break;
		}
		case ST_MDAT:
		{
			size_t t = (c->remain < n) ? (size_t)c->remain : n;
			if (!c->have_cfg)
			{
				ESP_LOGE(TAG, "Media data without AAC config");
				return -1;
			}
			if (mdat_chunk(c, d, t) < 0)
				return -1;
			if (c->remain != UINT64_MAX)
				c->remain -= t;
			c->seg_pos += t;
			d += t;
			n -= t;
			if (c->remain == 0)
				c->st = ST_HDR;
			break;
		}
		}
	}
	return 0;
}

static void *fmp4_create(hls_sink_t sink)
{
	fmp4_t *c = calloc(1, sizeof(fmp4_t));
	if (!c)
		return NULL;
	c->sink = sink;
	c->box = malloc(MAX_META_BOX);
	c->sizes = malloc(MAX_SAMPLES * sizeof(uint16_t));
	c->frame = malloc(MAX_FRAME + 7);
	if (!c->box || !c->sizes || !c->frame)
	{
		free(c->box);
		free(c->sizes);
		free(c->frame);
		free(c);
		return NULL;
	}
	fmp4_reset(c);
	return c;
}

static int fmp4_feed_init(void *vc, const uint8_t *d, size_t n)
{
	fmp4_reset(vc);
	int r = fmp4_feed(vc, d, n);
	fmp4_reset(vc);
	return r;
}

static void fmp4_segment_end(void *vc) { fmp4_reset(vc); }

static void fmp4_destroy(void *vc)
{
	fmp4_t *c = vc;
	if (!c)
		return;
	free(c->box);
	free(c->sizes);
	free(c->frame);
	free(c);
}

const hls_demux_ops_t hls_fmp4_ops = {
	.name = "fmp4",
	.content_type = KAUDIO_AAC,
	.create = fmp4_create,
	.feed_init = fmp4_feed_init,
	.feed = fmp4_feed,
	.segment_end = fmp4_segment_end,
	.destroy = fmp4_destroy,
};

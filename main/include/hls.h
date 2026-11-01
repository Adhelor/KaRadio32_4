/*
 * HLS (m3u8) support: manifest handling, segment download and container demuxing.
 */
#ifndef __HLS_H__
#define __HLS_H__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wolfssl/ssl.h"

/* Receives the elementary stream produced by a demuxer. Returns < 0 to abort. */
typedef int (*hls_sink_t)(const uint8_t *data, size_t len);

/* Container abstraction: fMP4 today, ogg / mpeg-ts can be added with the same interface. */
typedef struct
{
	const char *name;
	int content_type;										 // contentType_t of the demuxer output
	void *(*create)(hls_sink_t sink);
	int (*feed_init)(void *ctx, const uint8_t *d, size_t n); // EXT-X-MAP data
	int (*feed)(void *ctx, const uint8_t *d, size_t n);		 // media segment data
	void (*segment_end)(void *ctx);							 // end of one media segment
	void (*destroy)(void *ctx);
} hls_demux_ops_t;

extern const hls_demux_ops_t hls_fmp4_ops;
extern const hls_demux_ops_t hls_ts_ops;

typedef struct
{
	WOLFSSL_CTX *wctx;
	const char *user_agent;
	bool (*should_stop)(void);
	void (*on_start)(int content_type); // before the first audio byte is sent
	void (*on_playing)(void);			// after the first segment was delivered
	void (*on_meta)(const char *title); // timed metadata (ID3 in MPEG-TS), "Artist - Title"
} hls_cfg_t;

extern void (*hls_meta_sink)(const char *title);

typedef enum
{
	HLS_STOPPED = 0, // stop requested by the user
	HLS_ENDED,		 // VOD finished
	HLS_ERROR
} hls_result_t;

/* Heuristic: does the text look like an HLS manifest (not a plain m3u list)? */
bool hls_is_manifest(const char *text);

/* Blocking: plays the stream until stop, end or unrecoverable error. */
hls_result_t hls_play(const hls_cfg_t *cfg, const char *url);

#endif

/*
 * audio_player.h
 *
 *  Created on: 12.03.2017
 *      Author: michaelboeckling
 */

#ifndef INCLUDE_AUDIO_PLAYER_H_
#define INCLUDE_AUDIO_PLAYER_H_

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include "common_component.h"
#include "audio_renderer.h"

int audio_stream_consumer(const char *recv_buf, ssize_t bytes_read);


typedef enum {
    CMD_NONE, CMD_START, CMD_STOP, CMD_PAUSE, CMD_RESUME
} player_command_t;

typedef enum {
    BUF_PREF_FAST, BUF_PREF_SAFE
} buffer_pref_t;

typedef enum
{
    MIME_UNKNOWN = 1, OCTET_STREAM, AUDIO_AAC, AUDIO_MP4, AUDIO_MPEG, AUDIO_OGG, AUDIO_BT_PCM, AUDIO_BT_AAC, AUDIO_UNKNOWN, AUDIO_BT_LDAC, AUDIO_BT_APTX
} content_type_t;

typedef enum
{
    AUDIO_STREAM_CODEC_NONE,
    AUDIO_STREAM_CODEC_MP3,
    AUDIO_STREAM_CODEC_AAC,
    AUDIO_STREAM_CODEC_SBC,
    AUDIO_STREAM_CODEC_LDAC,
    AUDIO_STREAM_CODEC_APTX
} audio_stream_codec_t;

typedef struct
{
    audio_stream_codec_t codec;
    uint32_t sample_rate;
    uint32_t bitrate;
    bool variable_bitrate;
} audio_stream_info_t;

void audio_player_set_stream_info(audio_stream_codec_t codec, uint32_t sample_rate, uint32_t bitrate);
void audio_player_set_stream_vbr(audio_stream_codec_t codec, uint32_t sample_rate);
void audio_player_reset_stream_info(audio_stream_codec_t codec, uint32_t sample_rate);
void audio_player_get_stream_info(audio_stream_info_t *info);

typedef struct {
    content_type_t content_type;
    bool eof;
     int transport_type; // <-- Add this field (0 = TT_MP4_RAW, 2 = TT_MP4_ADTS)
} media_stream_t;

typedef struct {
    player_command_t command;
    player_command_t decoder_command;
    component_status_t decoder_status;
    buffer_pref_t buffer_pref;
    media_stream_t *media_stream;
    TaskHandle_t decoder_task_handle;
} player_t;


// Declare player_status as extern
extern component_status_t player_status;

// Declare the get_player_status function
component_status_t get_player_status(void);

void audio_player_init(player_t *player_config);
void audio_player_start();
void audio_player_stop();
int start_decoder_task(player_t *player);
//void audio_player_destroy();


#endif /* INCLUDE_AUDIO_PLAYER_H_ */

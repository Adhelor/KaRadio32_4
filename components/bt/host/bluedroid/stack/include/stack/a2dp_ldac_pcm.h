#ifndef A2DP_LDAC_PCM_H
#define A2DP_LDAC_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool a2dp_ldac_decoder_init_pcm(void);
bool a2dp_ldac_decoder_decode_payload(const uint8_t *payload, size_t payload_len,
                                      uint8_t *pcm, size_t pcm_capacity, size_t *pcm_len,
                                      uint32_t *sample_rate, uint8_t *channel_count,
                                      uint32_t *bitrate);

#endif
/*
 * fdk_aac_decoder.h
 *
 *  Created on: 08.05.2017
 *      Author: michaelboeckling
 */

#ifndef _INCLUDE_FDK_AAC_DECODER_H_
#define _INCLUDE_FDK_AAC_DECODER_H_
#include "common_buffer.h"
buffer_t *get_aac_in_buf(void);
buffer_t *get_aac_pcm_buf(void);
void fdkaac_decoder_task(void *pvParameters);

#endif /* _INCLUDE_FDK_AAC_DECODER_H_ */

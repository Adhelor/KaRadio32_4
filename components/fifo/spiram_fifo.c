/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: user_main.c
 *
 * Description: Routines to use a SPI RAM chip as a big FIFO buffer. Multi-
 * thread-aware: the reading and writing can happen in different threads and
 * will block if the fifo is empty and full, respectively.
 *
 * Modification history:
 *     2015/06/02, v1.0 File created.
 *******************************************************************************/
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
 #include "esp_system.h"
#include "string.h"
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "spiram_fifo.h"
// #include "spiram.h"
// #include "esp_heap_trace.h"
#include "esp_heap_caps.h"

extern void *kmalloc(size_t memorySize);
extern void *kcalloc(size_t elementCount, size_t elementSize);

#define FAKE_SPI_BUFF

// #define SPIREADSIZE 1024
#define SPIREADSIZE 1024

static unsigned fifoRpos;
static unsigned fifoWpos;
static unsigned fifoFill;
// static xSemaphoreHandle semCanRead;
// static xSemaphoreHandle semCanWrite;
static xSemaphoreHandle mux = NULL;
static long fifoOvfCnt, fifoUdrCnt;

// Low watermark where we restart the reader thread.
// #define FIFO_LOWMARK (16*1024)
#define FIFO_LOWMARK (512)

#ifdef FAKE_SPI_BUFF
// Re-define a bunch of things so we use the internal buffer
#undef SPIRAMSIZE
static char *fakespiram;

#define spiRamWrite(pos, buf, n) memcpy(&fakespiram[pos], buf, n)
#define spiRamRead(pos, buf, n) memcpy(buf, &fakespiram[pos], n)
#endif

static unsigned SPIRAMSIZE = (64 * 1024); //30

void setSPIRAMSIZE(unsigned size)
{
	SPIRAMSIZE = size;
}
unsigned getSPIRAMSIZE()
{
	return SPIRAMSIZE;
}

// Initialize the FIFO
void *spiRamFifoInit()
{
	fakespiram = kmalloc(SPIRAMSIZE);
	fifoRpos = 0;
	fifoWpos = 0;
	fifoFill = 0;
	fifoOvfCnt = 0;
	fifoUdrCnt = 0;
	if (mux == NULL)
		mux = xSemaphoreCreateMutex();
	return (fakespiram);
}

static volatile unsigned fifoEpoch = 0;

unsigned spiRamFifoEpoch()
{
	return fifoEpoch;
}

void spiRamFifoReset()
{
	xSemaphoreTake(mux, portMAX_DELAY);
	fifoEpoch++;
	fifoRpos = 0;
	fifoWpos = 0;
	fifoFill = 0;
	fifoOvfCnt = 0;
	fifoUdrCnt = 0;
	memset(fakespiram, 0, SPIRAMSIZE);
	xSemaphoreGive(mux);
}

// Read bytes from the FIFO
void spiRamFifoRead(char *buff, unsigned len)
{
	while (len > 0)
	{
		//		heap_caps_check_integrity_all(true) ;
		unsigned n = len;
		if (xSemaphoreTake(mux, portMAX_DELAY))
		{
			if (n > SPIREADSIZE)
				n = SPIREADSIZE; // don't read more than SPIREADSIZE
			if (n > (SPIRAMSIZE - fifoRpos))
				n = SPIRAMSIZE - fifoRpos; // don't read past end of buffer
			if (fifoFill < n)
			{
				// Drat, not enough data in FIFO. Wait till there's some written and try again.
				fifoUdrCnt++;
				xSemaphoreGive(mux);
				vTaskDelay(pdMS_TO_TICKS(20));
			}
			else
			{
				// Read the data.
				spiRamRead(fifoRpos, buff, n);
				buff += n;
				len -= n;
				fifoFill -= n;
				fifoRpos += n;
				if (fifoRpos >= SPIRAMSIZE)
					fifoRpos = 0;
				xSemaphoreGive(mux);
			}
		}
		else
		{
			printf("spiRamFifoRead MUX take failed\n");
		}
	}
}

// Read exactly len bytes if available; return immediately when the FIFO is short.
unsigned spiRamFifoTryRead(char *buff, unsigned len)
{
	if (buff == NULL || len == 0 || !xSemaphoreTake(mux, portMAX_DELAY))
		return 0;

	if (fifoFill < len)
	{
		xSemaphoreGive(mux);
		return 0;
	}

	unsigned remaining = len;
	while (remaining > 0)
	{
		unsigned n = remaining;
		if (n > SPIREADSIZE)
			n = SPIREADSIZE;
		if (n > SPIRAMSIZE - fifoRpos)
			n = SPIRAMSIZE - fifoRpos;
		spiRamRead(fifoRpos, buff, n);
		buff += n;
		remaining -= n;
		fifoRpos += n;
		if (fifoRpos >= SPIRAMSIZE)
			fifoRpos = 0;
	}
	fifoFill -= len;
	xSemaphoreGive(mux);
	return len;
}

// Write bytes to the FIFO
void spiRamFifoWrite(const char *buff, unsigned buffLen)
{
	unsigned startEpoch = fifoEpoch;
	while (buffLen > 0)
	{
		unsigned n = buffLen;
		if (xSemaphoreTake(mux, portMAX_DELAY))
		{
			// flushed mid-write: drop the rest so no partial packet survives
			if (startEpoch != fifoEpoch)
			{
				xSemaphoreGive(mux);
				return;
			}
			// don't read more than SPIREADSIZE
			if (n > SPIREADSIZE)
				n = SPIREADSIZE;

			// don't read past end of buffer
			if (n > (SPIRAMSIZE - fifoWpos))
			{
				n = SPIRAMSIZE - fifoWpos;
			}

			if ((SPIRAMSIZE - fifoFill) < n)
			{
				//  Drat, not enough free room in FIFO. Wait till there's some read and try again.
				fifoOvfCnt++;
				xSemaphoreGive(mux);
				vTaskDelay(pdMS_TO_TICKS(20));
			}
			else
			{
				// Write the data.
				spiRamWrite(fifoWpos, buff, n);
				buff += n;
				buffLen -= n;
				fifoFill += n;
				fifoWpos += n;
				if (fifoWpos >= SPIRAMSIZE)
					fifoWpos = 0;
				xSemaphoreGive(mux);
			}
		}
		else
		{
			printf(" spiRamFifoWrite MUX take failed\n");
		}
	}
}

// Get amount of bytes in use
unsigned spiRamFifoFill()
{
	xSemaphoreTake(mux, portMAX_DELAY);
	unsigned ret = fifoFill;
	xSemaphoreGive(mux);
	return ret;
}

unsigned spiRamFifoFree()
{
	return (SPIRAMSIZE - spiRamFifoFill());
}

void spiRamFifoDestroy()
{

	if (fakespiram)
		free(fakespiram);
}
unsigned spiRamFifoLen()
{
	return SPIRAMSIZE;
}

long spiRamGetOverrunCt()
{
	xSemaphoreTake(mux, portMAX_DELAY);
	long ret = fifoOvfCnt;
	xSemaphoreGive(mux);
	return ret;
}

long spiRamGetUnderrunCt()
{
	xSemaphoreTake(mux, portMAX_DELAY);
	long ret = fifoUdrCnt;
	xSemaphoreGive(mux);
	return ret;
}
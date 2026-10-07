#ifndef _SPIRAM_FIFO_H_
#define _SPIRAM_FIFO_H_

// #define SPIRAMSIZE (30*1024) //for a 23LC1024 chip
void*  spiRamFifoInit();
void  spiRamFifoRead(char *buff, unsigned len);
void  spiRamFifoWrite(const char *buff, unsigned len);
unsigned  spiRamFifoFill();
unsigned  spiRamFifoFree();
long  spiRamGetOverrunCt();
long  spiRamGetUnderrunCt();
void setSPIRAMSIZE(unsigned size);
unsigned getSPIRAMSIZE();

void spiRamFifoReset();
/* incremented by every spiRamFifoReset(); lets consumers detect a flush */
unsigned spiRamFifoEpoch();
void spiRamFifoDestroy();
unsigned spiRamFifoLen();

#endif

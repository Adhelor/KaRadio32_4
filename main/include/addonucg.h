#include "sdkconfig.h"
#if defined(CONFIG_DISPLAY_TYPE_COLOR)
#define COLOR
#elif defined(CONFIG_DISPLAY_TYPE_ALL)
#define COLOR
#else 
#ifdef COLOR
#undef COLOR
#endif
#endif
#if defined COLOR
/******************************************************************************
 * 
 * Copyright 2018 karawin (http://www.karawin.fr)
 *
*******************************************************************************/
#ifndef ADDONUCG_H_
#define ADDONUCG_H_
#define UCG_HEADER_TITLE 0
#define UCG_HEADER_DATE 1
#define UCG_HEADER_BT 2
#define UCG_HEADER_MAIN 3

void setfont(int size);
void playingUcg();
void namesetUcg(char* ici);
void statusUcg(const char* label);
void icy0Ucg(char* ici);
void icy4Ucg(char* ici);
void metaUcg(char* ici);
char* getNameNumUcg();
void scrollUcg();
void drawFrameUcg(uint8_t mTscreen);
void drawTTitleUcg(char* ttitle);
void drawNumberUcg(uint8_t mTscreen,char* irStr);
void drawStationUcg(uint8_t mTscreen,char* snum,char* ddot);
//void drawVolumeUcg(uint8_t mTscreen,char* aVolume);
void drawVolumeUcg(uint8_t mTscreen);
void prepareScreenTransitionUcg(const char *title, uint8_t header_kind, unsigned timein);
void finishScreenTransitionUcg(void);
void drawTimeUcg(uint8_t mTscreen,unsigned timein);
void lcd_initUcg(uint8_t* lcd_type);
void setVolumeUcg(uint16_t vol);
void drawLinesUcg();


#endif /* ADDONUCG_H_ */
#endif
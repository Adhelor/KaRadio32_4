/******************************************************************************
 *
 * LVGL Display Adapter Header
 * Copyright 2025 karawin (http://www.karawin.fr)
 *
 * Unified display interface for all display types using LVGL
 * Replaces addonu8g2.h and addonucg.h
 *
 ******************************************************************************/

#ifndef __ADDONLVGL_H__
#define __ADDONLVGL_H__
#if defined(CONFIG_DISPLAY_TYPE_LVGL)
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif
// I2C definitions (previously in u8g2_esp32_hal.h)
#define I2C_MASTER_NUM I2C_NUM_0           //  I2C port number for master dev
#define I2C_MASTER_TX_BUF_DISABLE   0      //  I2C master do not need buffer
#define I2C_MASTER_RX_BUF_DISABLE   0      //  I2C master do not need buffer
#define I2C_MASTER_FREQ_HZ          90000  //  I2C master clock frequency
#define ACK_CHECK_EN   0x1                 //  I2C master will check ack from slave
#define ACK_CHECK_DIS  0x0                 //  I2C master will not check ack from slave

/* ============================================================================
 * PUBLIC API - Display Functions
 * ============================================================================ */

/**
 * Initialize LVGL display
 * @param lcd_type Pointer to LCD type identifier
 */
void lcd_init_lvgl(uint8_t *lcd_type);

/**
 * Get current station name number
 * @return Pointer to station number string
 */
char *getNameNum(void);

/**
 * Set volume level
 * @param vol Volume value (0-255)
 */
void setVolume(uint16_t vol);

/**
 * Clear all display buffers and reset line indexes
 */
void clearAll(void);

/**
 * Clear title and lines starting from specified line
 * @param froml Starting line number
 */
void cleartitle(uint8_t froml);

/**
 * Mark line for redraw and reset scroll index
 * @param i Line number
 */
void markDrawReset(int i);

/**
 * Scroll display lines with word boundary detection
 */
void scroll(void);

/**
 * Draw main frame with all marked lines
 * @param mTscreen Display mode (MTNEW, MTREFRESH, MTNODISPLAY)
 */
void drawFrame(uint8_t mTscreen);

/**
 * Draw title in transient display mode
 * @param ttitle Title string
 */
void drawTTitle(char *ttitle);

/**
 * Draw number entered from IR remote
 * @param mTscreen Display mode
 * @param irStr Number string from IR
 */
void drawNumber(uint8_t mTscreen, char *irStr);

/**
 * Draw station selection screen
 * @param mTscreen Display mode
 * @param snum Station number
 * @param ddot Station name/description
 */
void drawStation(uint8_t mTscreen, char *snum, char *ddot);

/**
 * Draw volume display
 * @param mTscreen Display mode
 */
void drawVolume(uint8_t mTscreen);

/**
 * Draw time display
 * @param mTscreen Display mode
 * @param timein Time in seconds
 */
void drawTime(uint8_t mTscreen, unsigned timein);

/**
 * Draw separator line
 * @param from Label or description
 */
void separator(char *from);

/**
 * Draw metadata (ICY)
 * @param ici ICY metadata string
 */
void meta(char *ici);

/**
 * Draw ICY 4 metadata
 * @param ici ICY 4 data
 */
void icy4(char *ici);

/**
 * Draw ICY 0 metadata (genre)
 * @param ici Genre string
 */
void icy0(char *ici);

/**
 * Display status message
 * @param label Status label
 */
void status(const char *label);

/**
 * Set station name
 * @param ici Station name string
 */
void nameset(char *ici);

/**
 * Show "playing" status
 */
void playing(void);

/**
 * Display task (minimal in LVGL, driver handles most rendering)
 * @param pvParams Task parameters
 */
void task_lcd(void *pvParams);

/**
 * Addon service task
 * @param pvParams Task parameters
 */
void task_addon(void *pvParams);

/**
 * Get display width
 * @return Width in pixels
 */
uint16_t GetWidth(void);

/**
 * Get display height
 * @return Height in pixels
 */
uint16_t GetHeight(void);

/**
 * Set backlight level
 * @param val Brightness level (0-100)
 */
void setBlv(int val);

/**
 * Get backlight level
 * @return Current brightness level
 */
int getBlv(void);

/**
 * Get battery percentage
 * @return Battery percentage, -1 if not available
 */
int getBatPercent(void);

/* ============================================================================
 * TOUCH INPUT FUNCTIONS (XPT2046 via LVGL)
 * ============================================================================ */

/**
 * Initialize XPT2046 touch controller via LVGL driver
 * Must be called after lcd_init_lvgl() if touch is available
 * Registers touch device with LVGL input system
 */
void lvgl_touch_init(void);

/**
 * Disable touch input (typically during sleep mode)
 * Reduces power consumption and prevents accidental touches
 */
void lvgl_touch_disable(void);

/**
 * Enable touch input after wake from sleep or standby
 * Restores touch functionality and event delivery
 */
void lvgl_touch_enable(void);

/**
 * Get current LVGL touch input device
 * @return Pointer to lv_indev_t (input device) or NULL if not available
 */
lv_indev_t *lvgl_get_touch_device(void);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_DISPLAY_TYPE_LVGL
#endif  // __ADDONLVGL_H__

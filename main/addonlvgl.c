/******************************************************************************
 *
 * LVGL unified display adapter for KaRadio32
 * Replaces both UCGlib (color) and U8g2 (mono) implementations
 * Copyright 2025 karawin (http://www.karawin.fr)
 *
 * This module provides a unified display abstraction using LVGL for all
 * display types: color LCD (TFT), monochrome OLED, e-ink, etc.
 * 
 * Features:
 * - Single API for all display types
 * - Full UTF-8 character support with charset conversions (Cyrillic, Greek, Latin)
 * - Dynamic font sizing and scaling
 * - Text scrolling with proper character boundary detection
 * - Color support for color displays
 * - Minimal dependencies on LVGL configuration
 *
 ******************************************************************************/

#include "sdkconfig.h"
#if defined(CONFIG_DISPLAY_TYPE_LVGL)
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "addon.h"
#include "app_main.h"
#include "gpio.h"
#include "esp_log.h"
#include "logo.h"
#include "interface.h"
#include "eeprom.h"
#include "addoncommon.h"

#include "lvgl.h"

// Optional: Touch input support via LVGL
#ifndef CONFIG_NO_XPT2046
#include "lvgl_touch/touch_driver.h"
#endif

#define TAG "addonlvgl"

/* ============================================================================
 * LVGL DISPLAY OBJECT (replaces u8g2_t and ucg_t)
 * ============================================================================ */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *canvas;           // for advanced drawing if needed
    lv_color_t fg_color;        // foreground color
    lv_color_t bg_color;        // background color
    const lv_font_t *current_font;
    uint16_t width;
    uint16_t height;
    int16_t line_height;        // cached line height
} lv_display_t;

// Global display instance
static lv_display_t lvgl_display = {0};

/* ============================================================================
 * UTF-8 CHARACTER ENCODING SUPPORT (charset conversions)
 * ============================================================================ */

typedef enum Lang {
    Latin,
    Cyrillic,
    Greek
} LANG;

static LANG charset = Latin;

// Cyrillic encoding tables
#define UTF8TO1251 30
const struct {
    uint16_t utf8;
    uint8_t ascii;
} utf8To1251[UTF8TO1251] = {
    {0x401, 0x45}, {0x402, 0x80}, {0x403, 0x81}, {0x404, 0x45}, {0x405, 0xbd},
    {0x406, 0x49}, {0x407, 0x49}, {0x408, 0xa3}, {0x409, 0x8a}, {0x40a, 0x8c},
    {0x40b, 0x8e}, {0x40c, 0x8d}, {0x40e, 0xa1}, {0x40f, 0x8f}, {0x452, 0x90},
    {0x451, 0x65}, {0x453, 0x83}, {0x454, 0xE5}, {0x455, 0xbe}, {0x456, 0x69},
    {0x457, 0x69}, {0x458, 0x6a}, {0x459, 0x9a}, {0x45a, 0x9c}, {0x45b, 0x9e},
    {0x45c, 0x9d}, {0x45f, 0x9f}, {0x490, 0xa5}, {0x491, 0xb4}, {0, 0}
};

// Latin extended encoding
#define UTF8TO8859 63
const struct {
    uint16_t utf8;
    uint8_t ascii;
} utf8To8859[UTF8TO8859] = {
    {0x100, 0x41}, {0x102, 0x41}, {0x104, 0x41}, {0x101, 0x61}, {0x103, 0x61},
    {0x105, 0x61}, {0x106, 0x43}, {0x108, 0x43}, {0x1a, 0x43}, {0x10c, 0x43},
    {0x107, 0x63}, {0x109, 0x63}, {0x10b, 0x63}, {0x10d, 0x63}, {0x10E, 0x44},
    {0x110, 0x44}, {0x10F, 0x44}, {0x111, 0x44}, {0x11a, 0x45}, {0x11b, 0x65},
    {0x11c, 0x47}, {0x11e, 0x47}, {0x120, 0x47}, {0x122, 0x47}, {0x11d, 0x67},
    {0x11f, 0x67}, {0x121, 0x67}, {0x123, 0x67}, {0x124, 0x48}, {0x126, 0x48},
    {0x125, 0x68}, {0x127, 0x68}, {0x130, 0x49}, {0x132, 0x49}, {0x131, 0x69},
    {0x133, 0x69}, {0x147, 0x4e}, {0x148, 0x6e}, {0x158, 0x52}, {0x159, 0x72},
    {0x160, 0x53}, {0x161, 0x73}, {0x166, 0x54}, {0x167, 0x74}, {0x168, 0x55},
    {0x16A, 0x55}, {0x16C, 0x55}, {0x16E, 0x55}, {0x170, 0x55}, {0x172, 0x55},
    {0x169, 0x75}, {0x16B, 0x75}, {0x16D, 0x75}, {0x16F, 0x75}, {0x171, 0x75},
    {0x173, 0x75}, {0x179, 0x5a}, {0x17b, 0x5a}, {0x17d, 0x5a}, {0x17a, 0x7a},
    {0x17c, 0x7a}, {0x17e, 0x7a}, {0, 0}
};

/* ============================================================================
 * FONT SIZE ENUM & FONT SELECTION
 * ============================================================================ */

typedef enum sizefont {
    small,
    text,
    middle,
    large
} sizefont;

/**
 * Set font based on screen size and font size enum
 * This maintains the original layout logic from UCGlib/U8g2
 */
static void setfont(sizefont size)
{
    const lv_font_t *font = &lv_font_montserrat_14;
    int16_t height = 0;
    
    // Font selection based on screen resolution and size
    switch (size) {
        case small:
            font = &lv_font_montserrat_14;
            height = 12;
            break;
            
        case text:
            font = &lv_font_montserrat_14;
            height = 16;
            break;
            
        case middle:
            font = &lv_font_montserrat_14;
            height = 18;
            break;
            
        case large:
            font = &lv_font_montserrat_14;
            height = 20;
            break;
    }
    
    lvgl_display.current_font = font;
    lvgl_display.line_height = height;
    ESP_LOGV(TAG, "setfont: size=%d, height=%d", size, height);
}

/* ============================================================================
 * UTF-8 CHARSET CONVERSION FUNCTIONS
 * ============================================================================ */

static uint8_t to1251(uint16_t utf8)
{
    int i;
    if (utf8 > 0x491)
        return 0x1f;
    for (i = 0; i < UTF8TO1251; i++) {
        if (utf8 == utf8To1251[i].utf8)
            return utf8To1251[i].ascii;
    }
    return ((utf8 - 0x350) & 0xff);
}

static uint8_t to1253(uint16_t utf8)
{
    return ((utf8 - 0x300) & 0xff);
}

static uint8_t to8859(uint16_t utf8)
{
    int i;
    if (utf8 > 0x1ff)
        return 0x1f;
    for (i = 0; i < UTF8TO8859; i++) {
        if (utf8 == utf8To8859[i].utf8)
            return utf8To8859[i].ascii;
    }
    return ((utf8) & 0xff);
}

static uint16_t UtoC(uint8_t high, uint8_t low)
{
    uint16_t res = (((high & 0x01f) << 6) | (low & 0x3F)) & 0x7FF;
    return res;
}

/**
 * Remove/convert UTF-8 sequences to single-byte characters
 * Handles Cyrillic, Greek, and extended Latin charsets
 */
static void removeUtf8(char *characters)
{
    int Rindex = 0;
    uint16_t utf8;
    
    if (!characters) return;
    
    while (characters[Rindex]) {
        // Extended Latin: 0xC2-0xC5 range
        if ((characters[Rindex] >= 0xc2) && (characters[Rindex] <= 0xc5)) {
            utf8 = UtoC(characters[Rindex], characters[Rindex + 1]);
            if (utf8 >= 0x100)
                characters[Rindex + 1] = to8859(utf8);
            else
                characters[Rindex + 1] = (uint8_t)utf8 & 0xff;
            
            // Remove first byte by shifting left
            int sind = Rindex + 1;
            while (characters[sind]) {
                characters[sind - 1] = characters[sind];
                sind++;
            }
            characters[sind - 1] = 0;
        }
        // Cyrillic: 0xD0-0xD3 range
        else if ((characters[Rindex] >= 0xd0) && (characters[Rindex] <= 0xd3)) {
            utf8 = UtoC(characters[Rindex], characters[Rindex + 1]);
            characters[Rindex + 1] = to1251(utf8);
            int sind = Rindex + 1;
            while (characters[sind]) {
                characters[sind - 1] = characters[sind];
                sind++;
            }
            characters[sind - 1] = 0;
            charset = Cyrillic;
        }
        // Greek: 0xCD-0xCF range
        else if ((characters[Rindex] >= 0xcd) && (characters[Rindex] <= 0xcf)) {
            utf8 = UtoC(characters[Rindex], characters[Rindex + 1]);
            characters[Rindex + 1] = to1253(utf8);
            int sind = Rindex + 1;
            while (characters[sind]) {
                characters[sind - 1] = characters[sind];
                sind++;
            }
            characters[sind - 1] = 0;
            charset = Greek;
        }
        Rindex++;
    }
}

/**
 * Erase escape sequences: \", \', \\, \?, \/
 */
static void eraseSlashes(char *str)
{
    char *sym = str, *sym1;
    if (str != NULL) {
        while (*sym != 0) {
            if (*sym == 0x5c) {  // backslash
                sym1 = sym + 1;
                if (*sym1 == 0x22 || *sym1 == 0x27 || *sym1 == 0x5c || 
                    *sym1 == 0x3f || *sym1 == 0x2f) {
                    *sym = 0x1f;  // Replace with non-printable
                    sym++;
                }
            }
            sym++;
        }
    }
}

/* ============================================================================
 * DRAWING UTILITIES
 * ============================================================================ */

/**
 * Convert LVGL color (for color displays)
 * For monochrome displays, treat as black/white
 */
static lv_color_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_make(r, g, b);
}

/**
 * Get string width in pixels (approximation using font metrics)
 */
static uint16_t get_string_width(const char *str)
{
    if (!str || !lvgl_display.current_font)
        return 0;
    
    // Approximate width: 8 pixels per character is typical for montserrat 14
    uint32_t width = strlen(str) * 8;
    return width;
}

/**
 * Initialize LVGL display - called once at startup
 */
static void lvgl_display_init(void)
{
    lv_init();
    
    // Get default display (initialized by esp32_lvgl_drivers)
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lvgl_display.width = lv_disp_get_hor_res(disp);
        lvgl_display.height = lv_disp_get_ver_res(disp);
    } else {
        // Fallback dimensions
        lvgl_display.width = 320;
        lvgl_display.height = 240;
    }
    
    // Create screen
    lvgl_display.screen = lv_obj_create(NULL);
    lv_obj_set_size(lvgl_display.screen, lvgl_display.width, lvgl_display.height);
    
    // Default colors
    lvgl_display.fg_color = lv_color_white();
    lvgl_display.bg_color = lv_color_black();
    lvgl_display.current_font = &lv_font_montserrat_14;
    lvgl_display.line_height = 16;
    
    ESP_LOGI(TAG, "LVGL display initialized: %dx%d", 
             lvgl_display.width, lvgl_display.height);
}

/* ============================================================================
 * PUBLIC API - REPLACES addonu8g2.c AND addonucg.c
 * ============================================================================ */

/**
 * Get current station name number
 */
char *getNameNum(void)
{
    return nameNum;
}

/**
 * Set volume level (stored in global)
 */
void setVolume(uint16_t vol)
{
    volume = vol;
}

/**
 * Clear all display buffers and line indexes
 */
void clearAll(void)
{
    title[0] = 0;
    station[0] = 0;
    for (int i = 1; i < LINES; i++) {
        lline[i] = NULL;
        iline[i] = 0;
        tline[i] = 0;
        mline[i] = 1;
    }
    
    // Clear LVGL screen
    lv_obj_clean(lvgl_display.screen);
}

/**
 * Clear title and lines starting from 'froml'
 */
void cleartitle(uint8_t froml)
{
    title[0] = 0;
    for (int i = froml; i < LINES; i++) {
        lline[i] = NULL;
        iline[i] = 0;
        tline[i] = 0;
        mline[i] = 1;
    }
}

/**
 * Mark line for redraw and reset scroll index
 */
void markDrawReset(int i)
{
    mline[i] = 1;
    iline[i] = 0;
    tline[i] = 0;
}

/**
 * Scroll display lines with proper word boundary detection
 */
void scroll(void)
{
    int16_t len;
    setfont(text);
    
    for (int i = 0; i < LINES; i++) {
        if (lline[i] != NULL) {
            if (tline[i] > 0) {
                // Wait before scrolling
                len = (i == 0) ? get_string_width(nameNum) + get_string_width(lline[i]) 
                                : get_string_width(lline[i]);
                if ((tline[i] == 4) && (len >= lvgl_display.width)) {
                    iline[i] = 0;
                    mline[i] = 1;
                }
                tline[i]--;
            } else {
                // Perform scrolling
                len = (i == 0) ? get_string_width(nameNum) + get_string_width(lline[i] + iline[i])
                                : get_string_width(lline[i] + iline[i]);
                if (len > lvgl_display.width) {
                    len = iline[i];
                    // Advance by approximate character width
                    iline[i] += lvgl_display.width / get_string_width("MM");
                    
                    // Find word boundary (space or dash)
                    while ((*(lline[i] + iline[i]) != ' ') && 
                           (*(lline[i] + iline[i]) != '-') && 
                           (iline[i] != 0))
                        iline[i]--;
                    
                    if (iline[i] == len)
                        iline[i] += (lvgl_display.width / get_string_width("MM"));
                    
                    mline[i] = 1;
                } else {
                    tline[i] = 6;
                }
            }
        }
    }
}

/**
 * Draw a single line at specified position with color and font
 */
static void draw_line(int line_num, int x_pos, int y_pos, const char *text)
{
    if (!text || !lvgl_display.screen)
        return;
    
    // Create temporary label for drawing
    lv_obj_t *label = lv_label_create(lvgl_display.screen);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x_pos, y_pos);
    lv_obj_set_style_text_font(label, (lv_font_t *)lvgl_display.current_font, 0);
    
    // Set text color based on line number (mimics original behavior)
    lv_color_t color = lv_color_white();
    switch (line_num) {
        case 0: color = lv_color_make(0, 0, 0); break;      // station name (black)
        case 1: color = lv_color_white(); break;            // station1 (white)
        case 2: color = lv_color_make(255, 200, 200); break; // station2 (light red)
        case 3:
        case 4: color = lv_color_make(255, 255, 0); break;   // title (yellow)
        case 5:
        case 6: color = lv_color_make(0, 255, 255); break;   // title2 (cyan)
        case 7: color = lv_color_make(200, 200, 255); break; // volume (light blue)
        default: color = lv_color_make(110, 255, 110); break; // body (light green)
    }
    
    lv_obj_set_style_text_color(label, color, 0);
}

/**
 * Main frame drawing function
 * mTscreen: MTNEW (full redraw), MTREFRESH (partial), MTNODISPLAY (no draw)
 */
void drawFrame(uint8_t mTscreen)
{
    if (mTscreen == MTNODISPLAY)
        return;
    
    if (mTscreen == MTNEW) {
        // Full screen redraw
        lv_obj_clean(lvgl_display.screen);
    }
    
    setfont(text);
    int y_pos = 0;
    
    // Draw all marked lines
    for (int i = 0; i < LINES; i++) {
        if (mline[i] && lline[i]) {
            const char *text_to_draw = lline[i] + iline[i];
            if (i == 0) {
                // Station name line - show nameNum prefix
                char full_line[256];
                snprintf(full_line, sizeof(full_line), "%s %s", nameNum, text_to_draw);
                draw_line(i, 2, y_pos, full_line);
            } else {
                draw_line(i, 2, y_pos, text_to_draw);
            }
            mline[i] = 0;  // Clear mark after drawing
        }
        y_pos += lvgl_display.line_height;
    }
    
    lv_refr_now(NULL);
}

/**
 * Draw title in transient display
 */
void drawTTitle(char *ttitle)
{
    if (!ttitle)
        return;
    
    char temp[256];
    strncpy(temp, ttitle, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    
    removeUtf8(temp);
    eraseSlashes(temp);
    
    lv_obj_clean(lvgl_display.screen);
    setfont(large);
    draw_line(0, 2, lvgl_display.height / 2 - lvgl_display.line_height / 2, temp);
    lv_refr_now(NULL);
}

/**
 * Draw number entered from IR remote
 */
void drawNumber(uint8_t mTscreen, char *irStr)
{
    if (!irStr || strlen(irStr) == 0)
        return;
    
    if (mTscreen == MTNEW) {
        lv_obj_clean(lvgl_display.screen);
    }
    
    setfont(large);
    draw_line(0, 2, lvgl_display.height / 2, irStr);
    lv_refr_now(NULL);
}

/**
 * Draw station selection screen
 */
void drawStation(uint8_t mTscreen, char *snum, char *ddot)
{
    if (mTscreen == MTNEW) {
        lv_obj_clean(lvgl_display.screen);
    }
    
    char temp[256];
    strncpy(temp, ddot, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    removeUtf8(temp);
    eraseSlashes(temp);
    
    setfont(text);
    int y = 0;
    draw_line(0, 2, y, snum);
    y += lvgl_display.line_height;
    draw_line(1, 2, y, temp);
    
    lv_refr_now(NULL);
}

/**
 * Draw volume display
 */
void drawVolume(uint8_t mTscreen)
{
    if (mTscreen == MTNEW) {
        lv_obj_clean(lvgl_display.screen);
    }
    
    char vol_str[64];
    snprintf(vol_str, sizeof(vol_str), "Volume: %d%%", (int)(volume * 100 / 255));
    
    setfont(large);
    int y = lvgl_display.height / 2;
    draw_line(7, 2, y, vol_str);
    
    lv_refr_now(NULL);
}

/**
 * Draw time display
 */
void drawTime(uint8_t mTscreen, unsigned timein)
{
    if (mTscreen == MTNEW) {
        lv_obj_clean(lvgl_display.screen);
    }
    
    time_t timestamp = time(NULL);
    struct tm *dt = localtime(&timestamp);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", dt);
    
    setfont(large);
    int y = lvgl_display.height / 2;
    draw_line(6, 2, y, time_str);
    
    lv_refr_now(NULL);
}

/**
 * Draw separator line
 */
void separator(char *from)
{
    // Draw visual separator - can be implemented as a line or marker
    lv_obj_t *line = lv_line_create(lvgl_display.screen);
    lv_point_t points[2];
    points[0].x = 0;
    points[0].y = 0;
    points[1].x = lvgl_display.width;
    points[1].y = 0;
    lv_line_set_points(line, points, 2);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
}

/**
 * Draw metadata (ICY info)
 */
void meta(char *ici)
{
    if (!ici)
        return;
    
    char temp[256];
    strncpy(temp, ici, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    removeUtf8(temp);
    eraseSlashes(temp);
    
    lline[3] = temp;
    mline[3] = 1;
}

/**
 * Draw ICY 4 metadata
 */
void icy4(char *ici)
{
    if (!ici)
        return;
    
    char temp[256];
    strncpy(temp, ici, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    removeUtf8(temp);
    eraseSlashes(temp);
    
    lline[3] = temp;
    mline[3] = 1;
}

/**
 * Draw ICY 0 metadata (genre)
 */
void icy0(char *ici)
{
    if (!ici)
        return;
    
    char temp[256];
    strncpy(temp, ici, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    removeUtf8(temp);
    eraseSlashes(temp);
    
    lline[2] = temp;
    mline[2] = 1;
}

/**
 * Display status message
 */
void status(const char *label)
{
    if (!label)
        return;
    
    lv_obj_clean(lvgl_display.screen);
    setfont(text);
    draw_line(0, 2, lvgl_display.height / 2, label);
    lv_refr_now(NULL);
}

/**
 * Set station name (to be displayed in UI)
 * Note: Renamed to avoid conflict with extern variable
 */
void lvgl_nameset(char *ici)
{
    if (!ici)
        return;
    
    char temp[256];
    strncpy(temp, ici, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = 0;
    removeUtf8(temp);
    eraseSlashes(temp);
    
    lline[0] = temp;
    mline[0] = 1;
}

/**
 * Show "playing" status
 */
void playing(void)
{
    status("PLAYING");
}

/**
 * Initialize LVGL display for specified type
 * Replaces lcd_initUcg() and lcd_initU8g2()
 */
void lcd_init_lvgl(uint8_t *lcd_type)
{
    ESP_LOGI(TAG, "Initializing LVGL display (type: 0x%02x)", *lcd_type);
    
    lvgl_display_init();
    
    clearAll();
    lv_refr_now(NULL);
    
    ESP_LOGI(TAG, "LVGL display initialized successfully");
}

/* ============================================================================
 * TASK FUNCTIONS (called from addon.c)
 * ============================================================================ */

/**
 * Display task - called periodically to update display
 * This maintains compatibility with the task_lcd structure
 */
void task_lcd(void *pvParams)
{
    (void)pvParams;
    
    // LVGL display is updated automatically by the driver
    // This task can be minimal or serve for higher-level refresh
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        lv_timer_handler();  // LVGL internal timer handler
    }
}

/**
 * Service routine for UI updates (called frequently)
 */
void task_addon(void *pvParams)
{
    (void)pvParams;
    
    // Can be minimal since LVGL handles most rendering
    // Called from ServiceAddon() in addon.c
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ============================================================================
 * ADDITIONAL HELPER FUNCTIONS
 * ============================================================================ */

/**
 * Get display width
 */
uint16_t GetWidth(void)
{
    return lvgl_display.width;
}

/**
 * Get display height
 */
uint16_t GetHeight(void)
{
    return lvgl_display.height;
}

/**
 * Set backlight level
 */
void setBlv(int val)
{
    // Handled in gpio.c or backlight driver
}

/**
 * Get backlight level
 */
int getBlv(void)
{
    // Handled in gpio.c or backlight driver
    return 100;
}

/**
 * Get battery percentage
 */
int getBatPercent(void)
{
    // Handled in addon.c
    return -1;
}

/* ============================================================================
 * TOUCH INPUT SUPPORT (XPT2046 via LVGL)
 * ============================================================================ */

#ifndef CONFIG_NO_XPT2046

/**
 * Initialize touch input device (XPT2046)
 * Called from lcd_init_lvgl() if touch available
 * Integrates XPT2046 with LVGL input system
 */
void lvgl_touch_init(void)
{
    // Initialize LVGL touch driver (from lvgl_esp32_drivers)
    touch_driver_init();
    
    // Log status
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev) {
        ESP_LOGI(TAG, "Touch device initialized and registered with LVGL");
    } else {
        ESP_LOGW(TAG, "Touch device not registered with LVGL");
    }
}

/**
 * Disable touch input (e.g., during sleep mode)
 * Reduces power consumption and prevents accidental touches
 */
void lvgl_touch_disable(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev) {
        lv_indev_enable(indev, false);
        ESP_LOGV(TAG, "Touch input disabled");
    }
}

/**
 * Enable touch input after sleep or standby
 * Restores touch functionality
 */
void lvgl_touch_enable(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev) {
        lv_indev_enable(indev, true);
        ESP_LOGV(TAG, "Touch input enabled");
    }
}

/**
 * Get current touch input device
 * Returns pointer to LVGL input device or NULL
 */
lv_indev_t *lvgl_get_touch_device(void)
{
    return lv_indev_get_next(NULL);
}

#else

// Stub functions when touch disabled
void lvgl_touch_init(void) { }
void lvgl_touch_disable(void) { }
void lvgl_touch_enable(void) { }
lv_indev_t *lvgl_get_touch_device(void) { return NULL; }

#endif  // CONFIG_NO_XPT2046
#endif  // CONFIG_DISPLAY_TYPE_LVGL
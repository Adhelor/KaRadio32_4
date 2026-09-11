/*
 * Copyright 2017 jp Cocatrix (http://www.karawin.fr)
 */
#ifndef OTA_H
#define OTA_H
#include "esp_err.h"
#include <stdbool.h>

// OTA API for direct buffer upload (creates internal task)
esp_err_t ota_update_from_socket_start(void);
esp_err_t ota_update_from_socket_write(const uint8_t *data, size_t len);
esp_err_t ota_update_from_socket_finish(void);
// Check if OTA update is currently running
bool ota_is_running(void);

// OTA status websocket feedback
void wsUpgrade(const char *str, int count, int total);

// Optional: tell OTA expected total size so a percent can be computed
void ota_set_expected_size(size_t size);

// OTA PIN helpers - generate a one-time PIN (displayed on device), validate and clear
// generate PIN and show on device for given seconds; PIN is stored server-side only
void ota_generate_pin(int seconds);
// validate and consume the stored PIN; returns true if valid
bool ota_check_pin(const char *pin);
// clear stored PIN
void ota_clear_pin(void);

#endif
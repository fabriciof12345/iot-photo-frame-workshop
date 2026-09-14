#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PHOTO_STORAGE_FRAME_SIZE (800 * 480 * 2)

esp_err_t photo_storage_load(uint8_t *frame, size_t frame_size);
esp_err_t photo_storage_save(const uint8_t *frame, size_t frame_size);

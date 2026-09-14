#include "photo_storage.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

#define PHOTO_PARTITION_LABEL "photo"
#define PHOTO_PARTITION_SUBTYPE 0x40
#define PHOTO_MAGIC 0x544F4850U
#define PHOTO_FORMAT_VERSION 1U
#define PHOTO_FRAME_OFFSET 0x1000U
#define FLASH_SECTOR_SIZE 0x1000U
#define COPY_CHUNK_SIZE 0x1000U
#define PHOTO_ERASE_SIZE \
    (((PHOTO_FRAME_OFFSET + PHOTO_STORAGE_FRAME_SIZE + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t frame_size;
    uint8_t sha256[32];
    uint8_t reserved[20];
} photo_header_t;

_Static_assert(sizeof(photo_header_t) == 64, "Persistent photo header must remain 64 bytes");

static const char *TAG = "photo_storage";

static const esp_partition_t *find_photo_partition(void)
{
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)PHOTO_PARTITION_SUBTYPE,
        PHOTO_PARTITION_LABEL
    );
}

static esp_err_t calculate_sha256(const uint8_t *frame, uint8_t digest[32])
{
    int result = mbedtls_sha256(frame, PHOTO_STORAGE_FRAME_SIZE, digest, 0);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_frame_chunks(const esp_partition_t *partition, const uint8_t *frame)
{
    uint8_t *scratch = heap_caps_malloc(COPY_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(scratch != NULL, ESP_ERR_NO_MEM, TAG, "allocate flash write buffer");

    esp_err_t result = ESP_OK;
    for (size_t offset = 0; offset < PHOTO_STORAGE_FRAME_SIZE; offset += COPY_CHUNK_SIZE) {
        size_t length = PHOTO_STORAGE_FRAME_SIZE - offset;
        if (length > COPY_CHUNK_SIZE) {
            length = COPY_CHUNK_SIZE;
        }
        memcpy(scratch, frame + offset, length);
        result = esp_partition_write(partition, PHOTO_FRAME_OFFSET + offset, scratch, length);
        if (result != ESP_OK) {
            break;
        }
    }

    free(scratch);
    return result;
}

static esp_err_t read_frame_chunks(const esp_partition_t *partition, uint8_t *frame)
{
    uint8_t *scratch = heap_caps_malloc(COPY_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(scratch != NULL, ESP_ERR_NO_MEM, TAG, "allocate flash read buffer");

    esp_err_t result = ESP_OK;
    for (size_t offset = 0; offset < PHOTO_STORAGE_FRAME_SIZE; offset += COPY_CHUNK_SIZE) {
        size_t length = PHOTO_STORAGE_FRAME_SIZE - offset;
        if (length > COPY_CHUNK_SIZE) {
            length = COPY_CHUNK_SIZE;
        }
        result = esp_partition_read(partition, PHOTO_FRAME_OFFSET + offset, scratch, length);
        if (result != ESP_OK) {
            break;
        }
        memcpy(frame + offset, scratch, length);
    }

    free(scratch);
    return result;
}

esp_err_t photo_storage_save(const uint8_t *frame, size_t frame_size)
{
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is null");
    ESP_RETURN_ON_FALSE(frame_size == PHOTO_STORAGE_FRAME_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "frame is %u bytes; expected %u", (unsigned)frame_size,
                        (unsigned)PHOTO_STORAGE_FRAME_SIZE);

    const esp_partition_t *partition = find_photo_partition();
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "photo partition not found");
    ESP_RETURN_ON_FALSE(partition->size >= PHOTO_ERASE_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "photo partition is too small");

    photo_header_t header = {
        .magic = PHOTO_MAGIC,
        .format_version = PHOTO_FORMAT_VERSION,
        .header_size = sizeof(photo_header_t),
        .frame_size = PHOTO_STORAGE_FRAME_SIZE,
    };
    ESP_RETURN_ON_ERROR(calculate_sha256(frame, header.sha256), TAG, "hash cloud frame");

    ESP_RETURN_ON_ERROR(esp_partition_erase_range(partition, 0, PHOTO_ERASE_SIZE), TAG,
                        "erase persisted photo partition");
    ESP_RETURN_ON_ERROR(write_frame_chunks(partition, frame), TAG, "write persisted cloud frame");

    // Commit the validity header last so interrupted writes are never treated as valid.
    ESP_RETURN_ON_ERROR(esp_partition_write(partition, 0, &header, sizeof(header)), TAG,
                        "commit persisted photo header");
    ESP_LOGI(TAG, "Persisted cloud frame to flash");
    return ESP_OK;
}

esp_err_t photo_storage_load(uint8_t *frame, size_t frame_size)
{
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is null");
    ESP_RETURN_ON_FALSE(frame_size == PHOTO_STORAGE_FRAME_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "frame buffer has invalid size");

    const esp_partition_t *partition = find_photo_partition();
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "photo partition not found");

    photo_header_t header;
    ESP_RETURN_ON_ERROR(esp_partition_read(partition, 0, &header, sizeof(header)), TAG,
                        "read persisted photo header");
    if (header.magic != PHOTO_MAGIC ||
        header.format_version != PHOTO_FORMAT_VERSION ||
        header.header_size != sizeof(photo_header_t) ||
        header.frame_size != PHOTO_STORAGE_FRAME_SIZE) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(read_frame_chunks(partition, frame), TAG, "read persisted cloud frame");

    uint8_t digest[32];
    ESP_RETURN_ON_ERROR(calculate_sha256(frame, digest), TAG, "hash persisted cloud frame");
    ESP_RETURN_ON_FALSE(memcmp(digest, header.sha256, sizeof(digest)) == 0,
                        ESP_ERR_INVALID_CRC, TAG, "persisted cloud frame checksum mismatch");

    ESP_LOGI(TAG, "Loaded persisted cloud frame from flash");
    return ESP_OK;
}

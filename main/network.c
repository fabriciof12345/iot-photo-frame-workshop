#include "network.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "photo_storage.h"
#include "sdkconfig.h"

#define LCD_WIDTH 800
#define LCD_HEIGHT 480
#define FRAME_SIZE (LCD_WIDTH * LCD_HEIGHT * 2)
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRIES 10
#define DOWNLOAD_TASK_STACK_SIZE 8192

#define THING_NAME "<DEVICE_NAME>"
#define IOT_ENDPOINT "<IOT_ENDPOINT>"
#define MQTT_URI "mqtts://" IOT_ENDPOINT ":8883"
#define IMAGE_READY_TOPIC "photo-frame/" THING_NAME "/image-ready"
#define STATUS_TOPIC "photo-frame/" THING_NAME "/status"

extern const uint8_t root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t device_certificate_start[] asm("_binary_device_certificate_pem_crt_start");
extern const uint8_t device_private_key_start[] asm("_binary_device_private_key_start");

static const char *TAG = "frame_network";
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_download_mutex;
static esp_mqtt_client_handle_t s_mqtt_client;
static esp_lcd_panel_handle_t s_panel;
static int s_wifi_retry_count;
static char *s_incoming_payload;
static size_t s_incoming_payload_size;

static void publish_status(const char *status, const char *version, const char *detail)
{
    if (s_mqtt_client == NULL) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "status", status);
    if (version != NULL) {
        cJSON_AddStringToObject(root, "version", version);
    }
    if (detail != NULL) {
        cJSON_AddStringToObject(root, "detail", detail);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != NULL) {
        esp_mqtt_client_publish(s_mqtt_client, STATUS_TOPIC, payload, 0, 1, 0);
        free(payload);
    }
}

static esp_err_t download_frame(const char *url, const char *expected_sha256, uint8_t *frame)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "initialize HTTPS client");

    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open frame URL: %s", esp_err_to_name(result));
        esp_http_client_cleanup(client);
        return result;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Frame download returned HTTP %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length >= 0 && content_length != FRAME_SIZE) {
        ESP_LOGE(TAG, "Frame length is %" PRId64 " bytes; expected %d", content_length, FRAME_SIZE);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    while (total < FRAME_SIZE) {
        int received = esp_http_client_read(client, (char *)frame + total, FRAME_SIZE - total);
        if (received < 0) {
            ESP_LOGE(TAG, "HTTPS read failed after %u bytes", (unsigned)total);
            result = ESP_FAIL;
            break;
        }
        if (received == 0) {
            break;
        }
        total += (size_t)received;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        return result;
    }
    ESP_RETURN_ON_FALSE(total == FRAME_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "Downloaded %u bytes; expected %d", (unsigned)total, FRAME_SIZE);

    unsigned char digest[32];
    result = mbedtls_sha256(frame, FRAME_SIZE, digest, 0);
    ESP_RETURN_ON_ERROR(result, TAG, "calculate frame SHA-256");

    char digest_hex[65];
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(digest_hex + (index * 2), 3, "%02x", digest[index]);
    }
    digest_hex[64] = '\0';
    ESP_RETURN_ON_FALSE(strcasecmp(digest_hex, expected_sha256) == 0, ESP_ERR_INVALID_CRC, TAG,
                        "Frame SHA-256 mismatch");

    return ESP_OK;
}

static void image_download_task(void *argument)
{
    char *payload = argument;
    char version[80] = "unknown";
    esp_err_t result = ESP_FAIL;

    cJSON *root = cJSON_Parse(payload);
    free(payload);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid image-ready JSON");
        publish_status("error", version, "invalid-json");
        goto done;
    }

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *width = cJSON_GetObjectItemCaseSensitive(root, "width");
    const cJSON *height = cJSON_GetObjectItemCaseSensitive(root, "height");
    const cJSON *message_version = cJSON_GetObjectItemCaseSensitive(root, "version");

    if (cJSON_IsString(message_version)) {
        strlcpy(version, message_version->valuestring, sizeof(version));
    }
    if (!cJSON_IsString(url) || !cJSON_IsString(sha256) ||
        !cJSON_IsNumber(size) || size->valueint != FRAME_SIZE ||
        !cJSON_IsNumber(width) || width->valueint != LCD_WIDTH ||
        !cJSON_IsNumber(height) || height->valueint != LCD_HEIGHT) {
        ESP_LOGE(TAG, "Image-ready message has invalid frame metadata");
        publish_status("error", version, "invalid-metadata");
        cJSON_Delete(root);
        goto done;
    }

    uint8_t *frame = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Could not allocate %d-byte frame buffer", FRAME_SIZE);
        publish_status("error", version, "out-of-memory");
        cJSON_Delete(root);
        goto done;
    }

    ESP_LOGI(TAG, "Downloading frame version %s", version);
    publish_status("downloading", version, NULL);
    result = download_frame(url->valuestring, sha256->valuestring, frame);
    if (result == ESP_OK) {
        result = photo_storage_save(frame, FRAME_SIZE);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, frame);
    }
    free(frame);
    cJSON_Delete(root);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Cloud photo displayed, version %s", version);
        publish_status("displayed", version, NULL);
    } else {
        ESP_LOGE(TAG, "Cloud photo update failed: %s", esp_err_to_name(result));
        publish_status("error", version, esp_err_to_name(result));
    }

done:
    xSemaphoreGive(s_download_mutex);
    vTaskDelete(NULL);
}

static void dispatch_complete_payload(char *payload)
{
    if (xSemaphoreTake(s_download_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Ignoring image update while another download is active");
        free(payload);
        return;
    }

    if (xTaskCreate(image_download_task, "image_download", DOWNLOAD_TASK_STACK_SIZE,
                    payload, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create image download task");
        xSemaphoreGive(s_download_mutex);
        free(payload);
    }
}

static void handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        if (event->topic_len != (int)strlen(IMAGE_READY_TOPIC) ||
            memcmp(event->topic, IMAGE_READY_TOPIC, event->topic_len) != 0) {
            return;
        }

        ESP_LOGI(TAG, "Received image-ready payload (%d bytes)", event->total_data_len);
        free(s_incoming_payload);
        s_incoming_payload = calloc((size_t)event->total_data_len + 1, 1);
        s_incoming_payload_size = (size_t)event->total_data_len;
    } else if (s_incoming_payload == NULL) {
        return;
    }

    if (s_incoming_payload == NULL ||
        (size_t)event->current_data_offset + event->data_len > s_incoming_payload_size) {
        free(s_incoming_payload);
        s_incoming_payload = NULL;
        s_incoming_payload_size = 0;
        return;
    }

    memcpy(s_incoming_payload + event->current_data_offset, event->data, event->data_len);
    if (event->current_data_offset + event->data_len == event->total_data_len) {
        char *complete_payload = s_incoming_payload;
        s_incoming_payload = NULL;
        s_incoming_payload_size = 0;
        dispatch_complete_payload(complete_payload);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to AWS IoT Core");
        esp_mqtt_client_subscribe(event->client, IMAGE_READY_TOPIC, 1);
        publish_status("online", NULL, NULL);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed to %s", IMAGE_READY_TOPIC);
        break;
    case MQTT_EVENT_DATA:
        handle_mqtt_data(event);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from AWS IoT Core");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "AWS IoT MQTT error");
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAXIMUM_RETRIES) {
            ++s_wifi_retry_count;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    ESP_RETURN_ON_FALSE(strlen(CONFIG_PHOTO_FRAME_WIFI_SSID) > 0, ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi SSID is empty; configure IoT Photo Frame in menuconfig");

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create Wi-Fi event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize network interfaces");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create default event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG, "create Wi-Fi station");

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                        TAG, "register IP event handler");

    wifi_config_t configuration = {0};
    strlcpy((char *)configuration.sta.ssid, CONFIG_PHOTO_FRAME_WIFI_SSID, sizeof(configuration.sta.ssid));
    strlcpy((char *)configuration.sta.password, CONFIG_PHOTO_FRAME_WIFI_PASSWORD,
            sizeof(configuration.sta.password));
    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &configuration), TAG, "set Wi-Fi credentials");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    ESP_RETURN_ON_FALSE(bits & WIFI_CONNECTED_BIT, ESP_ERR_TIMEOUT, TAG,
                        "Could not connect to Wi-Fi SSID %s", CONFIG_PHOTO_FRAME_WIFI_SSID);
    return ESP_OK;
}

static esp_err_t sync_clock(void)
{
    esp_sntp_config_t configuration = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&configuration), TAG, "initialize SNTP");

    for (int attempt = 1; attempt <= 10; ++attempt) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) == ESP_OK) {
            ESP_LOGI(TAG, "System clock synchronized");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Waiting for network time (%d/10)", attempt);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_mqtt(void)
{
    const esp_mqtt_client_config_t configuration = {
        .broker = {
            .address.uri = MQTT_URI,
            .verification.certificate = (const char *)root_ca_start,
        },
        .credentials = {
            .client_id = THING_NAME,
            .authentication = {
                .certificate = (const char *)device_certificate_start,
                .key = (const char *)device_private_key_start,
            },
        },
        .session = {
            .keepalive = 60,
        },
        .network = {
            .timeout_ms = 15000,
            .reconnect_timeout_ms = 5000,
        },
        .buffer = {
            .size = 4096,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&configuration);
    ESP_RETURN_ON_FALSE(s_mqtt_client != NULL, ESP_ERR_NO_MEM, TAG, "initialize MQTT client");
    ESP_RETURN_ON_ERROR(
        esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL),
        TAG,
        "register MQTT handler"
    );
    return esp_mqtt_client_start(s_mqtt_client);
}

esp_err_t photo_frame_network_start(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    s_download_mutex = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_download_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create download semaphore");
    ESP_RETURN_ON_FALSE(xSemaphoreGive(s_download_mutex) == pdTRUE, ESP_FAIL, TAG,
                        "initialize download semaphore");

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        result = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(result, TAG, "initialize NVS");
    ESP_RETURN_ON_ERROR(connect_wifi(), TAG, "connect Wi-Fi");
    ESP_RETURN_ON_ERROR(sync_clock(), TAG, "synchronize clock");
    ESP_RETURN_ON_ERROR(start_mqtt(), TAG, "start AWS IoT MQTT");

    ESP_LOGI(TAG, "Cloud photo updates enabled");
    return ESP_OK;
}

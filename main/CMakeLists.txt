idf_component_register(
    SRCS "main.c" "network.c" "photo_storage.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_event
        esp_http_client
        esp_lcd
        esp_netif
        esp_partition
        esp_wifi
        json
        mbedtls
        mqtt
        nvs_flash
    EMBED_FILES "sze.rgb565"
    EMBED_TXTFILES
        "certs/AmazonRootCA1.pem"
        "certs/device-certificate.pem.crt"
        "certs/device-private.key"
)

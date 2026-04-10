idf_component_register(
    SRCS "log.c" "nvs.c" "effects.c" "nixies.c" "gpio.c" "leds.c" "clock.c" "ota.c" "mqtt.c" "main.c"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_driver_gpio nvs_flash esp_http_client esp_https_ota app_update lwip esp_ringbuf
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../ssl/ca_cert.pem" "../ssl/letsencrypt_cert.pem"
)

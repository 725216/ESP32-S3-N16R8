idf_component_register(
    SRCS "brookesia.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi nvs_flash httpd freertos driver
)

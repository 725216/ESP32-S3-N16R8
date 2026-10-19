idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio driver_i2s nvs_flash esp_wifi esp_http_client)

idf_component_register(SRCS "app_main.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES brookesia driver led_strip gpio freertos nvs_flash esp_wifi esp_event esp_http_server)

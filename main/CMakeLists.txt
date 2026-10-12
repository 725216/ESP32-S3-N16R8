idf_component_register(SRCS "app_main.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event driver esp_http_server)

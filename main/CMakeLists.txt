idf_component_register(SRCS "app_main.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_http_client esp_http_server nvs_flash esp_wifi esp_event esp_netif esp_driver_i2s esp_driver_gpio)

if(DEFINED ENV{COZE_TOKEN})
    add_compile_definitions(COZE_TOKEN=\"$ENV{COZE_TOKEN}\")
endif()
if(DEFINED ENV{DOUBAO_APIKEY})
    add_compile_definitions(DOUBAO_APIKEY=\"$ENV{DOUBAO_APIKEY}\")
endif()

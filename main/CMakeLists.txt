idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_http_client nvs_flash esp_wifi esp_event driver esp_driver_i2s gpio)

if(DEFINED ENV{COZE_TOKEN})
    add_compile_definitions(COZE_TOKEN=\"$ENV{COZE_TOKEN}\")
endif()
if(DEFINED ENV{DOUBAO_APIKEY})
    add_compile_definitions(DOUBAO_APIKEY=\"$ENV{DOUBAO_APIKEY}\")
endif()

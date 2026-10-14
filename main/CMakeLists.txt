idf_component_register(SRCS "app_main.c"
                    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart esp_driver_ledc esp_driver_rmt i2s_audio brookesia brookesia_agent_manager brookesia_service_audio brookesia_service_wifi
                    INCLUDE_DIRS ".")

idf_component_register(SRCS "app_main.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_event nvs_flash driver esp_driver_i2s mbedtls)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers -Wno-unused-function)
